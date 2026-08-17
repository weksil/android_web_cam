#include "FrameChannel.h"

#include <sddl.h>

#include <cstring>

#include "../Log.h"

#pragma comment(lib, "advapi32.lib")

namespace awc {

namespace {

// Local Service (the frame server) creates the objects; the producer runs as an
// interactive user, so Users need access too.
constexpr wchar_t kSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)(A;;GA;;;LS)(A;;GA;;;NS)";

bool makeAttributes(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr))
        return false;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

} // namespace

bool FrameChannel::open() {
    if (header_) return true;

    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    const bool haveSd = makeAttributes(sa, sd);

    section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, haveSd ? &sa : nullptr,
                                  PAGE_READWRITE, 0, static_cast<DWORD>(kSharedSize), kSectionName);
    if (!section_) {
        logf("CreateFileMapping(%ls) failed: %lu", kSectionName, GetLastError());
        if (sd) LocalFree(sd);
        return false;
    }
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;

    frameEvent_ = CreateEventW(haveSd ? &sa : nullptr, FALSE, FALSE, kFrameEventName);
    if (sd) LocalFree(sd);

    header_ = static_cast<SharedHeader*>(MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, kSharedSize));
    if (!header_) { close(); return false; }

    if (created || header_->magic != kSharedMagic) {
        std::memset(header_, 0, sizeof(SharedHeader));
        header_->magic = kSharedMagic;
        header_->version = kSharedVersion;
    }
    lastIndex_ = header_->writeIndex;
    return true;
}

void FrameChannel::close() {
    setActive(false);
    if (header_) { UnmapViewOfFile(header_); header_ = nullptr; }
    if (frameEvent_) { CloseHandle(frameEvent_); frameEvent_ = nullptr; }
    if (section_) { CloseHandle(section_); section_ = nullptr; }
}

void FrameChannel::setActive(bool active) {
    if (!header_) return;
    InterlockedExchange(&header_->consumerActive, active ? 1 : 0);
    InterlockedExchange64(&header_->consumerHeartbeat, static_cast<LONG64>(GetTickCount64()));
}

void FrameChannel::tick() {
    if (!header_) return;
    InterlockedExchange64(&header_->consumerHeartbeat, static_cast<LONG64>(GetTickCount64()));
}

bool FrameChannel::producerAlive() const {
    if (!header_) return false;
    const LONG64 last = header_->producerHeartbeat;
    return last != 0 && (static_cast<LONG64>(GetTickCount64()) - last) < 2000;
}

bool FrameChannel::readLatest(std::vector<uint8_t>& dst, uint32_t& width, uint32_t& height) {
    if (!header_) return false;
    const LONG index = header_->writeIndex;
    if (index == lastIndex_ || index <= 0) return false;

    const uint32_t slot = static_cast<uint32_t>((index - 1) % kSlots);
    const SlotHeader before = header_->slots[slot];
    if (before.size == 0 || before.size > kSlotSize) return false;

    dst.resize(before.size);
    std::memcpy(dst.data(), slotData(header_, slot), before.size);

    const SlotHeader after = header_->slots[slot];
    if (after.sequence != before.sequence) return false;   // torn read, skip this one

    width = before.width;
    height = before.height;
    lastIndex_ = index;
    return true;
}

} // namespace awc
