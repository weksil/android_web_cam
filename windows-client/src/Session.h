#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Protocol.h"

namespace awc {

struct PhoneInfo {
    std::string ip;
    std::string name;
};

struct StreamParams {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t  fps = 0;
    uint32_t bitrate = 0;
    uint32_t ssrc = 0;
    uint16_t rtpSourcePort = 0;
};

/**
 * Owns the control socket (ephemeral) and the RTP socket (bound to rtpPort),
 * runs the handshake, keeps the link alive and keeps the firewall pinhole open.
 */
class Session {
public:
    Session() = default;
    ~Session();

    bool init(uint16_t rtpPort, std::string* err);
    std::vector<PhoneInfo> discover(int timeoutMs);
    bool connect(const std::string& ip, int timeoutMs);
    void bye();
    void close();

    // Waits up to timeoutMs for RTP; invokes onRtp for every datagram received.
    // Returns false once the phone has gone silent for kLinkTimeoutMs.
    bool poll(int timeoutMs, const std::function<void(const uint8_t*, int)>& onRtp);

    void requestIdr();
    void setBitrate(uint32_t bps);
    void streamStart();
    void streamStop();
    /** true = phone runs its own EIS, false = we stabilize from gyro data. */
    void setDeviceStabilization(bool onDevice);
    /** Fixed exposure with the gain we choose; exposureNs 0 restores auto exposure. */
    void setExposure(int64_t exposureNs, int32_t iso);

    const StreamParams& params() const { return params_; }
    const std::string& peer() const { return peerIp_; }
    bool connected() const { return connected_; }

private:
    void sendControl(const uint8_t* data, int len);
    void sendPunch();
    bool recvControl(uint8_t* buf, int cap, int* len, sockaddr_in* from, int timeoutMs);

    SOCKET control_ = INVALID_SOCKET;
    SOCKET rtp_ = INVALID_SOCKET;
    sockaddr_in phone_{};
    sockaddr_in phoneRtp_{};
    std::string peerIp_;
    StreamParams params_{};
    bool connected_ = false;
    bool streaming_ = false;
    uint16_t rtpPort_ = kRtpPort;
    uint64_t lastKeepaliveMs_ = 0;
    uint64_t lastRtpMs_ = 0;
};

uint64_t nowMs();

} // namespace awc
