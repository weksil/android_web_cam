#pragma once
#include <windows.h>

// COM/MF headers first: ks.h redefines GUID_NULL as a named GUID, which breaks
// cguid.h if it is pulled in afterwards.
#include <objbase.h>
#include <cguid.h>
#include <strmif.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>

#include <deque>
#include <vector>

#include "FrameChannel.h"

namespace awc {

constexpr uint32_t kCamWidth = 1920;
constexpr uint32_t kCamHeight = 1080;
constexpr uint32_t kCamFps = 30;

class VCamSource;

/** Single NV12 video stream fed from the shared frame channel. */
class VCamStream
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
          IKsControl> {
public:
    HRESULT init(VCamSource* source, IMFStreamDescriptor* descriptor);
    void shutdown();
    HRESULT setState(bool active);

    // IMFMediaEventGenerator
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                              const PROPVARIANT* value) override;

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    IFACEMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

private:
    static DWORD WINAPI threadProc(void* self);
    void loop();
    HRESULT deliver(IUnknown* token);
    void fillPlaceholder(std::vector<uint8_t>& frame) const;

    Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor_;
    VCamSource* source_ = nullptr;                 // weak, the source owns us

    CRITICAL_SECTION lock_{};
    std::deque<Microsoft::WRL::ComPtr<IUnknown>> requests_;
    std::vector<uint8_t> frame_;
    std::vector<uint8_t> latest_;

    HANDLE thread_ = nullptr;
    HANDLE wake_ = nullptr;
    HANDLE quit_ = nullptr;
    bool active_ = false;
    bool shutdown_ = false;
    LONGLONG start_ = 0;
    LONGLONG nextTime_ = 0;
    LONGLONG lastDelivery_ = 0;
    bool haveFrame_ = false;
    uint64_t requested_ = 0;
    uint64_t delivered_ = 0;
};

/** The COM object the Frame Server activates for our virtual camera. */
class VCamSource
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>,
          IMFGetService, IKsControl> {
public:
    VCamSource();
    ~VCamSource();

    HRESULT init();
    FrameChannel& channel() { return channel_; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override;

    // IMFMediaEventGenerator
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                              const PROPVARIANT* value) override;

    // IMFMediaSource
    IFACEMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    IFACEMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                         const PROPVARIANT* startPosition) override;
    IFACEMETHODIMP Stop() override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    IFACEMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    IFACEMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

    // IMFActivate - the frame server activates the CLSID and asks it for the source
    IFACEMETHODIMP ActivateObject(REFIID riid, void** object) override {
        return QueryInterface(riid, object);
    }
    IFACEMETHODIMP DetachObject() override { return S_OK; }
    IFACEMETHODIMP ShutdownObject() override { return Shutdown(); }

    // IMFAttributes - the device pipeline expects the source itself to be an attribute store
    IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attr()->GetItem(k, v); }
    IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attr()->GetItemType(k, t); }
    IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attr()->CompareItem(k, v, r); }
    IFACEMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return attr()->Compare(a, t, r); }
    IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attr()->GetUINT32(k, v); }
    IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attr()->GetUINT64(k, v); }
    IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return attr()->GetDouble(k, v); }
    IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attr()->GetGUID(k, v); }
    IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return attr()->GetStringLength(k, n); }
    IFACEMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* len) override { return attr()->GetString(k, s, n, len); }
    IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) override { return attr()->GetAllocatedString(k, s, n); }
    IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return attr()->GetBlobSize(k, n); }
    IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* got) override { return attr()->GetBlob(k, b, n, got); }
    IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return attr()->GetAllocatedBlob(k, b, n); }
    IFACEMETHODIMP GetUnknown(REFGUID k, REFIID iid, LPVOID* v) override { return attr()->GetUnknown(k, iid, v); }
    IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attr()->SetItem(k, v); }
    IFACEMETHODIMP DeleteItem(REFGUID k) override { return attr()->DeleteItem(k); }
    IFACEMETHODIMP DeleteAllItems() override { return attr()->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attr()->SetUINT32(k, v); }
    IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attr()->SetUINT64(k, v); }
    IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return attr()->SetDouble(k, v); }
    IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attr()->SetGUID(k, v); }
    IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attr()->SetString(k, v); }
    IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attr()->SetBlob(k, b, n); }
    IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* v) override { return attr()->SetUnknown(k, v); }
    IFACEMETHODIMP LockStore() override { return attr()->LockStore(); }
    IFACEMETHODIMP UnlockStore() override { return attr()->UnlockStore(); }
    IFACEMETHODIMP GetCount(UINT32* n) override { return attr()->GetCount(n); }
    IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attr()->GetItemByIndex(i, k, v); }
    IFACEMETHODIMP CopyAllItems(IMFAttributes* dst) override { return attr()->CopyAllItems(dst); }

private:
    IMFAttributes* attr() const { return attributes_.Get(); }
    HRESULT createMediaType(IMFMediaType** type) const;

    CRITICAL_SECTION lock_{};
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentation_;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor_;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
    Microsoft::WRL::ComPtr<VCamStream> stream_;
    FrameChannel channel_;
    bool started_ = false;
    bool shutdown_ = false;
    bool mfStarted_ = false;
};

} // namespace awc
