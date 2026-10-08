#pragma once
// VBS Android Optics wire protocol. All fields little-endian. The Android side mirrors this in
// android/app/src/main/java/nz/vbs/androidoptics/Protocol.kt.
//
// Pose, phone -> PC:   UDP to <PC>:47830 (Wi-Fi), or TCP to the phone's 127.0.0.1:47830 tunnelled with
//                      "adb reverse tcp:47830 tcp:47830" (USB). The PC acks every POSE_ACK_EVERY packets, and
//                      straight away when the laser rangefinder display changes (the ack carries it).
// Video, PC -> phone:  UDP to <phone>:47831, one H.264 access unit split over packets. The phone asks for a
//                      keyframe with a KeyRequest to the PC's port 47831. Video needs Wi-Fi (the phone's address).
// Discovery:           the phone broadcasts a DiscoverRequest to UDP port 47830; every PC running the plugin
//                      replies straight to the phone with a DiscoverReply. The replies' source addresses are the PCs.
// Buttons:             the phone's Bearing and Range buttons ride in every PosePacket; lrf.cpp works them like the
//                      Vector's, and the readouts come back in PoseAck.
// DAGR, app <-> PC:    the DAGR app (dagr/) sends "VDG1" to UDP 47840 (this PC, or broadcast); the plugin replies with
//                      its latest reading as one line of JSON (dagr.cpp).
#include <cstdint>

const int POSE_PORT = 47830;
const int VIDEO_PORT = 47831;
const int DAGR_PORT = 47840;
const int POSE_ACK_EVERY = 10;
const int VIDEO_PAYLOAD = 1200;

#pragma pack(push, 1)
struct PosePacket {             // 40 bytes
    char     magic[4];          // "VAO2"
    uint32_t seq;               // increments every packet
    int64_t  timestamp_ns;      // ARCore frame timestamp
    float    qx, qy, qz, qw;    // ARCore display-oriented camera rotation (X right, Y up, looking along -Z;
                                // world Y up with gravity)
    uint8_t  tracking;          // 1 = tracking, 0 = pose is stale
    uint8_t  calibrate;         // incremented each time Calibrate is pressed
    uint16_t fov_cdeg;          // wanted horizontal field of view, hundredths of a degree (0 = VBS default)
    uint8_t  buttons;           // held now: bit 0 Bearing, bit 1 Range (the Vector's left and right buttons)
    uint8_t  flags;             // bit 0: calibration mode (the IG shows a + where it faces)
    uint8_t  reserved[2];
};

struct PoseAck {                // 48 bytes
    char     magic[4];          // "VAA3"
    uint32_t seq;               // last seq received
    uint32_t received;          // packets received since the plugin loaded
    char     lrf_left[8];       // laser rangefinder display, as the Vector shows it: left and right readouts,
    char     lrf_right[8];      // zero-terminated ("" = blank)
    uint8_t  lrf_mark;          // 1 = the aiming mark ("o") is showing
    uint8_t  reserved[3];
    char     lrf_grid[16];      // the lased grid after both buttons (MGRS, e.g. "60HUB9461034250"), "" otherwise
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
struct DiscoverRequest {        // 8 bytes, phone -> broadcast
    char     magic[4];          // "VAD1"
    uint32_t reserved;
};

struct DiscoverReply {          // 36 bytes, PC -> phone
    char     magic[4];          // "VAR1"
    char     name[32];          // the PC's name, zero-terminated
};
#pragma pack(pop)

static_assert(sizeof(PosePacket) == 40, "PosePacket layout");
static_assert(sizeof(PoseAck) == 48, "PoseAck layout");
static_assert(sizeof(VideoPacketHeader) == 16, "VideoPacketHeader layout");
static_assert(sizeof(KeyRequest) == 8, "KeyRequest layout");
static_assert(sizeof(DiscoverRequest) == 8, "DiscoverRequest layout");
static_assert(sizeof(DiscoverReply) == 36, "DiscoverReply layout");
