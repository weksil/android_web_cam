#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

#include "SharedFrames.h"

namespace awc {

/**
 * App side of the shared frame channel. The section is created by the DLL inside
 * the frame server, so this side only ever opens it: no section means no
 * application has the virtual camera open, and there is nothing to stream for.
 */
class ProducerChannel {
public:
    ~ProducerChannel() { close(); }

    bool tryOpen();
    void close();
    bool isOpen() const { return header_ != nullptr; }

    /** The frame server has our camera open and is asking for frames. */
    bool consumerActive() const;

    void publish(const uint8_t* nv12, size_t size, uint32_t width, uint32_t height, int64_t tsHns);

private:
    HANDLE section_ = nullptr;
    HANDLE frameEvent_ = nullptr;
    SharedHeader* header_ = nullptr;
};

/** Nearest-neighbour NV12 rescale; the virtual camera always exposes 1080p. */
void scaleNv12(const uint8_t* src, uint32_t sw, uint32_t sh,
               uint8_t* dst, uint32_t dw, uint32_t dh);

/** Mirrors an NV12 frame in place; both axes together is a 180 degree rotation. */
void flipNv12(uint8_t* data, uint32_t width, uint32_t height, bool horizontal, bool vertical);

} // namespace awc
