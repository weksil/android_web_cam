#include "ExposureControl.h"

#include <algorithm>
#include <cmath>

namespace awc {

namespace {
constexpr int64_t kMainsPeriodNs = 10'000'000;    // 50 Hz
constexpr int64_t kFlickerFreeMin = kMainsPeriodNs;
} // namespace

void ExposureControl::setLimits(int isoMin, int isoMax, int64_t exposureMinNs,
                                int64_t exposureMaxNs) {
    isoMin_ = isoMin;
    isoMax_ = isoMax;
    exposureMinNs_ = exposureMinNs;
    exposureMaxNs_ = exposureMaxNs;
    if (iso_ == 0) iso_ = (std::max)(isoMin, (std::min)(isoMax, 400));
    clamp();
}

void ExposureControl::reset() {
    exposureNs_ = kMainsPeriodNs;
    iso_ = isoMax_ > 0 ? (std::max)(isoMin_, (std::min)(isoMax_, 400)) : 0;
    dirty_ = true;
    settle_ = 0;
}

void ExposureControl::clamp() {
    if (exposureMaxNs_ > 0) {
        exposureNs_ = (std::max)(exposureMinNs_, (std::min)(exposureMaxNs_, exposureNs_));
        exposureNs_ = (std::min)(exposureNs_, int64_t(33'000'000));   // keep 30 fps
    }
    if (isoMax_ > 0) iso_ = (std::max)(isoMin_, (std::min)(isoMax_, iso_));
}

void ExposureControl::update(float meanLuma) {
    if (!haveLimits() || meanLuma <= 0.5f) return;
    if (--settle_ > 0) return;            // let a change take effect before reacting
    settle_ = 6;

    const float error = kTargetLuma / meanLuma;
    if (error > 0.93f && error < 1.07f) return;         // close enough, avoid pumping

    const int previousIso = iso_;
    const int64_t previousExposure = exposureNs_;

    // Gain first, damped in the multiplicative domain.
    const float step = std::pow(error, 0.5f);
    iso_ = int(std::lround(float(iso_) * step));
    clamp();

    // Only when gain has nothing left to give does the exposure move, and it moves in
    // whole mains periods so the picture stays free of banding.
    if (iso_ >= isoMax_ && error > 1.07f) {
        exposureNs_ = (std::min)(int64_t(33'000'000), exposureNs_ + kMainsPeriodNs);
    } else if (iso_ <= isoMin_ && error < 0.93f) {
        exposureNs_ = exposureNs_ > kFlickerFreeMin ? exposureNs_ - kMainsPeriodNs
                                                   : exposureNs_ / 2;
    }
    clamp();

    if (iso_ != previousIso || exposureNs_ != previousExposure) dirty_ = true;
}

bool ExposureControl::changed() {
    const bool was = dirty_;
    dirty_ = false;
    return was;
}

} // namespace awc
