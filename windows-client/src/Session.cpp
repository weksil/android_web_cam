#include "Session.h"

#include <iphlpapi.h>

#include <chrono>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace awc {

uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace {

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint16_t get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

int header(uint8_t* buf, uint8_t type) {
    std::memcpy(buf, kMagic, 4);
    buf[4] = type;
    return 5;
}

bool valid(const uint8_t* buf, int len) {
    return len >= 5 && std::memcmp(buf, kMagic, 4) == 0;
}

/** Local IPv4 subnets as (address, prefix length), host byte order. */
std::vector<std::pair<uint32_t, int>> localSubnets() {
    std::vector<std::pair<uint32_t, int>> result;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR) return result;

    for (auto* a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* in = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            const uint32_t ip = ntohl(in->sin_addr.s_addr);
            if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue;   // loopback / link-local
            result.emplace_back(ip, u->OnLinkPrefixLength);
        }
    }
    return result;
}

} // namespace

Session::~Session() { close(); }

bool Session::init(uint16_t rtpPort, std::string* err) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (err) *err = "WSAStartup failed";
        return false;
    }
    rtpPort_ = rtpPort;

    control_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    rtp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (control_ == INVALID_SOCKET || rtp_ == INVALID_SOCKET) {
        if (err) *err = "socket() failed";
        return false;
    }

    BOOL yes = TRUE;
    setsockopt(control_, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));
    setsockopt(rtp_, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    int rcvbuf = 8 << 20;
    setsockopt(rtp_, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = INADDR_ANY;
    any.sin_port = htons(rtpPort_);
    if (bind(rtp_, (sockaddr*)&any, sizeof(any)) != 0) {
        if (err) *err = "cannot bind UDP port " + std::to_string(rtpPort_) +
                        " (error " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }

    any.sin_port = 0;
    if (bind(control_, (sockaddr*)&any, sizeof(any)) != 0) {
        if (err) *err = "cannot bind control socket";
        return false;
    }
    return true;
}

void Session::close() {
    if (control_ != INVALID_SOCKET) { closesocket(control_); control_ = INVALID_SOCKET; }
    if (rtp_ != INVALID_SOCKET) { closesocket(rtp_); rtp_ = INVALID_SOCKET; }
    connected_ = false;
}

std::vector<PhoneInfo> Session::discover(int timeoutMs) {
    std::vector<PhoneInfo> found;
    uint8_t msg[5];
    int len = header(msg, kDiscover);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kControlPort);

    // A broadcast probe is answered from the phone's own address, which the Windows
    // firewall treats as unsolicited and drops. Probing each host of the local
    // subnet creates a matching UDP mapping per host, so the reply gets through.
    to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(control_, (char*)msg, len, 0, (sockaddr*)&to, sizeof(to));

    for (const auto& [ip, prefix] : localSubnets()) {
        if (prefix < 22 || prefix > 30) continue;              // keep the sweep small
        const uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
        const uint32_t network = ip & mask;
        const uint32_t broadcast = network | ~mask;
        for (uint32_t host = network + 1; host < broadcast; ++host) {
            if (host == ip) continue;
            to.sin_addr.s_addr = htonl(host);
            sendto(control_, (char*)msg, len, 0, (sockaddr*)&to, sizeof(to));
        }
    }

    const uint64_t deadline = nowMs() + timeoutMs;
    uint8_t buf[512];
    while (nowMs() < deadline) {
        int n = 0;
        sockaddr_in from{};
        if (!recvControl(buf, sizeof(buf), &n, &from, int(deadline - nowMs()))) continue;
        if (!valid(buf, n) || buf[4] != kFound || n < 8) continue;

        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        int nameLen = buf[7];
        std::string name(reinterpret_cast<char*>(buf + 8),
                         size_t((std::min)(nameLen, n - 8)));
        bool dup = false;
        for (auto& p : found) dup |= (p.ip == ip);
        if (!dup) found.push_back({ip, name});
    }
    return found;
}

bool Session::connect(const std::string& ip, int timeoutMs) {
    phone_ = {};
    phone_.sin_family = AF_INET;
    phone_.sin_port = htons(kControlPort);
    if (inet_pton(AF_INET, ip.c_str(), &phone_.sin_addr) != 1) return false;

    uint8_t msg[7];
    int len = header(msg, kHello);
    put16(msg + len, rtpPort_);
    len += 2;

    const uint64_t deadline = nowMs() + timeoutMs;
    uint8_t buf[512];
    while (nowMs() < deadline) {
        sendto(control_, (char*)msg, len, 0, (sockaddr*)&phone_, sizeof(phone_));

        int n = 0;
        sockaddr_in from{};
        int wait = (std::min)(500, int(deadline - nowMs()));
        if (wait <= 0) break;
        if (!recvControl(buf, sizeof(buf), &n, &from, wait)) continue;
        if (!valid(buf, n) || buf[4] != kHelloAck || n < 20) continue;

        params_.width = get16(buf + 5);
        params_.height = get16(buf + 7);
        params_.fps = buf[9];
        params_.bitrate = get32(buf + 10);
        params_.ssrc = get32(buf + 14);
        params_.rtpSourcePort = get16(buf + 18);

        phoneRtp_ = phone_;
        phoneRtp_.sin_port = htons(params_.rtpSourcePort);
        peerIp_ = ip;
        connected_ = true;
        lastRtpMs_ = nowMs();
        lastKeepaliveMs_ = 0;
        sendPunch();
        return true;
    }
    return false;
}

void Session::bye() {
    if (!connected_) return;
    uint8_t msg[5];
    int len = header(msg, kBye);
    sendControl(msg, len);
    connected_ = false;
}

void Session::requestIdr() {
    uint8_t msg[5];
    sendControl(msg, header(msg, kRequestIdr));
}

void Session::streamStart() {
    uint8_t msg[5];
    sendControl(msg, header(msg, kStreamStart));
    sendPunch();
    lastRtpMs_ = nowMs();       // give the phone time to open the camera
    streaming_ = true;
}

void Session::setDeviceStabilization(bool onDevice) {
    uint8_t msg[6];
    int len = header(msg, kSetEis);
    msg[len++] = onDevice ? 1 : 0;
    sendControl(msg, len);
}

void Session::setExposure(int64_t exposureNs, int32_t iso) {
    uint8_t msg[17];
    int len = header(msg, kSetExposure);
    for (int i = 0; i < 8; ++i) msg[len + i] = uint8_t(exposureNs >> (56 - i * 8));
    len += 8;
    put32(msg + len, uint32_t(iso));
    sendControl(msg, len + 4);
}

void Session::streamStop() {
    streaming_ = false;
    uint8_t msg[5];
    sendControl(msg, header(msg, kStreamStop));
}

void Session::setBitrate(uint32_t bps) {
    uint8_t msg[9];
    int len = header(msg, kSetBitrate);
    put32(msg + len, bps);
    sendControl(msg, len + 4);
}

void Session::sendControl(const uint8_t* data, int len) {
    if (control_ == INVALID_SOCKET) return;
    sendto(control_, (const char*)data, len, 0, (sockaddr*)&phone_, sizeof(phone_));
}

void Session::sendPunch() {
    uint8_t msg[5];
    int len = header(msg, kPunch);
    sendto(rtp_, (char*)msg, len, 0, (sockaddr*)&phoneRtp_, sizeof(phoneRtp_));
}

bool Session::recvControl(uint8_t* buf, int cap, int* len, sockaddr_in* from, int timeoutMs) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(control_, &rd);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) return false;

    int fromLen = sizeof(*from);
    int n = recvfrom(control_, (char*)buf, cap, 0, (sockaddr*)from, &fromLen);
    if (n <= 0) return false;
    *len = n;
    return true;
}

bool Session::poll(int timeoutMs, const std::function<void(const uint8_t*, int)>& onRtp) {
    const uint64_t now = nowMs();
    if (connected_ && now - lastKeepaliveMs_ >= kKeepaliveMs) {
        lastKeepaliveMs_ = now;
        uint8_t msg[5];
        sendControl(msg, header(msg, kKeepalive));
        sendPunch();                       // keep the UDP mapping alive
    }

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(rtp_, &rd);
    FD_SET(control_, &rd);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
        if (FD_ISSET(rtp_, &rd)) {
            uint8_t buf[2048];
            sockaddr_in from{};
            for (int i = 0; i < 256; ++i) {           // drain the socket
                int fromLen = sizeof(from);
                int n = recvfrom(rtp_, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
                if (n <= 0) break;
                // Telemetry shares this socket (and its firewall pinhole), so hand
                // every datagram to the caller and only count RTP towards liveness.
                if (n >= 12 && (buf[1] & 0x7F) == kRtpPayloadType) lastRtpMs_ = nowMs();
                onRtp(buf, n);
                u_long pending = 0;
                if (ioctlsocket(rtp_, FIONREAD, &pending) != 0 || pending == 0) break;
            }
        }
        if (FD_ISSET(control_, &rd)) {
            uint8_t buf[512];
            sockaddr_in from{};
            int fromLen = sizeof(from);
            recvfrom(control_, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        }
    }
    // Only the streaming state has a liveness signal; an idle link is kept by keepalives.
    return !connected_ || !streaming_ || (nowMs() - lastRtpMs_) < kLinkTimeoutMs;
}

} // namespace awc
