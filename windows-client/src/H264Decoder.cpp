#include "H264Decoder.h"

#include <strmif.h>          // ICodecAPI
#include <codecapi.h>
#include <mferror.h>

#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

using Microsoft::WRL::ComPtr;

namespace awc {

namespace {

void fail(std::string* err, const char* what, HRESULT hr) {
    if (!err) return;
    char buf[160];
    sprintf_s(buf, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    *err = buf;
}

} // namespace

H264Decoder::~H264Decoder() { shutdown(); }

bool H264Decoder::init(uint32_t width, uint32_t height, uint32_t fps, std::string* err) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { fail(err, "MFStartup", hr); return false; }
    mfStarted_ = true;

    if (!createDevice(err)) return false;
    if (!createTransform(err)) return false;
    if (!configureTypes(width, height, fps, err)) return false;

    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    started_ = true;
    return true;
}

void H264Decoder::shutdown() {
    if (transform_ && started_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    started_ = false;
    staging_.Reset();
    transform_.Reset();
    dxgiManager_.Reset();
    context_.Reset();
    device_.Reset();
    if (mfStarted_) { MFShutdown(); mfStarted_ = false; }
}

bool H264Decoder::createDevice(std::string* err) {
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    };
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   device_.GetAddressOf(), nullptr, context_.GetAddressOf());
    if (FAILED(hr)) { fail(err, "D3D11CreateDevice", hr); return false; }

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    hr = MFCreateDXGIDeviceManager(&resetToken_, dxgiManager_.GetAddressOf());
    if (FAILED(hr)) { fail(err, "MFCreateDXGIDeviceManager", hr); return false; }
    hr = dxgiManager_->ResetDevice(device_.Get(), resetToken_);
    if (FAILED(hr)) { fail(err, "IMFDXGIDeviceManager::ResetDevice", hr); return false; }
    return true;
}

bool H264Decoder::createTransform(std::string* err) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                               MFT_ENUM_FLAG_SORTANDFILTER,
                           &input, nullptr, &activates, &count);
    if (FAILED(hr) || count == 0) {
        fail(err, "MFTEnumEx(H264 decoder)", FAILED(hr) ? hr : E_FAIL);
        return false;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(transform_.GetAddressOf()));
    LPWSTR name = nullptr;
    if (SUCCEEDED(activates[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, nullptr))) {
        char utf8[256]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        name_ = utf8;
        CoTaskMemFree(name);
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) { fail(err, "ActivateObject", hr); return false; }

    hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                    reinterpret_cast<ULONG_PTR>(dxgiManager_.Get()));
    if (FAILED(hr)) name_ += " (software)";

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(transform_->GetAttributes(attrs.GetAddressOf())) && attrs) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
    }
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform_.As(&codec))) {
        VARIANT v{};
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
    return true;
}

bool H264Decoder::configureTypes(uint32_t width, uint32_t height, uint32_t fps, std::string* err) {
    ComPtr<IMFMediaType> in;
    HRESULT hr = MFCreateMediaType(in.GetAddressOf());
    if (FAILED(hr)) { fail(err, "MFCreateMediaType", hr); return false; }

    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) { fail(err, "SetInputType", hr); return false; }

    return selectOutputType(err);
}

bool H264Decoder::selectOutputType(std::string* err) {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> out;
        HRESULT hr = transform_->GetOutputAvailableType(0, i, out.GetAddressOf());
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) { fail(err, "GetOutputAvailableType", hr); return false; }

        GUID subtype{};
        out->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype != MFVideoFormat_NV12) continue;

        hr = transform_->SetOutputType(0, out.Get(), 0);
        if (FAILED(hr)) { fail(err, "SetOutputType(NV12)", hr); return false; }

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, &w, &h);
        bufW_ = w;
        bufH_ = h;
        outW_ = w;
        outH_ = h;

        // The coded frame is padded to a macroblock multiple; the aperture is the
        // part that is actually meant to be shown (1920x1088 -> 1920x1080).
        MFVideoArea area{};
        UINT32 blob = 0;
        if (SUCCEEDED(out->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                   reinterpret_cast<UINT8*>(&area), sizeof(area), &blob)) &&
            blob == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
            outW_ = UINT32(area.Area.cx);
            outH_ = UINT32(area.Area.cy);
        }
        outH_ &= ~1u;   // NV12 needs an even height

        MFT_OUTPUT_STREAM_INFO info{};
        transform_->GetOutputStreamInfo(0, &info);
        providesSamples_ =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                             MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        staging_.Reset();
        return true;
    }
    fail(err, "no NV12 output type", E_FAIL);
    return false;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, int64_t ptsHns, const OnFrame& onFrame) {
    if (!transform_) return false;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), buffer.GetAddressOf());
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    DWORD maxLen = 0;
    if (FAILED(buffer->Lock(&dst, &maxLen, nullptr))) return false;
    std::memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(sample.GetAddressOf()))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(ptsHns);

    hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        pullOutput(onFrame);
        hr = transform_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return false;

    pullOutput(onFrame);
    return true;
}

void H264Decoder::pullOutput(const OnFrame& onFrame) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        transform_->GetOutputStreamInfo(0, &info);

        MFT_OUTPUT_DATA_BUFFER out{};
        ComPtr<IMFSample> sample;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateMemoryBuffer(info.cbSize, buffer.GetAddressOf()))) return;
            if (FAILED(MFCreateSample(sample.GetAddressOf()))) return;
            sample->AddBuffer(buffer.Get());
            out.pSample = sample.Get();
        }

        DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (out.pSample && providesSamples_) out.pSample->Release();
            std::string ignored;
            if (!selectOutputType(&ignored)) return;
            continue;
        }
        if (FAILED(hr)) return;

        IMFSample* result = out.pSample;
        if (result) {
            if (onFrame) onFrame(result);
            if (providesSamples_) result->Release();
        }
        if (out.pEvents) out.pEvents->Release();
    }
}

void H264Decoder::flush() {
    if (transform_) transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

bool H264Decoder::copyNv12(IMFSample* sample, std::vector<uint8_t>& dst) {
    if (!sample || outW_ == 0 || outH_ == 0) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) return false;

    const size_t needed = size_t(outW_) * outH_ * 3 / 2;
    dst.resize(needed);

    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(buffer.As(&dxgi))) {
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(dxgi->GetResource(IID_PPV_ARGS(texture.GetAddressOf())))) return false;
        UINT subresource = 0;
        dxgi->GetSubresourceIndex(&subresource);

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!staging_) {
            D3D11_TEXTURE2D_DESC s = desc;
            s.ArraySize = 1;
            s.Usage = D3D11_USAGE_STAGING;
            s.BindFlags = 0;
            s.MiscFlags = 0;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&s, nullptr, staging_.GetAddressOf()))) return false;
        }
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture.Get(), subresource, nullptr);

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
        const uint8_t* src = static_cast<const uint8_t*>(map.pData);
        copyPlanes(src, map.RowPitch, desc.Height, dst.data());
        context_->Unmap(staging_.Get(), 0);
        return true;
    }

    ComPtr<IMF2DBuffer> two;
    if (SUCCEEDED(buffer.As(&two))) {
        BYTE* scan = nullptr;
        LONG pitch = 0;
        if (FAILED(two->Lock2D(&scan, &pitch))) return false;
        copyPlanes(scan, uint32_t(pitch), bufH_, dst.data());
        two->Unlock2D();
        return true;
    }

    BYTE* src = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (FAILED(buffer->Lock(&src, &maxLen, &curLen))) return false;
    copyPlanes(src, bufW_, bufH_, dst.data());
    buffer->Unlock();
    return true;
}

/** Crops the padded NV12 surface down to the visible aperture. */
void H264Decoder::copyPlanes(const uint8_t* src, uint32_t pitch, uint32_t allocatedHeight,
                             uint8_t* dst) const {
    for (uint32_t y = 0; y < outH_; ++y)
        std::memcpy(dst + size_t(y) * outW_, src + size_t(y) * pitch, outW_);

    const uint8_t* chroma = src + size_t(pitch) * allocatedHeight;
    uint8_t* dstChroma = dst + size_t(outW_) * outH_;
    for (uint32_t y = 0; y < outH_ / 2; ++y)
        std::memcpy(dstChroma + size_t(y) * outW_, chroma + size_t(y) * pitch, outW_);
}

} // namespace awc
