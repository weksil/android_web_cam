#pragma once
#include <cstdint>

// Wire protocol shared with the Android client (see android-client/.../Control.kt).
namespace awc {

constexpr uint16_t kControlPort = 45001;   // phone listens here
constexpr uint16_t kRtpPort     = 45000;   // PC listens here by default
constexpr uint8_t  kMagic[4]    = {'A', 'W', 'C', '1'};
constexpr uint8_t  kRtpPayloadType = 96;
constexpr int      kKeepaliveMs = 1000;
constexpr int      kLinkTimeoutMs = 4000;

enum MsgType : uint8_t {
    kHello       = 0x01,   // PC -> phone: u16 rtpPort. Links up; the camera stays off.
    kHelloAck    = 0x02,   // phone -> PC: u16 w, u16 h, u8 fps, u32 bitrate, u32 ssrc, u16 rtpSrcPort
    kKeepalive   = 0x03,
    kRequestIdr  = 0x04,
    kSetBitrate  = 0x05,   // u32 bps
    kBye         = 0x06,
    kStreamStart = 0x07,   // an application opened the virtual camera
    kStreamStop  = 0x08,   // ...and released it; phone turns the camera off
    kSetEis      = 0x09,   // u8: 1 = phone stabilizes, 0 = we do it from gyro
    kSetExposure = 0x0A,   // i64 exposureNs, i32 iso; exposureNs 0 = auto exposure
    kSetConfig   = 0x0B,   // u8 idLen, id, u16 w, u16 h, u8 fps, u32 bitrate, u8 codec

    // phone -> PC telemetry, delivered on the RTP socket
    kGyro        = 0x20,   // u8 count, count * { i64 tNs, f32 x, y, z }
    kCamInfo     = 0x21,   // f32 fx, fy, cx, cy, u16 w, h, i32 orientation, u8 facing
    kFrameMeta   = 0x22,   // u32 rtpTs, i64 sensorNs, i64 exposureNs, i64 skewNs
    kSensorLimits = 0x23,  // i32 isoMin, isoMax, i64 exposureMinNs, exposureMaxNs
    kGravity     = 0x24,   // f32 gx, gy, gz in device axes; the horizon reference
    kCapabilities = 0x26,  // cameras with their sizes, plus the frame rates on offer
    kDiscover    = 0x10,   // broadcast probe
    kFound       = 0x11,   // phone -> PC: u16 controlPort, u8 nameLen, name
    kPunch       = 0x12,   // PC -> phone rtp port, opens the firewall pinhole
};

} // namespace awc
