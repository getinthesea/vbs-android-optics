#pragma once
// VBS Android Optics wire protocol. All fields little-endian. The Android side mirrors this in
// android/app/src/main/java/nz/vbs/androidoptics/Protocol.kt.
//
// Pose, phone -> PC:   UDP to <PC>:47830 (Wi-Fi), or TCP to the phone's 127.0.0.1:47830 tunnelled with
//                      "adb reverse tcp:47830 tcp:47830" (USB). The PC acks every POSE_ACK_EVERY packets.
// Video, PC -> phone:  UDP to <phone>:47831, one H.264 access unit split over packets. The phone asks for a
//                      keyframe with a KeyRequest to the PC's port 47831. Video needs Wi-Fi (the phone's address).
#include <cstdint>

const int POSE_PORT = 47830;
const int VIDEO_PORT = 47831;
const int POSE_ACK_EVERY = 10;
const int VIDEO_PAYLOAD = 1200;

#pragma pack(push, 1)
struct PosePacket {             // 36 bytes
    char     magic[4];          // "VAO1"
    uint32_t seq;               // increments every packet
    int64_t  timestamp_ns;      // ARCore frame timestamp
    float    qx, qy, qz, qw;    // ARCore display-oriented camera rotation (X right, Y up, looking along -Z;
                                // world Y up with gravity)
    uint8_t  tracking;          // 1 = tracking, 0 = pose is stale
    uint8_t  calibrate;         // incremented each time Calibrate is pressed
    uint16_t fov_cdeg;          // wanted horizontal field of view, hundredths of a degree (0 = VBS default)
};

struct PoseAck {                // 12 bytes
    char     magic[4];          // "VAA1"
    uint32_t seq;               // last seq received
    uint32_t received;          // packets received since the plugin loaded
};

struct VideoPacketHeader {      // 16 bytes, then payload_len bytes of H.264 (Annex-B)
    char     magic[4];          // "VAV1"
    uint32_t frame;             // increments per encoded frame
    uint16_t index;             // packet index within the frame
    uint16_t count;             // packets in this frame
    uint8_t  flags;             // bit 0: keyframe
    uint8_t  reserved;
    uint16_t payload_len;
};

struct KeyRequest {             // 8 bytes, phone -> PC
    char     magic[4];          // "VAK1"
    uint32_t last_frame;
};
#pragma pack(pop)

static_assert(sizeof(PosePacket) == 36, "PosePacket layout");
static_assert(sizeof(PoseAck) == 12, "PoseAck layout");
static_assert(sizeof(VideoPacketHeader) == 16, "VideoPacketHeader layout");
static_assert(sizeof(KeyRequest) == 8, "KeyRequest layout");
