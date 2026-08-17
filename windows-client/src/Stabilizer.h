#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace awc {

struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
};

Quat operator*(const Quat& a, const Quat& b);
Quat conjugate(const Quat& q);
Quat normalize(const Quat& q);
/** Shortest-arc interpolation, used to low-pass the orientation trajectory. */
Quat slerp(const Quat& a, const Quat& b, float t);

struct CameraGeometry {
    float fx = 0, fy = 0, cx = 0, cy = 0;
    uint16_t width = 0, height = 0;
    int32_t orientation = 0;
    uint8_t facing = 0;
    bool valid() const { return fx > 0 && width > 0; }
};

/**
 * Gyro-based electronic stabilization.
 *
 * Integrates the phone's angular velocity into an orientation track, low-passes it
 * to get the camera path we *wish* the phone had followed, and reports the residual
 * rotation for each frame. The caller turns that rotation into a homography
 * (H = K R K^-1) and warps the frame by it.
 *
 * Smoothing needs samples from slightly after the frame, so frames are held back by
 * [lookaheadMs]: that delay is the price of steadiness.
 */
class Stabilizer {
public:
    void setGeometry(const CameraGeometry& geometry);
    CameraGeometry geometry() const;

    void addGyro(int64_t timestampNs, float x, float y, float z);

    /** True once gyro samples cover [timestampNs + lookahead]. */
    bool ready(int64_t timestampNs) const;

    /** Rotation to apply to the frame captured at [timestampNs], in camera axes. */
    Quat correctionFor(int64_t timestampNs);

    /**
     * Correction for one scanline, valid only right after [correctionFor] advanced the
     * smoothed path for this frame. A rolling shutter exposes each row at a different
     * instant, so a single per-frame rotation cannot undo the resulting shear.
     */
    Quat correctionAtRow(int64_t rowTimeNs);

    /** Absolute integrated orientation at [timestampNs]; used to validate the axes. */
    Quat orientationAt(int64_t timestampNs);

    /**
     * Gravity in device axes, the absolute reference a gyroscope cannot provide.
     * Heavily low-passed: the horizon does not move, so anything fast here is motion.
     */
    void addGravity(float x, float y, float z);
    bool haveGravity() const;
    /** Image roll relative to level, radians; positive means the frame leans one way. */
    float horizonRoll() const;

    /** Rotation about the optical axis that puts the horizon straight. */
    static Quat rollQuat(float radians);

    void setLookaheadMs(int ms) { lookaheadMs_ = ms; }
    int lookaheadMs() const { return lookaheadMs_; }
    void setSmoothingMs(int ms) { smoothingMs_ = ms; }
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    void reset();

    struct Stats {
        uint64_t samples = 0;
        uint64_t frames = 0;
        uint64_t starved = 0;      // frames that arrived before their gyro window
        float lastAngleDeg = 0;    // size of the last correction
        float maxAngleDeg = 0;
    };
    Stats stats() const;

private:
    struct Sample {
        int64_t t;
        float x, y, z;
    };

    mutable std::mutex lock_;
    std::deque<Sample> samples_;
    std::deque<std::pair<int64_t, Quat>> track_;   // integrated orientation over time
    Quat orientation_;
    int64_t lastIntegrated_ = 0;
    Quat smoothed_;
    bool haveSmoothed_ = false;
    int64_t lastSmoothedAt_ = 0;

    float gravityX_ = 0, gravityY_ = 0, gravityZ_ = 0;
    bool haveGravity_ = false;

    CameraGeometry geometry_;
    int lookaheadMs_ = 120;
    int smoothingMs_ = 500;
    bool enabled_ = false;
    Stats stats_;

    void integrateLocked(int64_t untilNs);
    Quat orientationAtLocked(int64_t t) const;
};

} // namespace awc
