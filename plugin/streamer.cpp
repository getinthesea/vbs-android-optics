// Streams VBS's own window to the phone, from a background thread inside the plugin.
//   Desktop Duplication on the GPU that shows VBS -> crop + scale + BGRA->NV12 on that GPU ->
//   its hardware H.264 encoder (no B-frames) -> UDP packets (protocol.h) to the phone.
// The plugin tells it where the phone is, which pose the view is aimed with and the field of view.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <wrl/client.h>
#include <initguid.h>
#include <codecapi.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "protocol.h"
#include "streamer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ws2_32.lib")

using Microsoft::WRL::ComPtr;

static std::atomic<bool> running{ false };
static std::thread worker;

// Set by the plugin every simulation step
static std::atomic<uint32_t> want_ip{ 0 };
static std::atomic<uint32_t> want_seq{ 0 };
static std::atomic<float> want_tan_x{ 0 };

// Latest status line for pluginFunction "status"
static std::mutex status_mutex;
static std::string status_text = "not started";

static void log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        status_text = buf;
    }
    OutputDebugStringA("[VBSAndroidOptics] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static double now_ms()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / freq.QuadPart;
}

// ---------------------------------------------------------------------------------------------
// Finding our window (the VBS process we are loaded in)

static int quality_vs_speed = -1; // encoder default
static DWORD poll_sleep_ms = 0;
static int low_latency = -1;      // -1 = off for Intel (its MFT then takes ~25 ms a frame), on otherwise
static UINT32 rate_control = eAVEncCommonRateControlMode_CBR;

static HWND find_vbs_window()
{
    struct Search { HWND found = nullptr; LONG area = 0; } search;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        RECT r;
        GetClientRect(hwnd, &r);
        auto s = (Search*)param;
        if (r.right > 64 && r.bottom > 64 && r.right * r.bottom > s->area) {
            s->found = hwnd;
            s->area = r.right * r.bottom;
        }
        return TRUE;
    }, (LPARAM)&search);
    return search.found;
}
static RECT client_rect_on_screen(HWND hwnd)
{
    RECT r;
    GetClientRect(hwnd, &r);
    POINT tl{ r.left, r.top }, br{ r.right, r.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    return RECT{ tl.x, tl.y, br.x, br.y };
}

// ---------------------------------------------------------------------------------------------
// Hardware H.264 encoder (Media Foundation, asynchronous MFT on our D3D11 device)

struct Encoder {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<ICodecAPI> codec;
    ComPtr<IMFDXGIDeviceManager> manager;
    bool provides_samples = true;
    DWORD output_size = 0;
    int need_input = 0;
    UINT fps = 60;
    std::string name;
    // Timing, reset by the stats printout
    double stat_wait_in = 0, stat_wait_out = 0, stat_process_in = 0, stat_delay = 0;
    int stat_outputs = 0;
    LONGLONG last_input_time = 0;
    int stat_early = 0, stat_missed = 0;

    bool init(ID3D11Device* device, LUID luid, UINT width, UINT height, UINT frame_rate, UINT bitrate)
    {
        fps = frame_rate;
        MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &activates, &count);
        if (count == 0) {
            log("No hardware H.264 encoder found");
            return false;
        }
        // Prefer the encoder on the GPU we capture from, so frames never cross between GPUs;
        // fall back to the others, since some (e.g. NVENC on a laptop's non-display GPU) refuse to start
        std::vector<UINT32> order;
        for (UINT32 i = 0; i < count; i++) {
            LUID l{};
            bool same_gpu = SUCCEEDED(activates[i]->GetBlob(MFT_ENUM_ADAPTER_LUID, (UINT8*)&l, sizeof(l), nullptr)) &&
                l.LowPart == luid.LowPart && l.HighPart == luid.HighPart;
            if (same_gpu) order.insert(order.begin(), i);
            else order.push_back(i);
        }
        HRESULT hr = E_FAIL;
        for (UINT32 i : order) {
            WCHAR* friendly = nullptr;
            UINT32 len = 0;
            std::string candidate = "?";
            if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
                char buf[256];
                WideCharToMultiByte(CP_UTF8, 0, friendly, -1, buf, sizeof(buf), nullptr, nullptr);
                candidate = buf;
                CoTaskMemFree(friendly);
            }
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(&mft));
            if (SUCCEEDED(hr)) {
                name = candidate;
                if (low_latency < 0) low_latency = candidate.find("Intel") == std::string::npos ? 1 : 0;
                break;
            }
            log("Encoder %s would not start (0x%08X), trying the next", candidate.c_str(), hr);
        }
        for (UINT32 i = 0; i < count; i++) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr)) { log("No hardware H.264 encoder would start"); return false; }

        ComPtr<IMFAttributes> attrs;
        mft->GetAttributes(&attrs);
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (low_latency) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

        UINT token = 0;
        MFCreateDXGIDeviceManager(&token, &manager);
        manager->ResetDevice(device, token);
        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager.Get());
        if (FAILED(hr)) { log("Encoder rejected the D3D11 device 0x%08X", hr); return false; }

        mft.As(&codec);
        mft.As(&events);
        set_codec(CODECAPI_AVEncCommonRateControlMode, rate_control);
        set_codec(CODECAPI_AVEncCommonMeanBitRate, bitrate);
        if (low_latency) set_codec(CODECAPI_AVLowLatencyMode, 1, VT_BOOL);
        set_codec(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        set_codec(CODECAPI_AVEncMPVGOPSize, frame_rate * 2);
        if (quality_vs_speed >= 0) set_codec(CODECAPI_AVEncCommonQualityVsSpeed, quality_vs_speed);

        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, frame_rate, 1);
        MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base); // No reordering for the decoder to wait on
        hr = mft->SetOutputType(0, out_type.Get(), 0);
        if (FAILED(hr)) { log("Encoder output type failed 0x%08X", hr); return false; }

        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, frame_rate, 1);
        MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = mft->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) { log("Encoder input type failed 0x%08X", hr); return false; }

        MFT_OUTPUT_STREAM_INFO info{};
        mft->GetOutputStreamInfo(0, &info);
        provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        output_size = info.cbSize ? info.cbSize : width * height;

        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void set_codec(const GUID& key, UINT32 value, VARTYPE type = VT_UI4)
    {
        if (!codec) return;
        VARIANT v;
        VariantInit(&v);
        v.vt = type;
        if (type == VT_BOOL) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        else v.ulVal = value;
        codec->SetValue(&key, &v);
    }

    // Handles encoder events until got_output or the timeout passes
    bool pump(double timeout_ms, std::vector<uint8_t>& out, bool& keyframe, bool& got_output)
    {
        double end = now_ms() + timeout_ms;
        while (now_ms() < end) {
            ComPtr<IMFMediaEvent> ev;
            HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) { Sleep(poll_sleep_ms); continue; }
            if (FAILED(hr)) return false;
            MediaEventType type;
            ev->GetType(&type);
            if (type == METransformNeedInput) need_input++;
            else if (type == METransformHaveOutput) {
                if (!read_output(out, keyframe)) return false;
                got_output = true;
                return true;
            }
            if (need_input > 0 && !got_output && timeout_ms == 0) return true;
        }
        return true;
    }

    bool read_output(std::vector<uint8_t>& out, bool& keyframe)
    {
        MFT_OUTPUT_DATA_BUFFER db{};
        db.dwStreamID = 0;
        ComPtr<IMFSample> own;
        if (!provides_samples) {
            ComPtr<IMFMediaBuffer> buf;
            MFCreateMemoryBuffer(output_size, &buf);
            MFCreateSample(&own);
            own->AddBuffer(buf.Get());
            db.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft->ProcessOutput(0, 1, &db, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &t))) mft->SetOutputType(0, t.Get(), 0);
            if (db.pEvents) db.pEvents->Release();
            return true;
        }
        if (FAILED(hr)) { log("Encoder output failed 0x%08X", hr); return false; }
        IMFSample* sample = db.pSample;
        ComPtr<IMFMediaBuffer> contiguous;
        if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&contiguous))) {
            BYTE* data = nullptr;
            DWORD length = 0;
            contiguous->Lock(&data, nullptr, &length);
            out.insert(out.end(), data, data + length);
            contiguous->Unlock();
            UINT32 clean = 0;
            keyframe = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;
            LONGLONG t = 0;
            if (SUCCEEDED(sample->GetSampleTime(&t))) { stat_delay += (last_input_time - t) / 10000.0; stat_outputs++; }
        }
        if (provides_samples && db.pSample) db.pSample->Release();
        if (db.pEvents) db.pEvents->Release();
        return true;
    }

    // Encodes one NV12 frame; out receives the Annex-B access unit (may be empty while the encoder warms up)
    bool encode(ID3D11Texture2D* nv12, LONGLONG time_100ns, bool force_key, std::vector<uint8_t>& out, bool& keyframe)
    {
        double t_call = now_ms();
        out.clear();
        keyframe = false;
        bool got = false;
        double wait_until = now_ms() + 200;
        while (need_input == 0) {
            if (now_ms() > wait_until) { log("Encoder stopped asking for input"); return false; }
            if (!pump(5, out, keyframe, got)) return false;
        }
        if (force_key) set_codec(CODECAPI_AVEncVideoForceKeyFrame, 1);
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer))) return false;
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(time_100ns);
        last_input_time = time_100ns;
        sample->SetSampleDuration(10000000 / fps);
        double t_in = now_ms();
        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) { log("Encoder input failed 0x%08X", hr); return false; }
        need_input--;
        stat_process_in += now_ms() - t_in;
        stat_wait_in += t_in - t_call;
        if (got) stat_early++;
        double t_out = now_ms();
        if (!got) pump(50, out, keyframe, got);
        stat_wait_out += now_ms() - t_out;
        if (!got) stat_missed++;
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// One streaming session: lasts until the VBS window changes size, VBS closes or the desktop is lost

struct Options {
    UINT height = 720;
    UINT fps = 60;
    UINT mbps = 12;
};

static bool run_session(const Options& opt, SOCKET sock)
{
    HWND vbs = find_vbs_window();
    if (!vbs) return false;
    RECT src = client_rect_on_screen(vbs);
    int src_w = src.right - src.left, src_h = src.bottom - src.top;

    // The GPU and output (monitor) showing VBS
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    HMONITOR monitor = MonitorFromWindow(vbs, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC out_desc{};
    for (UINT a = 0; !output; a++) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; o++) {
            ComPtr<IDXGIOutput> op;
            if (ad->EnumOutputs(o, &op) == DXGI_ERROR_NOT_FOUND) break;
            op->GetDesc(&out_desc);
            if (out_desc.Monitor == monitor) {
                op.As(&output);
                adapter = ad;
                break;
            }
        }
    }
    if (!output) { log("Could not find the monitor showing VBS"); return false; }
    DXGI_ADAPTER_DESC1 ad_desc;
    adapter->GetDesc1(&ad_desc);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) { log("D3D11 device failed 0x%08X", hr); return false; }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIOutputDuplication> dupl;
    hr = output->DuplicateOutput(device.Get(), &dupl);
    if (FAILED(hr)) { log("Desktop duplication failed 0x%08X", hr); return false; }

    // Stream size: opt.height tall, VBS's shape, multiple of 16
    UINT out_h = opt.height;
    UINT out_w = (UINT)((double)out_h * src_w / src_h / 16.0 + 0.5) * 16;

    // Our copy of the VBS region (desktop duplication textures can't be used by the video processor directly)
    D3D11_TEXTURE2D_DESC td{};
    td.Width = src_w; td.Height = src_h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> bgra;
    device->CreateTexture2D(&td, nullptr, &bgra);

    // A few NV12 frames, since the encoder may still be reading one while we fill the next
    td.Width = out_w; td.Height = out_h; td.Format = DXGI_FORMAT_NV12; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    const int RING = 4;
    ComPtr<ID3D11Texture2D> nv12[RING];
    // Flagged for the encoder so it can read them in place instead of copying each frame
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
    for (auto& t : nv12) {
        hr = device->CreateTexture2D(&td, nullptr, &t);
        if (FAILED(hr)) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = device->CreateTexture2D(&td, nullptr, &t);
        }
        if (FAILED(hr)) { log("NV12 texture failed 0x%08X", hr); return false; }
    }
    log("NV12 frames %s the encoder flag", td.BindFlags & D3D11_BIND_VIDEO_ENCODER ? "have" : "lack");

    // Crop + scale + colour conversion on the GPU
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    device.As(&vdev);
    context.As(&vctx);
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = src_w; cd.InputHeight = src_h;
    cd.OutputWidth = out_w; cd.OutputHeight = out_h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> vpe;
    ComPtr<ID3D11VideoProcessor> vp;
    if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &vpe)) || FAILED(vdev->CreateVideoProcessor(vpe.Get(), 0, &vp))) {
        log("Video processor failed");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> in_view;
    vdev->CreateVideoProcessorInputView(bgra.Get(), vpe.Get(), &ivd, &in_view);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> out_view[RING];
    for (int i = 0; i < RING; i++) vdev->CreateVideoProcessorOutputView(nv12[i].Get(), vpe.Get(), &ovd, &out_view[i]);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};  // RGB full range
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{}; // BT.709, studio range
    out_cs.YCbCr_Matrix = 1;
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in_cs);
    vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &out_cs);
    vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    Encoder enc;
    if (!enc.init(device.Get(), ad_desc.AdapterLuid, out_w, out_h, opt.fps, opt.mbps * 1000000)) return false;

    char gpu[128];
    WideCharToMultiByte(CP_UTF8, 0, ad_desc.Description, -1, gpu, sizeof(gpu), nullptr, nullptr);
    log("Streaming VBS %dx%d -> %ux%u @ %u fps, %u Mbps | GPU: %s | encoder: %s",
        src_w, src_h, out_w, out_h, opt.fps, opt.mbps, gpu, enc.name.c_str());

    uint32_t frame_no = 0;
    int ring = 0;
    bool force_key = true;
    double last_key = 0;
    uint32_t last_ip = 0;
    double last_capture = 0;
    std::vector<uint8_t> au;
    std::vector<uint8_t> packet(sizeof(VideoPacketHeader) + VIDEO_PAYLOAD);
    // Stats
    double stats_start = now_ms(), enc_total = 0;
    int stats_frames = 0;
    size_t stats_bytes = 0;

    while (running) {
        // Stop the session if VBS moved, resized or closed
        if (!IsWindow(vbs)) return false;
        RECT now_src = client_rect_on_screen(vbs);
        if (now_src.right - now_src.left != src_w || now_src.bottom - now_src.top != src_h) {
            log("VBS window changed size, restarting");
            return true;
        }
        src = now_src;

        // Keyframe requests from the phone
        char req[64];
        sockaddr_in from{};
        int from_len = sizeof(from);
        while (recvfrom(sock, req, sizeof(req), 0, (sockaddr*)&from, &from_len) == sizeof(KeyRequest)) {
            if (memcmp(req, "VAK1", 4) == 0) force_key = true;
            from_len = sizeof(from);
        }

        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> res;
        hr = dupl->AcquireNextFrame(50, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST) { log("Desktop duplication lost, restarting"); return true; }
        if (FAILED(hr)) { log("AcquireNextFrame failed 0x%08X", hr); return false; }
        if (info.LastPresentTime.QuadPart == 0) { dupl->ReleaseFrame(); continue; } // Only the mouse moved
        if (now_ms() - last_capture < 1000.0 / opt.fps - 2) { dupl->ReleaseFrame(); continue; } // Hold to the stream rate
        last_capture = now_ms();

        uint32_t ip = want_ip;
        uint32_t pose_seq = want_seq;
        float tan_x = want_tan_x;
        if (!ip) { dupl->ReleaseFrame(); log("phone gone, stopped streaming"); return false; }
        if (ip != last_ip) {
            char text[32];
            inet_ntop(AF_INET, &ip, text, sizeof(text));
            log("Sending to phone %s:%d", text, VIDEO_PORT);
            last_ip = ip;
            force_key = true;
        }

        // Copy the VBS client area out of the desktop image
        ComPtr<ID3D11Texture2D> desktop;
        res.As(&desktop);
        D3D11_BOX box;
        box.left = max(0L, src.left - out_desc.DesktopCoordinates.left);
        box.top = max(0L, src.top - out_desc.DesktopCoordinates.top);
        box.right = (UINT)min((LONG)(out_desc.DesktopCoordinates.right - out_desc.DesktopCoordinates.left), (LONG)box.left + src_w);
        box.bottom = (UINT)min((LONG)(out_desc.DesktopCoordinates.bottom - out_desc.DesktopCoordinates.top), (LONG)box.top + src_h);
        box.front = 0; box.back = 1;
        context->CopySubresourceRegion(bgra.Get(), 0, 0, 0, 0, desktop.Get(), 0, &box);
        dupl->ReleaseFrame();

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = in_view.Get();
        vctx->VideoProcessorBlt(vp.Get(), out_view[ring].Get(), 0, 1, &stream);

        double t0 = now_ms();
        if (t0 - last_key > 2000) force_key = true; // Safety net if key requests are lost
        bool keyframe = false;
        if (!enc.encode(nv12[ring].Get(), (LONGLONG)(t0 * 10000), force_key, au, keyframe)) return false;
        if (force_key) last_key = t0;
        force_key = false;
        ring = (ring + 1) % RING;
        enc_total += now_ms() - t0;
        if (au.empty()) continue;

        // Packetise
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(VIDEO_PORT);
        to.sin_addr.s_addr = ip;
        uint16_t count = (uint16_t)((au.size() + VIDEO_PAYLOAD - 1) / VIDEO_PAYLOAD);
        for (uint16_t i = 0; i < count; i++) {
            VideoPacketHeader h{};
            memcpy(h.magic, "VAV1", 4);
            h.frame = frame_no;
            h.index = i;
            h.count = count;
            h.flags = keyframe ? 1 : 0;
            size_t offset = (size_t)i * VIDEO_PAYLOAD;
            h.payload_len = (uint16_t)min((size_t)VIDEO_PAYLOAD, au.size() - offset);
            h.pose_seq = pose_seq;
            h.tan_x = tan_x;
            h.tan_y = tan_x * src_h / src_w; // VBS keeps the window's shape
            memcpy(packet.data(), &h, sizeof(h));
            memcpy(packet.data() + sizeof(h), au.data() + offset, h.payload_len);
            sendto(sock, (const char*)packet.data(), (int)(sizeof(h) + h.payload_len), 0, (sockaddr*)&to, sizeof(to));
        }
        frame_no++;
        stats_frames++;
        stats_bytes += au.size();

        double elapsed = now_ms() - stats_start;
        if (elapsed >= 2000) {
            log("streaming %ux%u, %.0f fps, %.1f Mbps, encode %.1f ms, encoder holds %.1f ms | %s",
                out_w, out_h, stats_frames * 1000.0 / elapsed, stats_bytes * 8 / elapsed / 1000.0,
                stats_frames ? enc_total / stats_frames : 0.0, enc.stat_outputs ? enc.stat_delay / enc.stat_outputs : 0.0,
                enc.name.c_str());
            enc.stat_delay = 0;
            enc.stat_outputs = 0;
            stats_start = now_ms();
            stats_frames = 0;
            stats_bytes = 0;
            enc_total = 0;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Background thread

static void worker_main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(VIDEO_PORT);
    local.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (sockaddr*)&local, sizeof(local)); // For keyframe requests
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

    Options opt;
    log("waiting for a phone on Wi-Fi");
    while (running) {
        if (!want_ip) { Sleep(200); continue; }  // Only capture while a phone is there to watch
        if (!run_session(opt, sock)) Sleep(500);
    }
    closesocket(sock);
    MFShutdown();
}

void streamer_start()
{
    if (running.exchange(true)) return;
    worker = std::thread([] {
        __try { worker_main(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log("streamer crashed and stopped (exception 0x%08X)", GetExceptionCode()); }
    });
}

void streamer_stop()
{
    // Called from DllMain: joining there can deadlock, so just ask the thread to finish
    running = false;
    if (worker.joinable()) worker.detach();
}

void streamer_update(uint32_t phone_ip, uint32_t pose_seq, float tan_half_fov_x)
{
    want_ip = phone_ip;
    want_seq = pose_seq;
    want_tan_x = tan_half_fov_x;
}

std::string streamer_status()
{
    std::lock_guard<std::mutex> lock(status_mutex);
    return status_text;
}
