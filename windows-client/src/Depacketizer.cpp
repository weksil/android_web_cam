#include "Depacketizer.h"

#include <cstring>

namespace awc {

namespace {
constexpr uint8_t kStartCode[4] = {0, 0, 0, 1};
constexpr int kFuA = 28;
constexpr int kStapA = 24;
} // namespace

void Depacketizer::reset() {
    au_.clear();
    corrupt_ = false;
    sawIdr_ = false;
    inFragment_ = false;
    haveSeq_ = false;
    needIdr_ = true;
}

void Depacketizer::startNal(const uint8_t* payload, int len) {
    au_.insert(au_.end(), kStartCode, kStartCode + 4);
    au_.insert(au_.end(), payload, payload + len);
}

void Depacketizer::emit(uint32_t ts) {
    if (!au_.empty() && !corrupt_ && cb_) {
        stats.frames++;
        if (sawIdr_) stats.keyframes++;
        cb_(au_.data(), au_.size(), ts, sawIdr_);
    } else if (!au_.empty()) {
        stats.dropped++;
        needIdr_ = true;
    }
    au_.clear();
    corrupt_ = false;
    sawIdr_ = false;
    inFragment_ = false;
}

/** RFC 7798: payload header is two bytes, type 49 is a fragment, 48 an aggregate. */
void Depacketizer::pushHevc(const uint8_t* p, int n, bool marker, uint32_t ts) {
    if (n < 2) { corrupt_ = true; return; }
    const int type = (p[0] >> 1) & 0x3F;

    if (type == 49) {                       // fragmentation unit
        if (n < 3) { corrupt_ = true; return; }
        const uint8_t fu = p[2];
        const bool start = (fu & 0x80) != 0;
        const bool end = (fu & 0x40) != 0;
        const int nalType = fu & 0x3F;
        if (start) {
            au_.insert(au_.end(), kStartCode, kStartCode + 4);
            au_.push_back(uint8_t((nalType << 1) | (p[0] & 0x01)));
            au_.push_back(p[1]);
            inFragment_ = true;
            if (nalType >= 16 && nalType <= 21) sawIdr_ = true;
        } else if (!inFragment_) {
            corrupt_ = true;
        }
        au_.insert(au_.end(), p + 3, p + n);
        if (end) inFragment_ = false;
    } else if (type == 48) {                // aggregation packet
        int pos = 2;
        while (pos + 2 <= n) {
            const int size = (p[pos] << 8) | p[pos + 1];
            pos += 2;
            if (size <= 0 || pos + size > n) { corrupt_ = true; break; }
            const int inner = (p[pos] >> 1) & 0x3F;
            if (inner >= 16 && inner <= 21) sawIdr_ = true;
            startNal(p + pos, size);
            pos += size;
        }
    } else {
        if (type >= 16 && type <= 21) sawIdr_ = true;
        startNal(p, n);
    }

    if (marker) emit(ts);
}

void Depacketizer::push(const uint8_t* pkt, int len) {
    if (len < 13) return;
    if ((pkt[0] >> 6) != 2) return;                       // RTP version
    const int csrc = pkt[0] & 0x0F;
    int offset = 12 + csrc * 4;
    if (pkt[0] & 0x10) {                                  // extension header
        if (len < offset + 4) return;
        const int words = (pkt[offset + 2] << 8) | pkt[offset + 3];
        offset += 4 + words * 4;
    }
    if (offset >= len) return;

    stats.packets++;
    stats.bytes += len;

    const bool marker = (pkt[1] & 0x80) != 0;
    const uint16_t seq = uint16_t((pkt[2] << 8) | pkt[3]);
    const uint32_t ts = (uint32_t(pkt[4]) << 24) | (uint32_t(pkt[5]) << 16) |
                        (uint32_t(pkt[6]) << 8) | pkt[7];

    if (haveSeq_ && seq != nextSeq_) {
        const uint16_t gap = uint16_t(seq - nextSeq_);
        if (gap < 0x8000) {
            stats.lost += gap;      // packets missing
            corrupt_ = true;
        } else {
            return;                 // late/duplicate packet, ignore
        }
    }
    haveSeq_ = true;
    nextSeq_ = uint16_t(seq + 1);

    const uint8_t* p = pkt + offset;
    const int n = len - offset;

    if (hevc_) {
        pushHevc(p, n, marker, ts);
        return;
    }

    const int type = p[0] & 0x1F;

    if (type == kFuA) {
        if (n < 2) { corrupt_ = true; return; }
        const uint8_t fu = p[1];
        const bool start = (fu & 0x80) != 0;
        const bool end = (fu & 0x40) != 0;
        const uint8_t nalType = fu & 0x1F;
        if (start) {
            const uint8_t header = uint8_t((p[0] & 0xE0) | nalType);
            au_.insert(au_.end(), kStartCode, kStartCode + 4);
            au_.push_back(header);
            inFragment_ = true;
            if (nalType == 5) sawIdr_ = true;
        } else if (!inFragment_) {
            corrupt_ = true;        // we joined mid-NAL
        }
        au_.insert(au_.end(), p + 2, p + n);
        if (end) inFragment_ = false;
    } else if (type == kStapA) {
        int pos = 1;
        while (pos + 2 <= n) {
            const int size = (p[pos] << 8) | p[pos + 1];
            pos += 2;
            if (size <= 0 || pos + size > n) { corrupt_ = true; break; }
            if ((p[pos] & 0x1F) == 5) sawIdr_ = true;
            startNal(p + pos, size);
            pos += size;
        }
    } else if (type >= 1 && type <= 23) {
        if (type == 5) sawIdr_ = true;
        startNal(p, n);
    }

    if (marker) emit(ts);
}

} // namespace awc
