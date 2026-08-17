#pragma once
#include <cstdint>

namespace awc {

/**
 * Keeps the exposure short and trades brightness for ISO instead.
 *
 * A dim room drives auto exposure to 25 ms of a 33 ms frame, which smears every
 * vibration inside the frame - stabilization corrects geometry, not blur, so the
 * only cure is a shorter exposure. Brightness is then recovered with gain.
 *
 * The exposure ladder stays on multiples of the mains period (10 ms at 50 Hz) while
 * the picture needs light: an exposure that is not a whole number of periods samples
 * a different part of the flicker in every row and bands the frame. Below 10 ms is
 * only used when the scene is bright enough that flicker is unlikely to matter.
 */
class ExposureControl {
public:
    void setLimits(int isoMin, int isoMax, int64_t exposureMinNs, int64_t exposureMaxNs);
    bool haveLimits() const { return isoMax_ > 0; }
    void reset();

    /** Feeds the mean luma (0..255) of a decoded frame. */
    void update(float meanLuma);

    int64_t exposureNs() const { return exposureNs_; }
    int iso() const { return iso_; }
    bool changed();                       // true once after the setpoint moves

    static constexpr float kTargetLuma = 115.0f;

private:
    void clamp();

    int isoMin_ = 0, isoMax_ = 0;
    int64_t exposureMinNs_ = 0, exposureMaxNs_ = 0;
    int64_t exposureNs_ = 10'000'000;     // one 50 Hz period
    int iso_ = 0;
    bool dirty_ = false;
    int settle_ = 0;
};

} // namespace awc
