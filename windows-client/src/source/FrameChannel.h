#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

#include "../SharedFrames.h"

namespace awc {

/**
 * Service side of the shared frame channel: creates the Global\ section and
 * events (only a service account may do that) and reads the newest frame the
 * producer published.
 */
class FrameChannel {
public:
    bool open();
    void close();
    bool valid() const { return header_ != nullptr; }

    /** Marks the camera as opened/closed so the producer knows when to stream. */
    void setActive(bool active);
    void tick();

    /** True if a fresh frame was copied into [dst]. */
    bool readLatest(std::vector<uint8_t>& dst, uint32_t& width, uint32_t& height);

    /** Producer published a frame within the last second. */
    bool producerAlive() const;

    HANDLE frameEvent() const { return frameEvent_; }

private:
    HANDLE section_ = nullptr;
    HANDLE frameEvent_ = nullptr;
    SharedHeader* header_ = nullptr;
    LONG lastIndex_ = 0;
};

} // namespace awc
