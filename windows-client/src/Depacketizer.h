#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace awc {

/**
 * RFC 6184 depacketizer: RTP packets in, Annex-B access units out.
 *
 * A sequence-number gap marks the current access unit as corrupt; the frame is
 * dropped and [needIdr] is raised so the caller can ask the phone for a key frame.
 */
class Depacketizer {
public:
    // data/size = Annex-B access unit, ts = 90 kHz RTP timestamp
    using OnAccessUnit = std::function<void(const uint8_t* data, size_t size, uint32_t ts, bool key)>;

    void setCallback(OnAccessUnit cb) { cb_ = std::move(cb); }
    /** HEVC uses RFC 7798: two-byte NAL headers and different aggregation types. */
    void setHevc(bool hevc) { hevc_ = hevc; }
    void push(const uint8_t* pkt, int len);
    void reset();

    bool takeNeedIdr() { bool v = needIdr_; needIdr_ = false; return v; }

    struct Stats {
        uint64_t packets = 0;
        uint64_t bytes = 0;
        uint64_t frames = 0;
        uint64_t dropped = 0;   // access units discarded because of loss
        uint64_t lost = 0;      // missing RTP sequence numbers
        uint64_t keyframes = 0;
    } stats;

private:
    void startNal(const uint8_t* payload, int len);
    void pushHevc(const uint8_t* payload, int len, bool marker, uint32_t ts);
    void emit(uint32_t ts);

    OnAccessUnit cb_;
    std::vector<uint8_t> au_;
    bool haveSeq_ = false;
    uint16_t nextSeq_ = 0;
    bool corrupt_ = false;
    bool sawIdr_ = false;
    bool needIdr_ = true;
    bool inFragment_ = false;
    bool hevc_ = false;
};

} // namespace awc
