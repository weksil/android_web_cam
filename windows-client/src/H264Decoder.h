#pragma once
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace awc {

/**
 * H.264 -> NV12 via the Media Foundation decoder MFT, GPU accelerated through a
 * DXGI device manager (DXVA2 / D3D11), configured for low latency.
 */
class H264Decoder {
public:
    using OnFrame = std::function<void(IMFSample*)>;

    ~H264Decoder();

    /** [hevc] selects the HEVC decoder instead of H.264. */
    bool init(uint32_t width, uint32_t height, uint32_t fps, bool hevc, std::string* err);
    void shutdown();

    /** Feeds one Annex-B access unit. [ptsHns] is in 100-ns units. */
    bool decode(const uint8_t* data, size_t size, int64_t ptsHns, const OnFrame& onFrame);
    void flush();

    uint32_t width() const { return outW_; }
    uint32_t height() const { return outH_; }
    const std::string& decoderName() const { return name_; }
    ID3D11Device* device() const { return device_.Get(); }

    /** Copies an NV12 sample (D3D-backed or system memory) into [dst], tightly packed. */
    bool copyNv12(IMFSample* sample, std::vector<uint8_t>& dst);

private:
    bool createDevice(std::string* err);
    bool createTransform(std::string* err);
    bool configureTypes(uint32_t width, uint32_t height, uint32_t fps, std::string* err);
    GUID inputSubtype() const;
    bool selectOutputType(std::string* err);
    void pullOutput(const OnFrame& onFrame);
    void copyPlanes(const uint8_t* src, uint32_t pitch, uint32_t allocatedHeight, uint8_t* dst) const;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    UINT resetToken_ = 0;
    uint32_t outW_ = 0;      // visible size (display aperture)
    uint32_t outH_ = 0;
    uint32_t bufW_ = 0;      // allocated size; MF pads the height to a multiple of 16
    uint32_t bufH_ = 0;
    bool providesSamples_ = false;
    bool started_ = false;
    bool mfStarted_ = false;
    bool hevc_ = false;
    std::string name_;
};

} // namespace awc
