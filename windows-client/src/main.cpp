#include "Session.h"          // must precede windows.h (winsock2)

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CameraIds.h"
#include "Depacketizer.h"
#include "H264Decoder.h"
#include "Log.h"
#include "ExposureControl.h"
#include "ProducerChannel.h"
#include "Stabilizer.h"
#include "Warp.h"

#include <deque>
#include <unordered_map>

#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace awc;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr int kMenuExit = 100;
constexpr int kMenuFlipH = 101;
constexpr int kMenuFlipV = 102;
constexpr int kMenuStabilize = 103;
constexpr int kMenuShortExposure = 104;
constexpr int kMenuLevel = 105;

std::atomic<bool> g_stabilize{false};
std::atomic<bool> g_shortExposure{false};
std::atomic<bool> g_level{false};
std::atomic<float> g_levelDeg{0.0f};
std::atomic<float> g_stabAngle{0.0f};
std::atomic<int> g_iso{0};
std::atomic<int> g_exposureUs{0};

constexpr wchar_t kSettingsKey[] = L"Software\\AndroidWebCam";

std::atomic<bool> g_flipH{false};
std::atomic<bool> g_flipV{false};
std::atomic<bool> g_quit{false};

DWORD loadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, size = sizeof(value);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size)
            != ERROR_SUCCESS) {
            value = fallback;
        }
        RegCloseKey(key);
    }
    return value;
}

void saveSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

struct Options {
    std::string ip;
    std::string dump;
    std::string frame;
    int seconds = 0;
    uint16_t port = kRtpPort;
    bool test = false;
    bool remove = false;
    bool list = false;
    bool capture = false;
    bool gyroCheck = false;
    bool warpTest = false;
    bool levelTest = false;
    std::string analyze;
    std::wstring selftest;
};

void attachConsole() {
    const HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdOut || stdOut == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
}

/**
 * Loads the freshly built source DLL directly (no registry, no frame server) and
 * exercises the media source the same way the pipeline does. Isolates "is our COM
 * object well formed" from "is it installed correctly".
 */
int runSelfTest(const std::wstring& dllPath) {
    attachConsole();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    HMODULE dll = LoadLibraryW(dllPath.c_str());
    if (!dll) { printf("LoadLibrary(%ls) failed: %lu\n", dllPath.c_str(), GetLastError()); return 1; }

    using GetClassObject = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(dll, "DllGetClassObject"));
    if (!getClassObject) { printf("no DllGetClassObject export\n"); return 1; }

    CLSID clsid{};
    CLSIDFromString(kSourceClsidString, &clsid);

    ComPtr<IClassFactory> factory;
    HRESULT hr = getClassObject(clsid, IID_PPV_ARGS(factory.GetAddressOf()));
    printf("DllGetClassObject -> 0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 1;

    ComPtr<IMFMediaSource> source;
    hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(source.GetAddressOf()));
    printf("CreateInstance(IMFMediaSource) -> 0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 1;

    struct Probe { const char* name; IID iid; };
    const Probe probes[] = {
        {"IMFMediaSource", __uuidof(IMFMediaSource)},
        {"IMFMediaSourceEx", __uuidof(IMFMediaSourceEx)},
        {"IMFMediaEventGenerator", __uuidof(IMFMediaEventGenerator)},
        {"IMFGetService", __uuidof(IMFGetService)},
        {"IMFAttributes", __uuidof(IMFAttributes)},
        {"IMFSampleAllocatorControl", __uuidof(IMFSampleAllocatorControl)},
        {"IMFActivate", __uuidof(IMFActivate)},
    };
    for (const auto& p : probes) {
        ComPtr<IUnknown> unknown;
        const HRESULT qi = source->QueryInterface(p.iid, &unknown);
        printf("  QI %-26s -> 0x%08lX%s\n", p.name, static_cast<unsigned long>(qi),
               SUCCEEDED(qi) ? "" : "   <-- not supported");
    }

    ComPtr<IMFActivate> activate;
    if (SUCCEEDED(source.As(&activate))) {
        ComPtr<IMFMediaSource> activated;
        const HRESULT ar = activate->ActivateObject(IID_PPV_ARGS(activated.GetAddressOf()));
        printf("ActivateObject(IMFMediaSource) -> 0x%08lX\n", static_cast<unsigned long>(ar));
    }

    ComPtr<IMFPresentationDescriptor> presentation;
    hr = source->CreatePresentationDescriptor(presentation.GetAddressOf());
    printf("CreatePresentationDescriptor -> 0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 1;

    PROPVARIANT start{};
    start.vt = VT_EMPTY;
    hr = source->Start(presentation.Get(), nullptr, &start);
    printf("Start -> 0x%08lX\n", static_cast<unsigned long>(hr));

    ComPtr<IMFMediaStream> stream;
    for (int i = 0; i < 20 && !stream; ++i) {
        ComPtr<IMFMediaEvent> event;
        if (FAILED(source->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.GetAddressOf()))) { Sleep(50); continue; }
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == MENewStream || type == MEUpdatedStream) {
            PROPVARIANT value{};
            if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal)
                value.punkVal->QueryInterface(IID_PPV_ARGS(stream.GetAddressOf()));
            PropVariantClear(&value);
        }
    }
    printf("MENewStream -> %s\n", stream ? "got the stream" : "NOT RECEIVED");
    if (!stream) return 1;

    for (int i = 0; i < 3; ++i) {
        const HRESULT rs = stream->RequestSample(nullptr);
        if (FAILED(rs)) printf("RequestSample -> 0x%08lX\n", static_cast<unsigned long>(rs));
    }
    int samples = 0;
    for (int i = 0; i < 40 && samples < 3; ++i) {
        ComPtr<IMFMediaEvent> event;
        if (FAILED(stream->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.GetAddressOf()))) { Sleep(50); continue; }
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == MEMediaSample) samples++;
    }
    printf("samples delivered: %d\n", samples);

    source->Shutdown();
    MFShutdown();
    return samples > 0 ? 0 : 1;
}

/** Opens the virtual camera the way a normal Media Foundation app does. */
int runCapture(const std::wstring& match, const std::string& framePath) {
    attachConsole();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(attributes.GetAddressOf(), 1);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    MFEnumDeviceSources(attributes.Get(), &devices, &count);

    ComPtr<IMFMediaSource> source;
    for (UINT32 i = 0; i < count; ++i) {
        LPWSTR name = nullptr;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, nullptr))) {
            if (wcsstr(name, match.c_str())) {
                printf("activating: %ls\n", name);
                const HRESULT hr = devices[i]->ActivateObject(IID_PPV_ARGS(source.GetAddressOf()));
                printf("ActivateObject -> 0x%08lX\n", static_cast<unsigned long>(hr));
            }
            CoTaskMemFree(name);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    if (!source) { printf("device not found\n"); return 1; }

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, reader.GetAddressOf());
    printf("MFCreateSourceReaderFromMediaSource -> 0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 1;

    ComPtr<IMFMediaType> type;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, type.GetAddressOf()))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        printf("current type: %ux%u  subtype.Data1=0x%08lX\n", w, h,
               static_cast<unsigned long>(subtype.Data1));
    }

    constexpr int kWanted = 180;      // ~6 s, enough for the phone to open its camera
    constexpr int kSaveAt = 150;
    int good = 0;
    for (int i = 0; i < kWanted * 2 && good < kWanted; ++i) {
        DWORD streamFlags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &streamFlags,
                                &timestamp, sample.GetAddressOf());
        if (FAILED(hr)) { printf("ReadSample -> 0x%08lX\n", static_cast<unsigned long>(hr)); break; }
        if (!sample) continue;
        good++;

        if (!framePath.empty() && good == kSaveAt) {
            ComPtr<IMFMediaBuffer> buffer;
            sample->ConvertToContiguousBuffer(buffer.GetAddressOf());
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (buffer && SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen))) {
                FILE* f = nullptr;
                fopen_s(&f, framePath.c_str(), "wb");
                if (f) { fwrite(data, 1, curLen, f); fclose(f); }
                printf("saved frame %d: %lu bytes -> %s\n", good, curLen, framePath.c_str());
                buffer->Unlock();
            }
        }
    }
    printf("samples read: %d\n", good);

    reader.Reset();
    source->Shutdown();
    MFShutdown();
    return good > 0 ? 0 : 1;
}

/**
 * Validates the gyro-to-image mapping: prints the frame-to-frame shift the gyro
 * predicts next to the shift actually measured from the pictures. Matching signs and
 * magnitudes mean the axis mapping, intrinsics and time alignment are all correct;
 * mirrored signs would mean the correction doubles the shake instead of cancelling it.
 *
 * Pan the phone slowly by hand while this runs.
 */
int runGyroCheck(const Options& options) {
    attachConsole();

    Session session;
    std::string err;
    if (!session.init(options.port, &err)) { printf("init: %s\n", err.c_str()); return 1; }
    std::string ip = options.ip;
    if (ip.empty()) {
        auto phones = session.discover(1500);
        if (phones.empty()) { printf("no phone found\n"); return 1; }
        ip = phones.front().ip;
    }
    if (!session.connect(ip, 3000)) { printf("no HELLO_ACK\n"); return 1; }

    const StreamParams& p = session.params();
    H264Decoder decoder;
    if (!decoder.init(p.width, p.height, p.fps ? p.fps : 30, &err)) {
        printf("decoder: %s\n", err.c_str());
        return 1;
    }

    CameraGeometry geometry;
    int64_t skewNs = 0;
    std::unordered_map<uint32_t, int64_t> frameTimes;
    std::vector<uint8_t> nv12;

    struct RawSample { int64_t t; float x, y, z; };
    std::vector<RawSample> rawGyro;              // device axes, deliberately unmapped
    struct Shot {
        uint32_t ts = 0;
        std::vector<float> columns, rows, rowsLeft, rowsRight, columnsTop, columnsBottom;
    };
    std::deque<Shot> shots;                      // decoded frames waiting for their metadata
    struct Motion {
        int64_t from = 0, to = 0, skew = 0;
        float dx = 0, dy = 0, roll = 0, shear = 0;
    };
    std::vector<Motion> motions;
    Shot previous;
    int64_t previousTime = 0;

    // Global-shift estimate from column and row intensity profiles. Every row must be
    // filled: leaving gaps turns the profile into a comb and the correlation into noise.
    // The mean is removed so the peak reflects structure rather than overall brightness.
    auto profile = [](const uint8_t* luma, uint32_t w, uint32_t h,
                      std::vector<float>& cols, std::vector<float>& rws,
                      std::vector<float>& rowsLeft, std::vector<float>& rowsRight,
                      std::vector<float>& colsTop, std::vector<float>& colsBottom) {
        cols.assign(w, 0.f);
        rws.assign(h, 0.f);
        rowsLeft.assign(h, 0.f);
        rowsRight.assign(h, 0.f);
        colsTop.assign(w, 0.f);
        colsBottom.assign(w, 0.f);
        const uint32_t third = w / 3;
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* row = luma + size_t(y) * w;
            float sum = 0, left = 0, right = 0;
            for (uint32_t x = 0; x < w; ++x) {
                cols[x] += row[x];
                sum += row[x];
                if (x < third) left += row[x];
                else if (x >= w - third) right += row[x];
            }
            rws[y] = sum;
            rowsLeft[y] = left;
            rowsRight[y] = right;
            // Column profiles of the top and bottom thirds: their relative shift is the
            // rolling-shutter shear.
            std::vector<float>& band = y < h / 3 ? colsTop : colsBottom;
            if (y < h / 3 || y >= h - h / 3) {
                for (uint32_t x = 0; x < w; ++x) band[x] += row[x];
            }
        }
        for (std::vector<float>* p : {&cols, &rws, &rowsLeft, &rowsRight, &colsTop, &colsBottom}) {
            double mean = 0;
            for (float v : *p) mean += v;
            mean /= double(p->size());
            for (float& v : *p) v -= float(mean);
        }
    };

    // Normalised cross-correlation over the central part of the profile.
    auto bestShift = [](const std::vector<float>& a, const std::vector<float>& b, int range) {
        const int n = int(a.size());
        const int margin = (std::max)(range, n / 10);
        int best = 0;
        double bestScore = -1e30;
        for (int s = -range; s <= range; ++s) {
            double dot = 0, sumA = 0, sumB = 0;
            for (int i = margin; i < n - margin; ++i) {
                const int j = i + s;
                if (j < 0 || j >= n) continue;
                dot += double(a[i]) * double(b[j]);
                sumA += double(a[i]) * a[i];
                sumB += double(b[j]) * b[j];
            }
            if (sumA <= 0 || sumB <= 0) continue;
            const double score = dot / std::sqrt(sumA * sumB);
            if (score > bestScore) { bestScore = score; best = s; }
        }
        return best;
    };

    session.setDeviceStabilization(false);      // we need unstabilized frames
    session.streamStart();
    printf("move the phone by hand for %d seconds - pan left/right and up/down\n",
           options.seconds > 0 ? options.seconds : 15);

    Depacketizer depack;
    depack.setCallback([&](const uint8_t* data, size_t size, uint32_t ts, bool) {
        decoder.decode(data, size, int64_t(ts) * 1000 / 9, [&](IMFSample* sample) {
            if (!decoder.copyNv12(sample, nv12)) return;
            Shot shot;
            shot.ts = ts;
            profile(nv12.data(), decoder.width(), decoder.height(), shot.columns, shot.rows,
                    shot.rowsLeft, shot.rowsRight, shot.columnsTop, shot.columnsBottom);
            shots.push_back(std::move(shot));    // metadata arrives after the frame data
        });
    });

    // Pairs up frames whose metadata has landed and measures the shift between them.
    auto measurePending = [&]() {
        while (!shots.empty()) {
            const auto it = frameTimes.find(shots.front().ts);
            if (it == frameTimes.end()) {
                if (shots.size() < 4) break;      // give the metadata a moment to arrive
                shots.pop_front();
                continue;
            }
            Shot shot = std::move(shots.front());
            shots.pop_front();
            const int64_t time = it->second;
            if (!previous.columns.empty() && previousTime != 0) {
                Motion m;
                m.from = previousTime;
                m.to = time;
                m.dx = float(bestShift(previous.columns, shot.columns, 64));
                m.dy = float(bestShift(previous.rows, shot.rows, 64));
                // Image rotation shows up as the left and right thirds shifting
                // vertically by different amounts.
                const float left = float(bestShift(previous.rowsLeft, shot.rowsLeft, 64));
                const float right = float(bestShift(previous.rowsRight, shot.rowsRight, 64));
                const float span = float(geometry.width) * 2.0f / 3.0f;
                m.roll = span > 0 ? (right - left) / span : 0.f;
                m.shear = float(bestShift(previous.columnsTop, shot.columnsTop, 64)) -
                          float(bestShift(previous.columnsBottom, shot.columnsBottom, 64));
                m.skew = skewNs;
                motions.push_back(m);
            }
            previous = std::move(shot);
            previousTime = time;
        }
    };

    const uint64_t start = nowMs();
    const int seconds = options.seconds > 0 ? options.seconds : 15;
    while (!g_quit && nowMs() - start < uint64_t(seconds) * 1000) {
        session.poll(50, [&](const uint8_t* pkt, int len) {
            if (len >= 12 && (pkt[1] & 0x7F) == kRtpPayloadType) {
                depack.push(pkt, len);
                return;
            }
            if (len < 5 || std::memcmp(pkt, kMagic, 4) != 0) return;
            const uint8_t* body = pkt + 5;
            auto readI32 = [](const uint8_t* d) {
                return int32_t((uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) |
                               (uint32_t(d[2]) << 8) | d[3]);
            };
            auto readI64 = [](const uint8_t* d) {
                int64_t v = 0;
                for (int i = 0; i < 8; ++i) v = (v << 8) | d[i];
                return v;
            };
            auto readF32 = [&](const uint8_t* d) {
                const int32_t bits = readI32(d);
                float f;
                std::memcpy(&f, &bits, 4);
                return f;
            };
            if (pkt[4] == kCamInfo && len >= 30) {
                geometry.fx = readF32(body); geometry.fy = readF32(body + 4);
                geometry.cx = readF32(body + 8); geometry.cy = readF32(body + 12);
                geometry.width = uint16_t((body[16] << 8) | body[17]);
                geometry.height = uint16_t((body[18] << 8) | body[19]);
                geometry.orientation = readI32(body + 20);
                geometry.facing = body[24];
            } else if (pkt[4] == kGyro && len >= 6) {
                const int count = body[0];
                for (int i = 0; i < count; ++i) {
                    const uint8_t* s = body + 1 + i * 20;
                    if (5 + 1 + (i + 1) * 20 > len) break;
                    rawGyro.push_back({readI64(s), readF32(s + 8), readF32(s + 12), readF32(s + 16)});
                }
            } else if (pkt[4] == kFrameMeta && len >= 33) {
                frameTimes[uint32_t(readI32(body))] = readI64(body + 4);
                skewNs = readI64(body + 20);
                if (frameTimes.size() > 600) frameTimes.clear();
            }
        });
        measurePending();
    }
    session.streamStop();
    session.bye();

    printf("\nframe pairs measured: %zu, gyro samples: %zu\n", motions.size(), rawGyro.size());
    if (!motions.empty() && !rawGyro.empty()) {
        printf("frame times: %lld .. %lld ns\n", (long long)motions.front().from,
               (long long)motions.back().to);
        printf("gyro  times: %lld .. %lld ns\n", (long long)rawGyro.front().t,
               (long long)rawGyro.back().t);
        const long long gap = (long long)rawGyro.front().t - (long long)motions.front().from;
        printf("clock gap between the two: %lld ns (%.3f s) - must be near zero\n", gap,
               double(gap) / 1e9);
    }
    if (motions.size() < 15 || rawGyro.size() < 100 || !geometry.valid()) {
        printf("not enough data: need a moving phone, gyro telemetry and camera info\n");
        decoder.shutdown();
        return 1;
    }

    // Integrated device-axis rotation over an interval, small-angle and unmapped: the
    // point of this tool is to discover the mapping rather than assume it.
    auto integrate = [&](int64_t from, int64_t to, double out[3]) {
        out[0] = out[1] = out[2] = 0;
        for (size_t i = 1; i < rawGyro.size(); ++i) {
            const RawSample& a = rawGyro[i - 1];
            const RawSample& b = rawGyro[i];
            if (b.t <= from || a.t >= to) continue;
            const double span = double(b.t - a.t);
            if (span <= 0 || span > 1e8) continue;
            const double overlap = double((std::min)(b.t, to) - (std::max)(a.t, from));
            if (overlap <= 0) continue;
            const double dt = overlap / 1e9;
            out[0] += a.x * dt;
            out[1] += a.y * dt;
            out[2] += a.z * dt;
        }
    };

    auto correlate = [](const std::vector<double>& a, const std::vector<double>& b) {
        const double n = double(a.size());
        double sa = 0, sb = 0, sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            sa += a[i]; sb += b[i]; sab += a[i] * b[i];
            saa += a[i] * a[i]; sbb += b[i] * b[i];
        }
        const double cov = sab / n - (sa / n) * (sb / n);
        const double va = saa / n - (sa / n) * (sa / n);
        const double vb = sbb / n - (sb / n) * (sb / n);
        struct Fit { double correlation, slope; };
        if (va <= 0 || vb <= 0) return Fit{0, 0};
        return Fit{cov / std::sqrt(va * vb), cov / vb};      // slope of a on b
    };

    std::vector<double> mx(motions.size()), my(motions.size());
    for (size_t i = 0; i < motions.size(); ++i) { mx[i] = motions[i].dx; my[i] = motions[i].dy; }
    double rmsX = 0, rmsY = 0;
    for (size_t i = 0; i < mx.size(); ++i) { rmsX += mx[i] * mx[i]; rmsY += my[i] * my[i]; }
    printf("measured motion rms: %.1f px horizontal, %.1f px vertical\n",
           std::sqrt(rmsX / mx.size()), std::sqrt(rmsY / my.size()));

    const char* axisName[3] = {"gyro x", "gyro y", "gyro z"};
    int bestOffset = 0;
    double bestScore = -1;
    printf("\n  offset |   dx vs gx    dx vs gy    dx vs gz |   dy vs gx    dy vs gy    dy vs gz\n");
    for (int offsetMs = -60; offsetMs <= 60; offsetMs += 10) {
        std::vector<double> g[3] = {std::vector<double>(motions.size()),
                                    std::vector<double>(motions.size()),
                                    std::vector<double>(motions.size())};
        for (size_t i = 0; i < motions.size(); ++i) {
            double w[3];
            const int64_t shift = int64_t(offsetMs) * 1'000'000;
            integrate(motions[i].from + shift, motions[i].to + shift, w);
            for (int k = 0; k < 3; ++k) g[k][i] = w[k];
        }
        double cx[3], cy[3];
        for (int k = 0; k < 3; ++k) {
            cx[k] = correlate(mx, g[k]).correlation;
            cy[k] = correlate(my, g[k]).correlation;
        }
        double score = 0;
        for (int k = 0; k < 3; ++k) {
            score = (std::max)(score, std::fabs(cx[k]));
            score = (std::max)(score, std::fabs(cy[k]));
        }
        printf("%+5d ms | %+10.2f  %+10.2f  %+10.2f | %+10.2f  %+10.2f  %+10.2f\n", offsetMs,
               cx[0], cx[1], cx[2], cy[0], cy[1], cy[2]);
        if (score > bestScore) { bestScore = score; bestOffset = offsetMs; }
    }

    // Report the mapping the data implies at the best offset.
    std::vector<double> g[3] = {std::vector<double>(motions.size()),
                                std::vector<double>(motions.size()),
                                std::vector<double>(motions.size())};
    for (size_t i = 0; i < motions.size(); ++i) {
        double w[3];
        integrate(motions[i].from + int64_t(bestOffset) * 1'000'000,
                  motions[i].to + int64_t(bestOffset) * 1'000'000, w);
        for (int k = 0; k < 3; ++k) g[k][i] = w[k];
    }
    printf("\nbest time offset: %+d ms\n", bestOffset);
    for (int axis = 0; axis < 2; ++axis) {
        const std::vector<double>& measured = axis == 0 ? mx : my;
        int bestAxis = 0;
        double bestCorrelation = 0, bestSlope = 0;
        for (int k = 0; k < 3; ++k) {
            const auto fit = correlate(measured, g[k]);
            if (std::fabs(fit.correlation) > std::fabs(bestCorrelation)) {
                bestCorrelation = fit.correlation;
                bestSlope = fit.slope;
                bestAxis = k;
            }
        }
        printf("image %s shift follows %s: correlation %+.2f, slope %+.0f px/rad (fx=%.0f)\n",
               axis == 0 ? "horizontal" : "vertical  ", axisName[bestAxis], bestCorrelation,
               bestSlope, axis == 0 ? geometry.fx : geometry.fy);
    }
    // Roll cannot be read off a translation estimate, so it gets its own comparison.
    std::vector<double> rolls(motions.size());
    for (size_t i = 0; i < motions.size(); ++i) rolls[i] = motions[i].roll;
    const auto rollFit = correlate(rolls, g[2]);
    printf("image roll follows gyro z: correlation %+.2f, slope %+.2f\n",
           rollFit.correlation, rollFit.slope);

    // Rolling-shutter readout direction. The top and bottom of a frame are exposed
    // [skew] apart, so their frame-to-frame shifts differ; predicting that difference
    // from the gyro shows whether row 0 is read first (positive) or last (negative).
    std::vector<double> measuredShear, predictedShear;
    for (const Motion& m : motions) {
        if (m.skew <= 0) continue;
        double a[3], b[3];
        integrate(m.from, m.to, a);                       // between the two top rows
        integrate(m.from + m.skew, m.to + m.skew, b);     // between the two bottom rows
        measuredShear.push_back(m.shear);
        predictedShear.push_back(geometry.fx * (a[0] - b[0]));
    }
    if (measuredShear.size() > 20) {
        const auto shearFit = correlate(measuredShear, predictedShear);
        double rms = 0;
        for (double v : measuredShear) rms += v * v;
        printf("\nrolling shutter: skew %.1f ms, measured shear %.1f px rms\n",
               double(motions.back().skew) / 1e6, std::sqrt(rms / measuredShear.size()));
        printf("shear vs gyro prediction: correlation %+.2f, slope %+.2f\n",
               shearFit.correlation, shearFit.slope);
        printf("%s\n", shearFit.correlation > 0.4
                           ? "readout direction as assumed: row 0 exposed first"
                           : shearFit.correlation < -0.4
                                 ? "READOUT DIRECTION INVERTED - per-row correction is doubling "
                                   "the shear instead of removing it"
                                 : "inconclusive: needs a faster pan");
    }

    printf("\nexpected by the current mapping: horizontal +%.0f with gyro x, "
           "vertical %.0f with gyro y, roll positive with gyro z\n",
           geometry.fx, -geometry.fy);
    decoder.shutdown();
    return 0;
}

/**
 * Self-contained check of the rolling-shutter correction: builds a frame sheared exactly
 * the way a constant yaw shears it during readout, feeds the matching gyro track, runs
 * the production warp and measures what is left.
 *
 * Residual near zero means the row timing and its direction are right; residual near
 * twice the input means the correction is applied backwards.
 */
int runWarpTest() {
    attachConsole();
    constexpr uint32_t W = 2560, H = 1440;
    constexpr int64_t skew = 27'510'000;        // as measured on the phone
    constexpr float omegaCamY = 0.5f;           // rad/s of yaw during the readout

    CameraGeometry geometry;
    geometry.fx = geometry.fy = 1703.6f;
    geometry.cx = W / 2.0f;
    geometry.cy = H / 2.0f;
    geometry.width = W;
    geometry.height = H;
    geometry.orientation = 90;
    geometry.facing = 1;                        // back camera

    // Vertical stripes, each row displaced by the rotation that happened by the time
    // that row was read: dx = -fx * yaw(t), t = skew * row / (H - 1).
    std::vector<uint8_t> src(size_t(W) * H * 3 / 2, 128);
    for (uint32_t y = 0; y < H; ++y) {
        const float t = float(skew) * float(y) / float(H - 1) / 1e9f;
        const float dx = -geometry.fx * omegaCamY * t;
        uint8_t* row = src.data() + size_t(y) * W;
        for (uint32_t x = 0; x < W; ++x) {
            const float sample = float(x) - dx;
            row[x] = (int(std::floor(sample / 64.0f)) & 1) ? 210 : 40;
        }
    }

    // Gyro: y_cam maps to -x_device, so a yaw of omegaCamY is -omegaCamY on device x.
    const int64_t t0 = 1'000'000'000'000LL;
    Stabilizer stabilizer;
    stabilizer.setGeometry(geometry);
    stabilizer.setEnabled(true);
    for (int i = -400; i <= 400; ++i) {
        stabilizer.addGyro(t0 + int64_t(i) * 5'000'000, -omegaCamY, 0.0f, 0.0f);
    }

    constexpr uint32_t kBands = 33;
    constexpr float crop = 0.9f;
    Homography bands[kBands];
    stabilizer.correctionFor(t0 + skew / 2);
    for (uint32_t i = 0; i < kBands; ++i) {
        const float fraction = float(i) / float(kBands - 1);
        const Quat q = stabilizer.correctionAtRow(t0 + int64_t(fraction * float(skew)));
        bands[i] = stabilizationHomography(geometry, q, kMaxWidth, kMaxHeight, crop);
    }

    std::vector<uint8_t> out(size_t(kMaxWidth) * kMaxHeight * 3 / 2, 0);
    warpNv12Banded(src.data(), W, H, out.data(), kMaxWidth, kMaxHeight, bands, kBands, 8);

    // Shear inside one frame: how far the bottom third's stripes sit from the top's.
    auto shearOf = [](const uint8_t* luma, uint32_t w, uint32_t h) {
        std::vector<float> top(w, 0.f), bottom(w, 0.f);
        for (uint32_t y = 0; y < h / 3; ++y)
            for (uint32_t x = 0; x < w; ++x) top[x] += luma[size_t(y) * w + x];
        for (uint32_t y = h - h / 3; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) bottom[x] += luma[size_t(y) * w + x];
        for (std::vector<float>* p : {&top, &bottom}) {
            double mean = 0;
            for (float v : *p) mean += v;
            mean /= double(p->size());
            for (float& v : *p) v -= float(mean);
        }
        int best = 0;
        double bestScore = -1e30;
        for (int s = -60; s <= 60; ++s) {
            double dot = 0, a = 0, b = 0;
            for (uint32_t i = 60; i + 60 < w; ++i) {
                const int j = int(i) + s;
                if (j < 0 || j >= int(w)) continue;
                dot += double(top[i]) * bottom[j];
                a += double(top[i]) * top[i];
                b += double(bottom[j]) * bottom[j];
            }
            if (a <= 0 || b <= 0) continue;
            const double score = dot / std::sqrt(a * b);
            if (score > bestScore) { bestScore = score; best = s; }
        }
        return best;
    };

    const int sourceShear = shearOf(src.data(), W, H);
    const int outputShear = shearOf(out.data(), kMaxWidth, kMaxHeight);
    const float scale = float(kMaxWidth) / (float(W) * crop);
    printf("synthetic yaw %.2f rad/s, skew %.2f ms\n", omegaCamY, double(skew) / 1e6);
    printf("shear in the source frame : %d px\n", sourceShear);
    printf("expected in the output if uncorrected: %.0f px\n", float(sourceShear) * scale);
    printf("shear after the warp      : %d px\n", outputShear);
    const float expected = float(sourceShear) * scale;
    printf("\n%s\n", std::fabs(float(outputShear)) < std::fabs(expected) * 0.35f
                         ? "rolling-shutter correction WORKS"
                         : std::fabs(float(outputShear)) > std::fabs(expected) * 1.5f
                               ? "correction is INVERTED - it doubles the shear"
                               : "correction has little effect - check the row timing");
    return 0;
}

/**
 * Self-contained check of horizon levelling: renders a frame as a camera rolled by a
 * known angle would see it, feeds the matching gravity vector, and measures how level
 * the result comes out. Catches both a wrong angle and a wrong rotation direction.
 */
int runLevelTest() {
    attachConsole();
    constexpr uint32_t W = 2560, H = 1440;
    constexpr float tiltDeg = 5.0f;
    const float tilt = tiltDeg / 57.2957795f;

    CameraGeometry geometry;
    geometry.fx = geometry.fy = 1703.6f;
    geometry.cx = W / 2.0f;
    geometry.cy = H / 2.0f;
    geometry.width = W;
    geometry.height = H;
    geometry.orientation = 90;
    geometry.facing = 1;

    // A camera rolled by +tilt sees the horizon at -tilt, so draw the band that way.
    std::vector<uint8_t> src(size_t(W) * H * 3 / 2, 40);
    std::fill(src.begin() + size_t(W) * H, src.end(), 128);
    for (uint32_t x = 0; x < W; ++x) {
        const float centre = H / 2.0f - (float(x) - W / 2.0f) * std::tan(tilt);
        for (int dy = -12; dy <= 12; ++dy) {
            const int y = int(centre) + dy;
            if (y >= 0 && y < int(H)) src[size_t(y) * W + x] = 230;
        }
    }

    // Gravity in camera axes is (g*sin, g*cos, 0) for a roll; addGravity expects device
    // axes, so invert the mapping used for orientation 90 on the back camera.
    Stabilizer stabilizer;
    stabilizer.setGeometry(geometry);
    const float gx = 9.81f * std::sin(tilt), gy = 9.81f * std::cos(tilt);
    for (int i = 0; i < 200; ++i) stabilizer.addGravity(-gy, -gx, 0.0f);

    const float measuredRoll = stabilizer.horizonRoll();
    printf("tilt fed in: %+.2f deg, horizonRoll(): %+.2f deg\n", tiltDeg,
           measuredRoll * 57.2957795f);

    constexpr float crop = 0.9f;
    const Quat levelling = Stabilizer::rollQuat(-measuredRoll);
    const Quat q = normalize(conjugate(levelling) * Quat{});
    const Homography h = stabilizationHomography(geometry, q, kMaxWidth, kMaxHeight, crop);
    std::vector<uint8_t> out(size_t(kMaxWidth) * kMaxHeight * 3 / 2, 0);
    warpNv12(src.data(), W, H, out.data(), kMaxWidth, kMaxHeight, h, 8);

    // Angle of the bright band: vertical centre of mass on the left and right thirds.
    auto bandAngle = [](const uint8_t* luma, uint32_t w, uint32_t h) {
        auto centroid = [&](uint32_t fromX, uint32_t toX) {
            double weight = 0, sum = 0;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = fromX; x < toX; ++x) {
                    const double v = luma[size_t(y) * w + x] > 150 ? 1.0 : 0.0;
                    weight += v;
                    sum += v * y;
                }
            }
            return weight > 0 ? sum / weight : 0.0;
        };
        const double left = centroid(0, w / 3);
        const double right = centroid(w - w / 3, w);
        const double span = double(w) * 2.0 / 3.0;
        return std::atan2(right - left, span) * 57.2957795;
    };

    printf("band angle in the source: %+.2f deg\n", bandAngle(src.data(), W, H));
    const double after = bandAngle(out.data(), kMaxWidth, kMaxHeight);
    printf("band angle after levelling: %+.2f deg\n", after);
    printf("\n%s\n", std::fabs(after) < 1.0
                         ? "horizon levelling WORKS"
                         : std::fabs(after) > tiltDeg * 1.5
                               ? "ROTATION IS INVERTED - it doubles the tilt"
                               : "levelling incomplete - check the angle");
    return 0;
}

/**
 * Offline check on a recorded clip (Y4M on stdin or a file): separates the three things
 * that all look like "jitter" to the eye.
 *
 *   shear     - top and bottom of the frame shifting by different amounts, i.e. the
 *               rolling-shutter jello that per-row correction is meant to remove
 *   residual  - whole-frame shake left over after stabilization
 *   sharpness - mean gradient; low values mean motion blur from a long exposure,
 *               which no amount of stabilization can undo
 */
int runAnalyze(const std::string& path) {
    attachConsole();
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || !file) {
        printf("cannot open %s\n", path.c_str());
        return 1;
    }

    char header[256]{};
    if (!fgets(header, sizeof(header), file)) { printf("not a Y4M file\n"); fclose(file); return 1; }
    int width = 0, height = 0;
    if (const char* w = strstr(header, " W")) width = atoi(w + 2);
    if (const char* h = strstr(header, " H")) height = atoi(h + 2);
    if (width <= 0 || height <= 0) { printf("bad Y4M header: %s\n", header); fclose(file); return 1; }
    printf("%dx%d\n", width, height);

    const size_t lumaSize = size_t(width) * height;
    const size_t frameSize = lumaSize * 3 / 2;                 // 4:2:0
    std::vector<uint8_t> frame(frameSize), previous;

    auto columnProfile = [&](const uint8_t* luma, uint32_t fromRow, uint32_t toRow,
                             std::vector<float>& cols) {
        cols.assign(width, 0.f);
        for (uint32_t y = fromRow; y < toRow; ++y) {
            const uint8_t* row = luma + size_t(y) * width;
            for (int x = 0; x < width; ++x) cols[x] += row[x];
        }
        double mean = 0;
        for (float v : cols) mean += v;
        mean /= double(cols.size());
        for (float& v : cols) v -= float(mean);
    };
    // Returns the shift and how good the match was: on a blurred, low-contrast frame the
    // correlation peak is flat and the shift is meaningless, so the caller drops it.
    struct Match { int shift; double peak; };
    auto shift = [](const std::vector<float>& a, const std::vector<float>& b, int range) {
        const int n = int(a.size());
        const int margin = (std::max)(range, n / 10);
        Match best{0, -1e30};
        for (int s = -range; s <= range; ++s) {
            double dot = 0, sa = 0, sb = 0;
            for (int i = margin; i < n - margin; ++i) {
                const int j = i + s;
                if (j < 0 || j >= n) continue;
                dot += double(a[i]) * b[j];
                sa += double(a[i]) * a[i];
                sb += double(b[j]) * b[j];
            }
            if (sa <= 0 || sb <= 0) continue;
            const double score = dot / std::sqrt(sa * sb);
            if (score > best.peak) { best.peak = score; best.shift = s; }
        }
        return best;
    };
    auto sharpness = [&](const uint8_t* luma) {
        double sum = 0;
        for (int y = 1; y < height - 1; y += 2) {
            const uint8_t* row = luma + size_t(y) * width;
            for (int x = 1; x < width - 1; x += 2) sum += std::abs(int(row[x + 1]) - int(row[x - 1]));
        }
        return sum / (double(width) * height / 4.0);
    };

    std::vector<float> topPrev, bottomPrev, allPrev, top, bottom, all;
    double sumShear2 = 0, sumResidual2 = 0, sumSharp = 0;
    int pairs = 0, frames = 0, rejected = 0;
    printf("%6s %8s %8s %8s %10s\n", "frame", "dx top", "dx bot", "shear", "sharpness");

    while (true) {
        char tag[16]{};
        if (!fgets(tag, sizeof(tag), file)) break;
        if (strncmp(tag, "FRAME", 5) != 0) break;
        if (fread(frame.data(), 1, frameSize, file) != frameSize) break;
        frames++;

        const uint8_t* luma = frame.data();
        columnProfile(luma, 0, height / 3, top);
        columnProfile(luma, height * 2 / 3, height, bottom);
        columnProfile(luma, 0, height, all);
        const double sharp = sharpness(luma);
        sumSharp += sharp;

        if (!topPrev.empty()) {
            const Match top1 = shift(topPrev, top, 40);
            const Match bottom1 = shift(bottomPrev, bottom, 40);
            const Match all1 = shift(allPrev, all, 40);
            const double weakest = (std::min)((std::min)(top1.peak, bottom1.peak), all1.peak);
            if (weakest < 0.97) {
                rejected++;                        // no reliable structure to track
            } else {
                const int shear = top1.shift - bottom1.shift;
                sumShear2 += double(shear) * shear;
                sumResidual2 += double(all1.shift) * all1.shift;
                pairs++;
                if (pairs < 30)
                    printf("%6d %8d %8d %8d %10.1f\n", frames, top1.shift, bottom1.shift, shear,
                           sharp);
            }
        }
        topPrev = top;
        bottomPrev = bottom;
        allPrev = all;
    }
    fclose(file);

    if (pairs > 0) {
        printf("\nframes %d, pairs used %d, rejected as unreliable %d\n", frames, pairs, rejected);
        printf("residual whole-frame shake: %.1f px rms\n", std::sqrt(sumResidual2 / pairs));
        printf("rolling-shutter shear:      %.1f px rms (top vs bottom)\n",
               std::sqrt(sumShear2 / pairs));
        printf("sharpness:                  %.1f (below ~6 means heavy motion blur)\n",
               sumSharp / frames);
    }
    return 0;
}

/** Lists the video capture devices Media Foundation sees, virtual cameras included. */
int runList() {
    attachConsole();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(attributes.GetAddressOf(), 1);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
    printf("MFEnumDeviceSources -> 0x%08lX, %u device(s)\n", static_cast<unsigned long>(hr), count);
    for (UINT32 i = 0; i < count; ++i) {
        LPWSTR name = nullptr;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                     &name, nullptr))) {
            printf("  [%u] %ls\n", i, name);
            CoTaskMemFree(name);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    MFShutdown();
    return 0;
}

enum class LinkState { NoPhone, Linked, Streaming };

struct Status {
    LinkState state = LinkState::NoPhone;
    std::string phone;
    std::string error;
    uint32_t width = 0, height = 0, fps = 0;
    uint64_t kbps = 0, lost = 0, dropped = 0;
};

std::mutex g_statusLock;
Status g_status;
void setStatus(const Status& s) {
    std::lock_guard<std::mutex> guard(g_statusLock);
    g_status = s;
}

Status getStatus() {
    std::lock_guard<std::mutex> guard(g_statusLock);
    return g_status;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ------------------------------------------------------------------ tray icon

HICON makeDotIcon(COLORREF color) {
    constexpr int size = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP colorBitmap = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                           &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!colorBitmap) return LoadIcon(nullptr, IDI_APPLICATION);

    auto* px = static_cast<uint32_t*>(bits);
    const float center = (size - 1) / 2.0f, radius = 6.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x - center, dy = y - center;
            const float d = std::sqrt(dx * dx + dy * dy);
            float a = d <= radius ? 1.0f : (d <= radius + 1.0f ? (radius + 1.0f - d) : 0.0f);
            const auto alpha = uint32_t(a * 255.0f);
            const auto r = uint32_t(GetRValue(color) * a), g = uint32_t(GetGValue(color) * a),
                       b = uint32_t(GetBValue(color) * a);
            px[y * size + x] = (alpha << 24) | (r << 16) | (g << 8) | b;
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{TRUE, 0, 0, mask, colorBitmap};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(colorBitmap);
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

class Tray {
public:
    void install(HWND window) {
        window_ = window;
        icons_[0] = makeDotIcon(RGB(140, 140, 140));   // no phone
        icons_[1] = makeDotIcon(RGB(230, 170, 40));    // linked, camera idle
        icons_[2] = makeDotIcon(RGB(60, 190, 90));     // streaming

        data_.cbSize = sizeof(data_);
        data_.hWnd = window;
        data_.uID = 1;
        data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        data_.uCallbackMessage = WM_TRAY;
        data_.hIcon = icons_[0];
        wcscpy_s(data_.szTip, L"AndroidWebCam");
        Shell_NotifyIconW(NIM_ADD, &data_);
    }

    void remove() {
        Shell_NotifyIconW(NIM_DELETE, &data_);
        for (HICON icon : icons_) if (icon) DestroyIcon(icon);
    }

    void update(const Status& s) {
        const int slot = s.state == LinkState::Streaming ? 2 : (s.state == LinkState::Linked ? 1 : 0);
        data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        data_.hIcon = icons_[slot];
        const std::wstring tip = tooltip(s);
        wcsncpy_s(data_.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data_);
    }

    void balloon(const std::wstring& title, const std::wstring& text) {
        NOTIFYICONDATAW n = data_;
        n.uFlags = NIF_INFO;
        n.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }

    static std::wstring tooltip(const Status& s) {
        wchar_t buf[128]{};
        if (!s.error.empty()) {
            swprintf_s(buf, L"AndroidWebCam\n%s", widen(s.error).c_str());
        } else if (s.state == LinkState::Streaming) {
            swprintf_s(buf, L"AndroidWebCam - streaming\n%ux%u %llu kbps\n%s", s.width, s.height,
                       s.kbps, widen(s.phone).c_str());
        } else if (s.state == LinkState::Linked) {
            swprintf_s(buf, L"AndroidWebCam - phone ready\n%s\ncamera not in use",
                       widen(s.phone).c_str());
        } else {
            swprintf_s(buf, L"AndroidWebCam - looking for the phone");
        }
        return buf;
    }

private:
    HWND window_ = nullptr;
    NOTIFYICONDATAW data_{};
    HICON icons_[3]{};
};

Tray g_tray;

// ------------------------------------------------------------------ worker

/** Links to the phone, or returns null if it cannot be reached right now. */
std::unique_ptr<Session> link(const Options& options, std::string* phoneIp) {
    auto session = std::make_unique<Session>();
    std::string err;
    if (!session->init(options.port, &err)) return nullptr;

    std::string ip = options.ip;
    if (ip.empty()) {
        auto phones = session->discover(1200);
        if (phones.empty()) return nullptr;
        ip = phones.front().ip;
    }
    if (!session->connect(ip, 1500)) return nullptr;
    *phoneIp = ip;
    return session;
}

void workerLoop(Options options, HWND window) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Status status;

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        status.error = "MFStartup failed";
        setStatus(status);
        return;
    }

    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                       MFVirtualCameraLifetime_Session,
                                       MFVirtualCameraAccess_CurrentUser,
                                       kCameraFriendlyName, kSourceClsidString,
                                       nullptr, 0, camera.GetAddressOf());
    logf("MFCreateVirtualCamera -> 0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && options.remove) {
        camera->Remove();
        PostMessageW(window, WM_CLOSE, 0, 0);
        return;
    }
    if (SUCCEEDED(hr)) {
        hr = camera->Start(nullptr);
        logf("IMFVirtualCamera::Start -> 0x%08lX", static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "cannot create the camera (0x%08lX) - run install.ps1 as administrator",
                  static_cast<unsigned long>(hr));
        status.error = buf;
        setStatus(status);
        return;
    }

    ProducerChannel channel;
    std::unique_ptr<Session> session;
    std::unique_ptr<H264Decoder> decoder;
    Depacketizer depack;
    std::string phoneIp;
    std::vector<uint8_t> nv12;
    std::vector<uint8_t> scaled(size_t(kMaxWidth) * kMaxHeight * 3 / 2);

    Stabilizer stabilizer;
    ExposureControl exposureControl;
    const unsigned warpThreads = (std::max)(2u, std::thread::hardware_concurrency() / 2);
    const float crop = 0.9f;                       // margin the correction can move into

    struct PendingFrame {
        std::vector<uint8_t> nv12;
        uint32_t width = 0, height = 0, rtpTimestamp = 0;
        uint64_t arrivedMs = 0;
    };
    struct FrameTiming {
        int64_t sensorNs = 0;      // exposure start of the first row
        int64_t skewNs = 0;        // time from the first to the last row
        int64_t exposureNs = 0;
    };
    std::deque<PendingFrame> pendingFrames;
    std::unordered_map<uint32_t, FrameTiming> frameTimes;
    bool announcedStabilization = false;
    bool loggedSkew = false;
    bool shortExposureWas = false;
    uint64_t stabilizedFrames = 0;

    uint64_t telemetryPackets = 0, foreignPackets = 0, decodedFrames = 0;

    auto handleTelemetry = [&](const uint8_t* p, int len) {
        if (len < 5 || std::memcmp(p, kMagic, 4) != 0) {
            if (foreignPackets++ < 5)
                logf("unrecognised datagram: %d bytes, first=%02X %02X %02X %02X", len,
                     len > 0 ? p[0] : 0, len > 1 ? p[1] : 0, len > 2 ? p[2] : 0, len > 3 ? p[3] : 0);
            return;
        }
        if (telemetryPackets++ < 5) logf("telemetry packet type 0x%02X, %d bytes", p[4], len);
        const uint8_t* body = p + 5;
        const int size = len - 5;
        auto readU16 = [](const uint8_t* d) { return uint16_t((d[0] << 8) | d[1]); };
        auto readI32 = [](const uint8_t* d) {
            return int32_t((uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) |
                           (uint32_t(d[2]) << 8) | d[3]);
        };
        auto readI64 = [](const uint8_t* d) {
            int64_t v = 0;
            for (int i = 0; i < 8; ++i) v = (v << 8) | d[i];
            return v;
        };
        auto readF32 = [&](const uint8_t* d) {
            const int32_t bits = readI32(d);
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        };

        switch (p[4]) {
            case kCamInfo: {
                if (size < 25) return;
                CameraGeometry g;
                g.fx = readF32(body);
                g.fy = readF32(body + 4);
                g.cx = readF32(body + 8);
                g.cy = readF32(body + 12);
                g.width = readU16(body + 16);
                g.height = readU16(body + 18);
                g.orientation = readI32(body + 20);
                g.facing = body[24];
                stabilizer.setGeometry(g);
                logf("cam info: fx=%.1f fy=%.1f c=(%.1f,%.1f) %ux%u orient=%d facing=%u",
                     g.fx, g.fy, g.cx, g.cy, g.width, g.height, g.orientation, g.facing);
                break;
            }
            case kGyro: {
                if (size < 1) return;
                const int count = body[0];
                for (int i = 0; i < count; ++i) {
                    const uint8_t* s = body + 1 + i * 20;
                    if (1 + (i + 1) * 20 > size) break;
                    stabilizer.addGyro(readI64(s), readF32(s + 8), readF32(s + 12), readF32(s + 16));
                }
                break;
            }
            case kGravity: {
                if (size < 12) return;
                stabilizer.addGravity(readF32(body), readF32(body + 4), readF32(body + 8));
                break;
            }
            case kSensorLimits: {
                if (size < 24) return;
                exposureControl.setLimits(readI32(body), readI32(body + 4), readI64(body + 8),
                                          readI64(body + 16));
                break;
            }
            case kFrameMeta: {
                if (size < 28) return;
                FrameTiming timing;
                timing.sensorNs = readI64(body + 4);
                timing.exposureNs = readI64(body + 12);
                timing.skewNs = readI64(body + 20);
                frameTimes[uint32_t(readI32(body))] = timing;
                if (!loggedSkew) {
                    loggedSkew = true;
                    logf("rolling shutter skew %.2f ms, exposure %.2f ms",
                         timing.skewNs / 1e6, timing.exposureNs / 1e6);
                }
                if (frameTimes.size() > 300) frameTimes.clear();
                break;
            }
            default:
                break;
        }
    };

    uint64_t published = 0, lastPublished = 0, lastBytes = 0;
    uint64_t nextReport = nowMs() + 1000, nextConnect = 0;
    bool streaming = false;
    bool lastChannelOpen = false;
    LinkState announced = LinkState::NoPhone;

    // Keeps the correction inside the crop margin: sampling past the frame edge would
    // smear the border, which reads as distortion.
    auto marginScale = [&](const Quat& correction, const CameraGeometry& geometry) {
        const float angle = 2.0f * std::acos((std::min)(1.0f, std::fabs(correction.w)));
        const float halfDiagonal =
            0.5f * std::sqrt(float(geometry.width) * geometry.width +
                             float(geometry.height) * geometry.height);
        const float shiftPx = angle * (std::max)((std::max)(geometry.fx, geometry.fy), halfDiagonal);
        const float marginPx = (1.0f - crop) * 0.5f * float(geometry.width) * 0.8f;
        return shiftPx > marginPx && shiftPx > 0 ? marginPx / shiftPx : 1.0f;
    };

    // Publishes one frame, stabilized when gyro data for it is available.
    auto emit = [&](PendingFrame& f, bool stabilized, const FrameTiming& timing) {
        const CameraGeometry geometry = stabilizer.geometry();
        uint8_t* out = scaled.data();

        // Horizon levelling: a constant roll from the gravity vector, limited to what the
        // crop margin allows - rotating a frame needs room in the corners.
        Quat levelling;
        if (g_level && stabilizer.haveGravity() && geometry.valid()) {
            const float roll = stabilizer.horizonRoll();
            const float maxRoll = (1.0f - crop) /
                                  (float(geometry.height) / float(geometry.width) + 1e-3f) * 0.9f;
            const float limited = (std::max)(-maxRoll, (std::min)(maxRoll, roll));
            g_levelDeg = roll * 57.2957795f;
            levelling = Stabilizer::rollQuat(-limited);
        } else if (!g_level) {
            g_levelDeg = 0.0f;
        }

        if (stabilized || (g_level && stabilizer.haveGravity() && geometry.valid())) {
            // One homography per band of rows: a rolling shutter exposes the top and
            // bottom of the frame milliseconds apart, so fast vibration shears the
            // picture and a single per-frame rotation cannot undo it.
            constexpr uint32_t kBands = 33;
            Homography bands[kBands];
            const int64_t centre = timing.sensorNs + timing.skewNs / 2 + timing.exposureNs / 2;
            const float scale = stabilized ? marginScale(stabilizer.correctionFor(centre), geometry)
                                           : 1.0f;

            float firstAngle = 0, lastAngle = 0;
            for (uint32_t i = 0; i < kBands; ++i) {
                const float fraction = float(i) / float(kBands - 1);
                const int64_t rowTime = timing.sensorNs + timing.exposureNs / 2 +
                                        int64_t(fraction * float(timing.skewNs));
                Quat q = stabilized ? stabilizer.correctionAtRow(rowTime) : Quat{};
                if (scale < 1.0f) q = slerp(Quat{}, q, scale);
                q = normalize(conjugate(levelling) * q);   // desired orientation is levelled
                bands[i] = stabilizationHomography(geometry, q, kMaxWidth, kMaxHeight, crop);
                const float angle = 2.0f * std::acos((std::min)(1.0f, std::fabs(q.w))) * 57.2957795f;
                if (i == 0) firstAngle = angle;
                if (i == kBands - 1) lastAngle = angle;
            }
            if (++stabilizedFrames % 30 == 0) {
                logf("bands: first %.3f deg, last %.3f deg, spread %.3f deg (%.1f px), scale %.2f",
                     firstAngle, lastAngle, lastAngle - firstAngle,
                     (lastAngle - firstAngle) / 57.2957795f * geometry.fx, scale);
            }
            warpNv12Banded(f.nv12.data(), f.width, f.height, out, kMaxWidth, kMaxHeight,
                           bands, kBands, warpThreads);
        } else if (f.width == kMaxWidth && f.height == kMaxHeight) {
            out = f.nv12.data();                       // exact size, no resampling at all
        } else {
            scaleNv12(f.nv12.data(), f.width, f.height, out, kMaxWidth, kMaxHeight);
        }

        const size_t outSize = size_t(kMaxWidth) * kMaxHeight * 3 / 2;
        flipNv12(out, kMaxWidth, kMaxHeight, g_flipH, g_flipV);
        channel.publish(out, outSize, kMaxWidth, kMaxHeight, 0);
        published++;
    };

    // Frames wait for the gyro window around their timestamp; that wait is the
    // look-ahead the smoothing needs.
    auto drainPending = [&]() {
        while (!pendingFrames.empty()) {
            PendingFrame& f = pendingFrames.front();
            const auto it = frameTimes.find(f.rtpTimestamp);
            const FrameTiming timing = it != frameTimes.end() ? it->second : FrameTiming{};
            const bool usable = g_stabilize && timing.sensorNs != 0 && stabilizer.geometry().valid();
            const bool ready = usable && stabilizer.ready(timing.sensorNs + timing.skewNs);
            const bool expired = nowMs() - f.arrivedMs > 250;
            // The queue is the look-ahead buffer; once it is full the oldest frame has
            // to go out as-is rather than be dropped.
            const bool overflowing = pendingFrames.size() > 6;

            if (!ready && !expired && !overflowing) break;
            if (ready) {
                emit(f, true, timing);
                g_stabAngle = stabilizer.stats().lastAngleDeg;
            } else {
                emit(f, false, timing);
            }
            if (it != frameTimes.end()) frameTimes.erase(it);
            pendingFrames.pop_front();
        }
    };

    depack.setCallback([&](const uint8_t* data, size_t size, uint32_t ts, bool) {
        if (!decoder) return;
        decoder->decode(data, size, int64_t(ts) * 1000 / 9, [&](IMFSample* sample) {
            decodedFrames++;
            if (!decoder->copyNv12(sample, nv12)) return;

            // Metering for the exposure controller: the decoded luma is already here, so
            // the phone needs no extra camera stream just to measure brightness.
            if (g_shortExposure && !nv12.empty()) {
                const uint32_t w = decoder->width(), h = decoder->height();
                uint64_t sum = 0;
                uint32_t counted = 0;
                for (uint32_t y = 0; y < h; y += 8) {
                    const uint8_t* row = nv12.data() + size_t(y) * w;
                    for (uint32_t x = 0; x < w; x += 8) { sum += row[x]; counted++; }
                }
                if (counted) exposureControl.update(float(sum) / float(counted));
            }
            if (!g_stabilize) {                        // no buffering when not stabilizing
                PendingFrame direct;
                direct.width = decoder->width();
                direct.height = decoder->height();
                direct.nv12.swap(nv12);
                emit(direct, false, FrameTiming{});
                direct.nv12.swap(nv12);                // keep the buffer for reuse
                return;
            }
            PendingFrame f;
            f.nv12 = nv12;
            f.width = decoder->width();
            f.height = decoder->height();
            f.rtpTimestamp = ts;
            f.arrivedMs = nowMs();
            pendingFrames.push_back(std::move(f));
        });
    });

    while (!g_quit) {
        // 1. keep a link with the phone whenever one is reachable
        if (!session && nowMs() >= nextConnect) {
            nextConnect = nowMs() + 2000;
            session = link(options, &phoneIp);
            if (session) {
                logf("linked to %s", phoneIp.c_str());
                const StreamParams& p = session->params();
                status.phone = phoneIp;
                status.width = p.width;
                status.height = p.height;
                status.fps = p.fps;
                status.state = LinkState::Linked;
                status.error.clear();
                setStatus(status);
            }
        }

        // 2. stream only while an application actually holds the camera open
        const bool channelOpen = channel.tryOpen();
        const bool wanted = session && channelOpen && channel.consumerActive();
        if (channelOpen != lastChannelOpen) {
            logf("shared channel %s", channelOpen ? "opened (source DLL is loaded)" : "gone");
            lastChannelOpen = channelOpen;
        }

        if (wanted && !streaming) {
            const StreamParams& p = session->params();
            decoder = std::make_unique<H264Decoder>();
            std::string err;
            if (!decoder->init(p.width, p.height, p.fps ? p.fps : 30, &err)) {
                status.error = "decoder: " + err;
                setStatus(status);
                decoder.reset();
                session.reset();
                continue;
            }
            depack.reset();
            stabilizer.reset();
            exposureControl.reset();
            pendingFrames.clear();
            frameTimes.clear();
            session->setDeviceStabilization(!g_stabilize);
            announcedStabilization = g_stabilize;
            session->streamStart();
            logf("STREAM_START sent to %s (gyro stabilization %s)", phoneIp.c_str(),
                 g_stabilize ? "on" : "off");
            streaming = true;
            status.state = LinkState::Streaming;
            setStatus(status);
        } else if (!wanted && streaming) {
            session->streamStop();
            streaming = false;
            decoder.reset();
            status.state = session ? LinkState::Linked : LinkState::NoPhone;
            status.kbps = 0;
            setStatus(status);
        }

        if (session && streaming && announcedStabilization != g_stabilize) {
            announcedStabilization = g_stabilize;
            session->setDeviceStabilization(!g_stabilize);
            session->requestIdr();
            stabilizer.reset();
            pendingFrames.clear();
            logf("gyro stabilization switched %s", g_stabilize ? "on" : "off");
        }

        // Push a new exposure/gain setpoint when the controller moves it, or hand control
        // back to the phone's auto exposure when the mode is switched off.
        if (session && streaming) {
            if (g_shortExposure && exposureControl.haveLimits() && exposureControl.changed()) {
                session->setExposure(exposureControl.exposureNs(), exposureControl.iso());
                g_exposureUs = int(exposureControl.exposureNs() / 1000);
                g_iso = exposureControl.iso();
                logf("exposure %.1f ms, ISO %d", exposureControl.exposureNs() / 1e6,
                     exposureControl.iso());
            } else if (!g_shortExposure && shortExposureWas) {
                session->setExposure(0, 0);
                g_exposureUs = 0;
                g_iso = 0;
                logf("exposure back to auto");
            }
            shortExposureWas = g_shortExposure;
        }

        if (session) {
            if (!session->poll(streaming ? 50 : 200, [&](const uint8_t* pkt, int len) {
                    if (len >= 12 && (pkt[1] & 0x7F) == kRtpPayloadType) depack.push(pkt, len);
                    else handleTelemetry(pkt, len);
                })) {
                session.reset();
                decoder.reset();
                streaming = false;
                status.state = LinkState::NoPhone;
                status.kbps = 0;
                setStatus(status);
                continue;
            }
            if (streaming && depack.takeNeedIdr()) session->requestIdr();
            drainPending();
        } else {
            Sleep(200);
        }

        const uint64_t now = nowMs();
        if (now >= nextReport) {
            nextReport = now + 1000;
            const auto& s = depack.stats;
            status.kbps = (s.bytes - lastBytes) * 8 / 1000;
            status.fps = uint32_t(published - lastPublished);
            status.lost = s.lost;
            status.dropped = s.dropped;
            lastBytes = s.bytes;
            lastPublished = published;
            setStatus(status);
            if (streaming) {
                logf("pub %u fps | %llu kbps | au %llu dec %llu pub %llu queued %zu | "
                     "telemetry %llu gyro %llu | gravity %s tilt %+.1f deg | lost %llu dropped %llu",
                     status.fps, status.kbps, s.frames, decodedFrames, published,
                     pendingFrames.size(), telemetryPackets, stabilizer.stats().samples,
                     stabilizer.haveGravity() ? "yes" : "no",
                     stabilizer.horizonRoll() * 57.2957795f, s.lost, s.dropped);
            }

            if (status.state != announced) {
                if (status.state == LinkState::Linked && announced == LinkState::NoPhone)
                    g_tray.balloon(L"AndroidWebCam", L"Phone connected: " + widen(phoneIp));
                else if (status.state == LinkState::NoPhone)
                    g_tray.balloon(L"AndroidWebCam", L"Phone disconnected");
                announced = status.state;
            }
        }
    }

    if (session) {
        if (streaming) session->streamStop();
        session->bye();
    }
    if (camera) { camera->Stop(); camera->Shutdown(); }
    decoder.reset();
    MFShutdown();
    CoUninitialize();
}

// ------------------------------------------------------------------ window

LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_TIMER:
            g_tray.update(getStatus());
            return 0;

        case WM_TRAY:
            if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_LBUTTONUP) {
                const Status s = getStatus();
                HMENU menu = CreatePopupMenu();
                std::wstring line = Tray::tooltip(s);
                for (auto& c : line) if (c == L'\n') c = L' ';
                AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, line.c_str());
                if (s.state == LinkState::Streaming) {
                    wchar_t stats[96];
                    swprintf_s(stats, L"lost packets %llu, dropped frames %llu", s.lost, s.dropped);
                    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, stats);
                }
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING | (g_flipH ? MF_CHECKED : MF_UNCHECKED), kMenuFlipH,
                            L"Mirror horizontally");
                AppendMenuW(menu, MF_STRING | (g_flipV ? MF_CHECKED : MF_UNCHECKED), kMenuFlipV,
                            L"Mirror vertically");
                wchar_t stabLabel[80];
                swprintf_s(stabLabel, L"Gyro stabilization (%.1f°)", g_stabAngle.load());
                AppendMenuW(menu, MF_STRING | (g_stabilize ? MF_CHECKED : MF_UNCHECKED),
                            kMenuStabilize, stabLabel);
                wchar_t exposureLabel[80];
                swprintf_s(exposureLabel, L"Short exposure (%d ms, ISO %d)",
                           g_exposureUs.load() / 1000, g_iso.load());
                AppendMenuW(menu, MF_STRING | (g_shortExposure ? MF_CHECKED : MF_UNCHECKED),
                            kMenuShortExposure, exposureLabel);
                wchar_t levelLabel[80];
                swprintf_s(levelLabel, L"Level horizon (tilt %+.1f°)", g_levelDeg.load());
                AppendMenuW(menu, MF_STRING | (g_level ? MF_CHECKED : MF_UNCHECKED), kMenuLevel,
                            levelLabel);
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

                POINT p{};
                GetCursorPos(&p);
                SetForegroundWindow(window);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
                DestroyMenu(menu);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kMenuExit:
                    PostMessageW(window, WM_CLOSE, 0, 0);
                    break;
                case kMenuFlipH:
                    g_flipH = !g_flipH;
                    saveSetting(L"MirrorHorizontal", g_flipH ? 1 : 0);
                    break;
                case kMenuFlipV:
                    g_flipV = !g_flipV;
                    saveSetting(L"MirrorVertical", g_flipV ? 1 : 0);
                    break;
                case kMenuStabilize:
                    g_stabilize = !g_stabilize;
                    saveSetting(L"GyroStabilize", g_stabilize ? 1 : 0);
                    break;
                case kMenuShortExposure:
                    g_shortExposure = !g_shortExposure;
                    saveSetting(L"ShortExposure", g_shortExposure ? 1 : 0);
                    break;
                case kMenuLevel:
                    g_level = !g_level;
                    saveSetting(L"LevelHorizon", g_level ? 1 : 0);
                    break;
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int runTray(const Options& options) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AwcTrayWindow";
    RegisterClassExW(&wc);

    HWND window = CreateWindowExW(0, wc.lpszClassName, L"AndroidWebCam", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!window) return 1;

    g_tray.install(window);
    SetTimer(window, kTimerId, 1000, nullptr);

    std::thread worker(workerLoop, options, window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_quit = true;
    if (worker.joinable()) worker.join();
    KillTimer(window, kTimerId);
    g_tray.remove();
    return 0;
}

// ------------------------------------------------------------------ console test mode

int runTest(const Options& options) {
    attachConsole();

    Session session;
    std::string err;
    if (!session.init(options.port, &err)) { printf("init: %s\n", err.c_str()); return 1; }

    std::string ip = options.ip;
    if (ip.empty()) {
        printf("discovering phones on the LAN...\n");
        auto phones = session.discover(1500);
        if (phones.empty()) { printf("no phone answered; pass --ip <addr>\n"); return 1; }
        for (auto& p : phones) printf("  found %s (%s)\n", p.ip.c_str(), p.name.c_str());
        ip = phones.front().ip;
    }

    printf("connecting to %s...\n", ip.c_str());
    if (!session.connect(ip, 3000)) {
        printf("no HELLO_ACK from %s:%u\n", ip.c_str(), kControlPort);
        return 1;
    }
    const StreamParams& p = session.params();
    printf("stream: %ux%u@%u  %u kbps  ssrc=%08X  rtpSrcPort=%u\n",
           p.width, p.height, p.fps, p.bitrate / 1000, p.ssrc, p.rtpSourcePort);

    H264Decoder decoder;
    if (!decoder.init(p.width, p.height, p.fps ? p.fps : 30, &err)) {
        printf("decoder: %s\n", err.c_str());
        return 1;
    }
    printf("decoder: %s\n", decoder.decoderName().c_str());

    FILE* dump = nullptr;
    if (!options.dump.empty()) fopen_s(&dump, options.dump.c_str(), "wb");

    Depacketizer depack;
    uint64_t decoded = 0;
    bool savedFrame = false;
    std::vector<uint8_t> nv12;

    depack.setCallback([&](const uint8_t* data, size_t size, uint32_t ts, bool) {
        if (dump) fwrite(data, 1, size, dump);
        decoder.decode(data, size, int64_t(ts) * 1000 / 9, [&](IMFSample* sample) {
            decoded++;
            if (!options.frame.empty() && !savedFrame && decoder.copyNv12(sample, nv12)) {
                FILE* f = nullptr;
                fopen_s(&f, options.frame.c_str(), "wb");
                if (f) { fwrite(nv12.data(), 1, nv12.size(), f); fclose(f); }
                savedFrame = true;
                printf("saved first frame: %s (%ux%u NV12)\n", options.frame.c_str(),
                       decoder.width(), decoder.height());
            }
        });
    });

    session.streamStart();

    const uint64_t start = nowMs();
    uint64_t nextReport = start + 1000, lastBytes = 0, lastFrames = 0, lastDecoded = 0;
    bool alive = true;

    while (!g_quit && alive) {
        alive = session.poll(50, [&](const uint8_t* pkt, int len) { depack.push(pkt, len); });
        if (depack.takeNeedIdr()) session.requestIdr();

        const uint64_t now = nowMs();
        if (now >= nextReport) {
            const auto& s = depack.stats;
            printf("%5.1fs  %4llu kbps  rx %3llu fps  dec %3llu fps  lost %llu  dropped %llu  key %llu\n",
                   (now - start) / 1000.0, (s.bytes - lastBytes) * 8 / 1000,
                   s.frames - lastFrames, decoded - lastDecoded, s.lost, s.dropped, s.keyframes);
            lastBytes = s.bytes;
            lastFrames = s.frames;
            lastDecoded = decoded;
            nextReport = now + 1000;
        }
        if (options.seconds > 0 && now - start >= uint64_t(options.seconds) * 1000) break;
    }
    if (!alive) printf("link timed out\n");

    session.streamStop();
    session.bye();
    if (dump) fclose(dump);

    const auto& s = depack.stats;
    printf("\ntotal: packets %llu  bytes %llu  access units %llu  decoded %llu  lost %llu  dropped %llu\n",
           s.packets, s.bytes, s.frames, decoded, s.lost, s.dropped);
    decoder.shutdown();
    return decoded > 0 ? 0 : 2;
}

BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_quit = true;
        return TRUE;
    }
    return FALSE;
}

Options parse(int argc, wchar_t** argv) {
    Options options;
    auto narrow = [](const wchar_t* w) {
        char buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
        return std::string(buf);
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? narrow(argv[++i]) : std::string(); };
        if (a == L"--ip") options.ip = next();
        else if (a == L"--port") options.port = uint16_t(atoi(next().c_str()));
        else if (a == L"--seconds") options.seconds = atoi(next().c_str());
        else if (a == L"--dump") options.dump = next();
        else if (a == L"--frame") options.frame = next();
        else if (a == L"--test") options.test = true;
        else if (a == L"--remove") options.remove = true;
        else if (a == L"--list") options.list = true;
        else if (a == L"--capture") options.capture = true;
        else if (a == L"--gyrocheck") options.gyroCheck = true;
        else if (a == L"--analyze") options.analyze = next();
        else if (a == L"--warptest") options.warpTest = true;
        else if (a == L"--leveltest") options.levelTest = true;
        else if (a == L"--selftest") options.selftest = (i + 1 < argc) ? argv[++i] : L"awc-source.dll";
    }
    return options;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const Options options = parse(argc, argv);
    LocalFree(argv);

    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    logInit(L"C:\\ProgramData\\AndroidWebCam", L"client.log");
    g_flipH = loadSetting(L"MirrorHorizontal", 0) != 0;
    g_flipV = loadSetting(L"MirrorVertical", 0) != 0;
    g_stabilize = loadSetting(L"GyroStabilize", 0) != 0;
    g_shortExposure = loadSetting(L"ShortExposure", 0) != 0;
    g_level = loadSetting(L"LevelHorizon", 0) != 0;
    if (!options.selftest.empty()) return runSelfTest(options.selftest);
    if (options.capture) return runCapture(L"AndroidWebCam", options.frame);
    if (options.gyroCheck) return runGyroCheck(options);
    if (!options.analyze.empty()) return runAnalyze(options.analyze);
    if (options.warpTest) return runWarpTest();
    if (options.levelTest) return runLevelTest();
    if (options.list) return runList();
    return options.test ? runTest(options) : runTray(options);
}
