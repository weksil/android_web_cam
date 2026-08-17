#include "Stabilizer.h"

#include <algorithm>
#include <cmath>

namespace awc {

Quat operator*(const Quat& a, const Quat& b) {
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

Quat conjugate(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }

Quat normalize(const Quat& q) {
    const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n <= 0) return {};
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}

Quat slerp(const Quat& a, const Quat& b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    Quat target = b;
    if (dot < 0) {                       // take the shortest arc
        target = {-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }
    if (dot > 0.9995f) {                 // nearly identical, lerp is accurate enough
        return normalize({a.w + (target.w - a.w) * t, a.x + (target.x - a.x) * t,
                          a.y + (target.y - a.y) * t, a.z + (target.z - a.z) * t});
    }
    const float theta = std::acos(dot) * t;
    Quat perpendicular = normalize({target.w - a.w * dot, target.x - a.x * dot,
                                    target.y - a.y * dot, target.z - a.z * dot});
    const float c = std::cos(theta), s = std::sin(theta);
    return normalize({a.w * c + perpendicular.w * s, a.x * c + perpendicular.x * s,
                      a.y * c + perpendicular.y * s, a.z * c + perpendicular.z * s});
}

namespace {

/**
 * Gyro samples arrive in device axes (x right, y up, z out of the screen); the stream
 * is the raw sensor readout, which is landscape for a SENSOR_ORIENTATION 90 camera.
 * Camera axes follow the usual convention: X right, Y down, Z into the scene.
 *
 * Verified with --gyrocheck against all three axes at once (see README). With
 * dx = -fx*yaw, dy = +fy*pitch and imageRoll = -roll, the mapping below predicts
 *   horizontal = +fx * gyro x   measured +1790 px/rad, correlation +0.96
 *   vertical   = -fy * gyro y   measured -1609 px/rad, correlation -0.86
 *   image roll = +      gyro z  correlation +0.84
 * Two axes alone cannot pin this down - the roll measurement is what distinguishes
 * this mapping from its mirrored twin.
 */
void toCameraAxes(int orientation, uint8_t facing, float& x, float& y, float& z) {
    const float dx = x, dy = y, dz = z;
    const bool front = facing == 0;
    switch (((orientation % 360) + 360) % 360) {
        case 270:
            x = dy;  y = dx;  z = front ? dz : -dz;
            break;
        case 90:
        default:
            x = -dy; y = -dx; z = front ? dz : -dz;
            break;
    }
    if (front) x = -x;                   // front camera output is mirrored
}

} // namespace

Quat Stabilizer::rollQuat(float radians) {
    // Rotation about the camera Z axis, which is the optical axis.
    const float half = radians * 0.5f;
    return {std::cos(half), 0.0f, 0.0f, std::sin(half)};
}

void Stabilizer::addGravity(float x, float y, float z) {
    std::lock_guard<std::mutex> guard(lock_);
    toCameraAxes(geometry_.orientation, geometry_.facing, x, y, z);
    const float alpha = haveGravity_ ? 0.08f : 1.0f;    // ~1 s at 25 Hz
    gravityX_ += (x - gravityX_) * alpha;
    gravityY_ += (y - gravityY_) * alpha;
    gravityZ_ += (z - gravityZ_) * alpha;
    haveGravity_ = true;
}

bool Stabilizer::haveGravity() const {
    std::lock_guard<std::mutex> guard(lock_);
    return haveGravity_;
}

float Stabilizer::horizonRoll() const {
    std::lock_guard<std::mutex> guard(lock_);
    if (!haveGravity_) return 0.0f;
    // In camera axes X is right and Y is down, so a level camera sees gravity pointing
    // straight down the image: (0, +g). Any sideways component is the lean.
    const float planar = std::sqrt(gravityX_ * gravityX_ + gravityY_ * gravityY_);
    if (planar < 1.5f) return 0.0f;      // camera points nearly up or down: no horizon

    float roll = std::atan2(gravityX_, gravityY_);
    // Which way the sensor rows run depends on which side up the phone is mounted, so the
    // raw angle can come out half a turn off. Levelling never needs a half turn: fold the
    // angle into +/-90 degrees and the feature works in either mounting.
    constexpr float pi = 3.14159265f;
    if (roll > pi / 2) roll -= pi;
    else if (roll < -pi / 2) roll += pi;
    return roll;
}

void Stabilizer::setGeometry(const CameraGeometry& geometry) {
    std::lock_guard<std::mutex> guard(lock_);
    geometry_ = geometry;
}

CameraGeometry Stabilizer::geometry() const {
    std::lock_guard<std::mutex> guard(lock_);
    return geometry_;
}

void Stabilizer::reset() {
    std::lock_guard<std::mutex> guard(lock_);
    samples_.clear();
    track_.clear();
    orientation_ = {};
    smoothed_ = {};
    haveSmoothed_ = false;
    lastIntegrated_ = 0;
    lastSmoothedAt_ = 0;
    stats_ = {};
}

void Stabilizer::addGyro(int64_t timestampNs, float x, float y, float z) {
    std::lock_guard<std::mutex> guard(lock_);
    toCameraAxes(geometry_.orientation, geometry_.facing, x, y, z);
    if (!samples_.empty() && timestampNs <= samples_.back().t) return;   // out of order
    samples_.push_back({timestampNs, x, y, z});
    stats_.samples++;
    while (samples_.size() > 4000) samples_.pop_front();                  // ~20 s at 200 Hz
}

bool Stabilizer::ready(int64_t timestampNs) const {
    std::lock_guard<std::mutex> guard(lock_);
    if (samples_.empty()) return false;
    return samples_.back().t >= timestampNs + int64_t(lookaheadMs_) * 1'000'000;
}

void Stabilizer::integrateLocked(int64_t untilNs) {
    for (const Sample& s : samples_) {
        if (s.t <= lastIntegrated_) continue;
        if (s.t > untilNs) break;
        if (lastIntegrated_ == 0) {
            lastIntegrated_ = s.t;
            track_.emplace_back(s.t, orientation_);
            continue;
        }
        const float dt = float(s.t - lastIntegrated_) / 1e9f;
        lastIntegrated_ = s.t;
        if (dt <= 0 || dt > 0.5f) continue;

        const float omega = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
        const float angle = omega * dt;
        if (angle > 1e-9f) {
            const float half = angle * 0.5f;
            const float k = std::sin(half) / omega;      // axis = w/|w|, so sin(a/2)*w/|w|
            orientation_ = normalize(orientation_ *
                                     Quat{std::cos(half), s.x * k, s.y * k, s.z * k});
        }
        track_.emplace_back(s.t, orientation_);
        while (track_.size() > 4000) track_.pop_front();
    }
}

Quat Stabilizer::orientationAtLocked(int64_t t) const {
    if (track_.empty()) return {};
    if (t <= track_.front().first) return track_.front().second;
    if (t >= track_.back().first) return track_.back().second;
    for (size_t i = track_.size(); i-- > 1;) {
        if (track_[i - 1].first <= t && t <= track_[i].first) {
            const int64_t span = track_[i].first - track_[i - 1].first;
            const float f = span > 0 ? float(t - track_[i - 1].first) / float(span) : 0.f;
            return slerp(track_[i - 1].second, track_[i].second, f);
        }
    }
    return track_.back().second;
}

Quat Stabilizer::correctionFor(int64_t timestampNs) {
    std::lock_guard<std::mutex> guard(lock_);
    const int64_t horizon = timestampNs + int64_t(lookaheadMs_) * 1'000'000;
    integrateLocked(horizon);
    if (track_.empty()) return {};

    // Low-pass the orientation track up to the look-ahead horizon: the smoothed
    // value is where we wish the camera had been pointing at frame time.
    const float tau = float(smoothingMs_) / 1000.0f;
    if (!haveSmoothed_) {
        smoothed_ = track_.front().second;
        lastSmoothedAt_ = track_.front().first;
        haveSmoothed_ = true;
    }
    for (const auto& [t, q] : track_) {
        if (t <= lastSmoothedAt_) continue;
        if (t > horizon) break;
        const float dt = float(t - lastSmoothedAt_) / 1e9f;
        lastSmoothedAt_ = t;
        const float alpha = tau > 0 ? 1.0f - std::exp(-dt / tau) : 1.0f;
        smoothed_ = slerp(smoothed_, q, alpha);
    }

    const Quat actual = orientationAtLocked(timestampNs);
    const Quat correction = normalize(conjugate(smoothed_) * actual);

    stats_.frames++;
    const float angle = 2.0f * std::acos((std::min)(1.0f, std::fabs(correction.w)));
    stats_.lastAngleDeg = angle * 57.2957795f;
    stats_.maxAngleDeg = (std::max)(stats_.maxAngleDeg, stats_.lastAngleDeg);
    return correction;
}

Quat Stabilizer::correctionAtRow(int64_t rowTimeNs) {
    std::lock_guard<std::mutex> guard(lock_);
    integrateLocked(rowTimeNs);
    if (track_.empty() || !haveSmoothed_) return {};
    return normalize(conjugate(smoothed_) * orientationAtLocked(rowTimeNs));
}

Quat Stabilizer::orientationAt(int64_t timestampNs) {
    std::lock_guard<std::mutex> guard(lock_);
    integrateLocked(timestampNs);
    return orientationAtLocked(timestampNs);
}

Stabilizer::Stats Stabilizer::stats() const {
    std::lock_guard<std::mutex> guard(lock_);
    return stats_;
}

} // namespace awc
