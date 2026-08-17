#pragma once
#include <cstdint>

#include "Stabilizer.h"

namespace awc {

/** Row-major 3x3, maps destination pixels to source pixels. */
struct Homography {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

/**
 * Builds the inverse warp for a frame: H = K * R^T * Kout^-1.
 *
 * [correction] is the rotation from where the camera actually pointed to where we
 * want it to point. [crop] < 1 shrinks the visible field to leave room for the
 * correction, and the result is scaled to the output size in the same pass.
 */
Homography stabilizationHomography(const CameraGeometry& geometry, const Quat& correction,
                                   uint32_t outWidth, uint32_t outHeight, float crop);

/** Plain crop + scale, used when there is no gyro data to work with. */
Homography cropHomography(uint32_t inWidth, uint32_t inHeight,
                          uint32_t outWidth, uint32_t outHeight, float crop);

/**
 * Bilinear NV12 resample through [h]. Source and destination may differ in size;
 * work is split across [threads] rows-wise.
 */
void warpNv12(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight,
              uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight,
              const Homography& h, unsigned threads);

/**
 * Rolling-shutter aware variant: [bands] holds homographies for evenly spaced row
 * boundaries (first = top of the frame, last = bottom) and each output row is warped
 * by the interpolation between its neighbours. This is what removes the "jello"
 * shear that a single per-frame rotation leaves behind.
 */
void warpNv12Banded(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight,
                    uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight,
                    const Homography* bands, uint32_t bandCount, unsigned threads);

} // namespace awc
