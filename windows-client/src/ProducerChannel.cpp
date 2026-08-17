#include "ProducerChannel.h"

#include <algorithm>
#include <cstring>

namespace awc {

bool ProducerChannel::tryOpen() {
    if (header_) return true;

    section_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSectionName);
    if (!section_) return false;

    header_ = static_cast<SharedHeader*>(
        MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, kSharedSize));
    if (!header_ || header_->magic != kSharedMagic || header_->version != kSharedVersion) {
        close();
        return false;
    }
    frameEvent_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, kFrameEventName);
    return true;
}

void ProducerChannel::close() {
    if (header_) { UnmapViewOfFile(header_); header_ = nullptr; }
    if (frameEvent_) { CloseHandle(frameEvent_); frameEvent_ = nullptr; }
    if (section_) { CloseHandle(section_); section_ = nullptr; }
}

bool ProducerChannel::consumerActive() const {
    if (!header_) return false;
    if (!header_->consumerActive) return false;
    const LONG64 beat = header_->consumerHeartbeat;
    return (static_cast<LONG64>(GetTickCount64()) - beat) < 3000;
}

void ProducerChannel::publish(const uint8_t* nv12, size_t size, uint32_t width, uint32_t height,
                              int64_t tsHns) {
    if (!header_ || size == 0 || size > kSlotSize) return;

    const LONG index = header_->writeIndex;
    const uint32_t slot = static_cast<uint32_t>(index % kSlots);

    SlotHeader& s = header_->slots[slot];
    s.sequence = static_cast<uint32_t>(index) + 1;
    s.width = width;
    s.height = height;
    s.size = static_cast<uint32_t>(size);
    s.timestampHns = tsHns;
    std::memcpy(slotData(header_, slot), nv12, size);

    InterlockedExchange(&header_->writeIndex, index + 1);
    InterlockedExchange64(&header_->producerHeartbeat, static_cast<LONG64>(GetTickCount64()));
    if (frameEvent_) SetEvent(frameEvent_);
}

void flipNv12(uint8_t* data, uint32_t width, uint32_t height, bool horizontal, bool vertical) {
    if (!horizontal && !vertical) return;
    uint8_t* chroma = data + size_t(width) * height;
    const uint32_t chromaRows = height / 2;

    if (horizontal) {
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* row = data + size_t(y) * width;
            for (uint32_t x = 0; x < width / 2; ++x) std::swap(row[x], row[width - 1 - x]);
        }
        for (uint32_t y = 0; y < chromaRows; ++y) {
            uint8_t* row = chroma + size_t(y) * width;
            for (uint32_t x = 0; x < width / 4; ++x) {        // U and V travel as a pair
                std::swap(row[2 * x], row[width - 2 - 2 * x]);
                std::swap(row[2 * x + 1], row[width - 1 - 2 * x]);
            }
        }
    }

    if (vertical) {
        static thread_local std::vector<uint8_t> line;
        line.resize(width);
        auto swapRows = [&](uint8_t* plane, uint32_t rows) {
            for (uint32_t y = 0; y < rows / 2; ++y) {
                uint8_t* a = plane + size_t(y) * width;
                uint8_t* b = plane + size_t(rows - 1 - y) * width;
                std::memcpy(line.data(), a, width);
                std::memcpy(a, b, width);
                std::memcpy(b, line.data(), width);
            }
        };
        swapRows(data, height);
        swapRows(chroma, chromaRows);
    }
}

void scaleNv12(const uint8_t* src, uint32_t sw, uint32_t sh,
               uint8_t* dst, uint32_t dw, uint32_t dh) {
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t sy = y * sh / dh;
        const uint8_t* srow = src + size_t(sy) * sw;
        uint8_t* drow = dst + size_t(y) * dw;
        for (uint32_t x = 0; x < dw; ++x) drow[x] = srow[x * sw / dw];
    }
    const uint8_t* srcUv = src + size_t(sw) * sh;
    uint8_t* dstUv = dst + size_t(dw) * dh;
    for (uint32_t y = 0; y < dh / 2; ++y) {
        const uint32_t sy = y * (sh / 2) / (dh / 2);
        const uint8_t* srow = srcUv + size_t(sy) * sw;
        uint8_t* drow = dstUv + size_t(y) * dw;
        for (uint32_t x = 0; x < dw / 2; ++x) {
            const uint32_t sx = x * (sw / 2) / (dw / 2);
            drow[x * 2] = srow[sx * 2];
            drow[x * 2 + 1] = srow[sx * 2 + 1];
        }
    }
}

} // namespace awc
