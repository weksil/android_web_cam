#pragma once
#include <windows.h>

#include <cstdint>

/**
 * Shared-memory frame channel between awc-client.exe (producer, user session)
 * and awc-source.dll (consumer, loaded by the Frame Server service in session 0).
 *
 * The DLL creates the section and the events, because only a service account may
 * create objects in the Global\ namespace; the app merely opens them. That also
 * gives the desired lifecycle: the phone only streams while some application has
 * actually opened the virtual camera.
 */
namespace awc {

constexpr wchar_t kSectionName[]   = L"Global\\AWC_Frames_v1";
constexpr wchar_t kFrameEventName[] = L"Global\\AWC_FrameReady_v1";
constexpr wchar_t kWantEventName[]  = L"Global\\AWC_Wanted_v1";

constexpr uint32_t kSharedMagic = 0x31435741;   // 'AWC1'
constexpr uint32_t kSharedVersion = 1;
constexpr uint32_t kSlots = 3;
constexpr uint32_t kMaxWidth = 1920;
constexpr uint32_t kMaxHeight = 1080;
constexpr size_t   kSlotSize = size_t(kMaxWidth) * kMaxHeight * 3 / 2;   // NV12

struct SlotHeader {
    uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t size;
    int64_t  timestampHns;
};

struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    volatile LONG  writeIndex;          // incremented after every published frame
    volatile LONG  consumerActive;      // frame server has the camera open
    volatile LONG64 consumerHeartbeat;  // GetTickCount64 of the last stream tick
    volatile LONG64 producerHeartbeat;  // GetTickCount64 of the last published frame
    volatile LONG  requestedFps;
    SlotHeader slots[kSlots];
};

constexpr size_t kSharedSize = sizeof(SharedHeader) + size_t(kSlots) * kSlotSize;

inline uint8_t* slotData(SharedHeader* h, uint32_t slot) {
    return reinterpret_cast<uint8_t*>(h) + sizeof(SharedHeader) + size_t(slot) * kSlotSize;
}

} // namespace awc
