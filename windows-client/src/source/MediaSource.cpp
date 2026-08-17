#include "MediaSource.h"

#include <cstring>

#include "../Log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfsensorgroup.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace awc {

namespace {
constexpr LONGLONG kFrameDuration = 10'000'000LL / kCamFps;
const HRESULT kNotSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

// ------------------------------------------------------------------ stream

HRESULT VCamStream::init(VCamSource* source, IMFStreamDescriptor* descriptor) {
    InitializeCriticalSection(&lock_);
    source_ = source;
    descriptor_ = descriptor;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake_ || !quit_) return E_FAIL;

    const HRESULT hr = MFCreateEventQueue(events_.GetAddressOf());
    if (FAILED(hr)) return hr;
    frame_.resize(size_t(kCamWidth) * kCamHeight * 3 / 2);
    fillPlaceholder(frame_);

    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    return thread_ ? S_OK : E_FAIL;
}

void VCamStream::shutdown() {
    EnterCriticalSection(&lock_);
    shutdown_ = true;
    active_ = false;
    requests_.clear();
    LeaveCriticalSection(&lock_);

    if (quit_) SetEvent(quit_);
    if (wake_) SetEvent(wake_);
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (events_) events_->Shutdown();
    if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
    if (quit_) { CloseHandle(quit_); quit_ = nullptr; }
    DeleteCriticalSection(&lock_);
}

HRESULT VCamStream::setState(bool active) {
    EnterCriticalSection(&lock_);
    if (shutdown_) { LeaveCriticalSection(&lock_); return MF_E_SHUTDOWN; }
    const bool changed = active != active_;
    active_ = active;
    if (active) {
        start_ = 0;
        nextTime_ = 0;
    } else {
        requests_.clear();
    }
    LeaveCriticalSection(&lock_);

    if (changed && events_)
        events_->QueueEventParamVar(active ? MEStreamStarted : MEStreamStopped, GUID_NULL, S_OK, nullptr);
    SetEvent(wake_);
    return S_OK;
}

IFACEMETHODIMP VCamStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    return events_ ? events_->GetEvent(flags, event) : MF_E_SHUTDOWN;
}
IFACEMETHODIMP VCamStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    return events_ ? events_->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
}
IFACEMETHODIMP VCamStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    return events_ ? events_->EndGetEvent(result, event) : MF_E_SHUTDOWN;
}
IFACEMETHODIMP VCamStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                      const PROPVARIANT* value) {
    return events_ ? events_->QueueEventParamVar(type, extendedType, status, value) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP VCamStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    if (!source_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

IFACEMETHODIMP VCamStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    if (!descriptor_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(descriptor);
}

IFACEMETHODIMP VCamStream::RequestSample(IUnknown* token) {
    EnterCriticalSection(&lock_);
    const HRESULT hr = shutdown_ ? MF_E_SHUTDOWN : (active_ ? S_OK : MF_E_INVALIDREQUEST);
    if (SUCCEEDED(hr)) requests_.emplace_back(token);
    const size_t pending = requests_.size();
    LeaveCriticalSection(&lock_);
    if (requested_++ < 5) logf("RequestSample #%llu -> 0x%08lX (pending %llu)",
                               (unsigned long long)requested_, (unsigned long)hr,
                               (unsigned long long)pending);
    if (SUCCEEDED(hr)) SetEvent(wake_);
    return hr;
}

IFACEMETHODIMP VCamStream::SetStreamState(MF_STREAM_STATE state) {
    logf("SetStreamState(%d)", int(state));
    return setState(state == MF_STREAM_STATE_RUNNING);
}

IFACEMETHODIMP VCamStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    EnterCriticalSection(&lock_);
    *state = active_ ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

IFACEMETHODIMP VCamStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return kNotSupported;
}
IFACEMETHODIMP VCamStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return kNotSupported;
}
IFACEMETHODIMP VCamStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return kNotSupported;
}

void VCamStream::fillPlaceholder(std::vector<uint8_t>& frame) const {
    const size_t luma = size_t(kCamWidth) * kCamHeight;
    std::memset(frame.data(), 24, luma);              // near-black
    std::memset(frame.data() + luma, 128, frame.size() - luma);
}

DWORD WINAPI VCamStream::threadProc(void* self) {
    static_cast<VCamStream*>(self)->loop();
    return 0;
}

void VCamStream::loop() {
    FrameChannel& channel = source_->channel();
    uint32_t width = 0, height = 0;

    // One sample per iteration, paced by the arrival of new frames: the consumer
    // asks for samples as fast as we answer, so the frame rate is ours to set.
    const DWORD frameMs = 1000 / kCamFps;
    for (;;) {
        HANDLE waits[3] = {quit_, wake_, channel.frameEvent()};
        const DWORD count = channel.frameEvent() ? 3 : 2;
        if (WaitForMultipleObjects(count, waits, FALSE, frameMs) == WAIT_OBJECT_0) return;

        channel.tick();
        bool fresh = false;
        if (channel.readLatest(latest_, width, height)) {
            const size_t expected = size_t(kCamWidth) * kCamHeight * 3 / 2;
            if (width == kCamWidth && height == kCamHeight && latest_.size() == expected) {
                EnterCriticalSection(&lock_);
                frame_.swap(latest_);
                haveFrame_ = true;
                LeaveCriticalSection(&lock_);
                fresh = true;
            }
        }

        // Pace on frame arrival, not on a timer: a timer tick of the same length as
        // the frame interval beats against it and drops every other frame.
        const LONGLONG now = MFGetSystemTime();
        if (!fresh && now - lastDelivery_ < kFrameDuration * 2) continue;

        ComPtr<IUnknown> token;
        bool have = false;
        EnterCriticalSection(&lock_);
        if (!shutdown_ && active_ && !requests_.empty()) {
            token = requests_.front();
            requests_.pop_front();
            have = true;
        }
        LeaveCriticalSection(&lock_);
        if (have) {
            lastDelivery_ = now;
            deliver(token.Get());
        }
    }
}

HRESULT VCamStream::deliver(IUnknown* token) {
    ComPtr<IMFMediaBuffer> buffer;
    const DWORD size = DWORD(size_t(kCamWidth) * kCamHeight * 3 / 2);
    HRESULT hr = MFCreateMemoryBuffer(size, buffer.GetAddressOf());
    if (FAILED(hr)) return hr;

    BYTE* dst = nullptr;
    DWORD maxLen = 0;
    hr = buffer->Lock(&dst, &maxLen, nullptr);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&lock_);
    std::memcpy(dst, frame_.data(), size);
    LeaveCriticalSection(&lock_);
    buffer->Unlock();
    buffer->SetCurrentLength(size);

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(sample.GetAddressOf());
    if (FAILED(hr)) return hr;
    sample->AddBuffer(buffer.Get());

    // Live capture sources timestamp on the system clock, not from zero.
    LONGLONG time = MFGetSystemTime();
    if (time <= nextTime_) time = nextTime_ + 1;
    nextTime_ = time;

    sample->SetSampleTime(time);
    sample->SetSampleDuration(kFrameDuration);
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);

    hr = events_ ? events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get())
                 : MF_E_SHUTDOWN;
    if (delivered_++ < 5) logf("deliver #%llu time=%lld -> 0x%08lX",
                               (unsigned long long)delivered_, time, (unsigned long)hr);
    return hr;
}

// ------------------------------------------------------------------ source

VCamSource::VCamSource() {
    InitializeCriticalSection(&lock_);
    MFCreateAttributes(attributes_.GetAddressOf(), 8);   // must outlive Shutdown: we expose IMFAttributes
}

IFACEMETHODIMP VCamSource::QueryInterface(REFIID riid, void** object) {
    const HRESULT hr = __super::QueryInterface(riid, object);
    if (FAILED(hr)) {
        wchar_t iid[64]{};
        StringFromGUID2(riid, iid, ARRAYSIZE(iid));
        logf("QI %ls -> unsupported", iid);
    }
    return hr;
}

VCamSource::~VCamSource() {
    Shutdown();
    DeleteCriticalSection(&lock_);
}

HRESULT VCamSource::createMediaType(IMFMediaType** type) const {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(t.GetAddressOf());
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, kCamWidth);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, UINT32(size_t(kCamWidth) * kCamHeight * 3 / 2));
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, kCamWidth, kCamHeight);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kCamFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return t.CopyTo(type);
}

HRESULT VCamSource::init() {
    logInit(L"C:\\ProgramData\\AndroidWebCam", L"source.log");
    logf("VCamSource::init");

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { logf("  MFStartup failed 0x%08lX", (unsigned long)hr); return hr; }
    mfStarted_ = true;

    hr = MFCreateEventQueue(events_.GetAddressOf());
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> type;
    hr = createMediaType(type.GetAddressOf());
    if (FAILED(hr)) return hr;

    IMFMediaType* types[] = {type.Get()};
    hr = MFCreateStreamDescriptor(0, 1, types, descriptor_.GetAddressOf());
    if (FAILED(hr)) return hr;

    descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    descriptor_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    ComPtr<IMFMediaTypeHandler> handler;
    hr = descriptor_->GetMediaTypeHandler(handler.GetAddressOf());
    if (FAILED(hr)) return hr;
    handler->SetCurrentMediaType(type.Get());

    IMFStreamDescriptor* descriptors[] = {descriptor_.Get()};
    hr = MFCreatePresentationDescriptor(1, descriptors, presentation_.GetAddressOf());
    if (FAILED(hr)) return hr;
    presentation_->SelectStream(0);

    if (!attributes_) return E_UNEXPECTED;

    // The frame server refuses to start a stream unless the source advertises a
    // sensor profile: without this ReadSample fails with
    // MF_E_HW_MFT_FAILED_START_STREAMING and Start() is never even called.
    attributes_->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    ComPtr<IMFSensorProfileCollection> profiles;
    hr = MFCreateSensorProfileCollection(profiles.GetAddressOf());
    if (SUCCEEDED(hr)) {
        ComPtr<IMFSensorProfile> profile;
        hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, profile.GetAddressOf());
        if (SUCCEEDED(hr)) {
            profile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
            profiles->AddProfile(profile.Get());
        }
        attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get());
    }
    logf("  sensor profile 0x%08lX", (unsigned long)hr);

    const bool channelOk = channel_.open();
    logf("  shared channel %s", channelOk ? "created" : "FAILED");

    stream_ = Make<VCamStream>();
    if (!stream_) return E_OUTOFMEMORY;
    hr = stream_->init(this, descriptor_.Get());
    logf("  init done 0x%08lX", (unsigned long)hr);
    return hr;
}

IFACEMETHODIMP VCamSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    return events_ ? events_->GetEvent(flags, event) : MF_E_SHUTDOWN;
}
IFACEMETHODIMP VCamSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    return events_ ? events_->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
}
IFACEMETHODIMP VCamSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    return events_ ? events_->EndGetEvent(result, event) : MF_E_SHUTDOWN;
}
IFACEMETHODIMP VCamSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                      const PROPVARIANT* value) {
    return events_ ? events_->QueueEventParamVar(type, extendedType, status, value) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP VCamSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

IFACEMETHODIMP VCamSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    EnterCriticalSection(&lock_);
    HRESULT hr = shutdown_ ? MF_E_SHUTDOWN : S_OK;
    if (SUCCEEDED(hr)) hr = presentation_->Clone(descriptor);
    LeaveCriticalSection(&lock_);
    return hr;
}

IFACEMETHODIMP VCamSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                 const PROPVARIANT* startPosition) {
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    EnterCriticalSection(&lock_);
    if (shutdown_) { LeaveCriticalSection(&lock_); return MF_E_SHUTDOWN; }
    const bool first = !started_;
    started_ = true;
    LeaveCriticalSection(&lock_);

    logf("VCamSource::Start (first=%d)", first ? 1 : 0);
    channel_.setActive(true);

    PROPVARIANT position{};
    if (startPosition && startPosition->vt == VT_I8) {
        position.vt = VT_I8;
        position.hVal.QuadPart = startPosition->hVal.QuadPart;
    } else {
        position.vt = VT_EMPTY;
    }

    if (first) {
        events_->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK,
                                    static_cast<IMFMediaStream*>(stream_.Get()));
    } else {
        events_->QueueEventParamUnk(MEUpdatedStream, GUID_NULL, S_OK,
                                    static_cast<IMFMediaStream*>(stream_.Get()));
    }
    stream_->setState(true);
    events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &position);
    (void)descriptor;
    return S_OK;
}

IFACEMETHODIMP VCamSource::Stop() {
    EnterCriticalSection(&lock_);
    if (shutdown_) { LeaveCriticalSection(&lock_); return MF_E_SHUTDOWN; }
    LeaveCriticalSection(&lock_);

    logf("VCamSource::Stop");
    channel_.setActive(false);
    stream_->setState(false);
    events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

IFACEMETHODIMP VCamSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

IFACEMETHODIMP VCamSource::Shutdown() {
    EnterCriticalSection(&lock_);
    if (shutdown_) { LeaveCriticalSection(&lock_); return S_OK; }
    shutdown_ = true;
    LeaveCriticalSection(&lock_);

    if (stream_) { stream_->shutdown(); stream_.Reset(); }
    channel_.close();
    if (events_) { events_->Shutdown(); events_.Reset(); }
    presentation_.Reset();
    descriptor_.Reset();
    if (mfStarted_) { MFShutdown(); mfStarted_ = false; }
    return S_OK;
}

IFACEMETHODIMP VCamSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    if (!attributes_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

IFACEMETHODIMP VCamSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    if (streamId != 0 || !descriptor_) return MF_E_INVALIDSTREAMNUMBER;
    return descriptor_.CopyTo(IID_PPV_ARGS(attributes));
}

IFACEMETHODIMP VCamSource::SetD3DManager(IUnknown*) { return E_NOTIMPL; }

IFACEMETHODIMP VCamSource::GetService(REFGUID, REFIID riid, LPVOID* object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

IFACEMETHODIMP VCamSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return kNotSupported;
}
IFACEMETHODIMP VCamSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return kNotSupported;
}
IFACEMETHODIMP VCamSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return kNotSupported;
}

} // namespace awc
