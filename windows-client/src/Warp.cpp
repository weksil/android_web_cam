#include "Warp.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace awc {

namespace {

void multiply(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] +
                             a[r * 3 + 2] * b[2 * 3 + c];
        }
    }
}

void rotationMatrix(const Quat& q, float m[9]) {
    const float w = q.w, x = q.x, y = q.y, z = q.z;
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y - w * z);
    m[2] = 2 * (x * z + w * y);
    m[3] = 2 * (x * y + w * z);
    m[4] = 1 - 2 * (x * x + z * z);
    m[5] = 2 * (y * z - w * x);
    m[6] = 2 * (x * z - w * y);
    m[7] = 2 * (y * z + w * x);
    m[8] = 1 - 2 * (x * x + y * y);
}

inline uint8_t sample(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t stride,
                      float x, float y, uint32_t channel, uint32_t channels) {
    x = (std::max)(0.0f, (std::min)(x, float(width - 1)));
    y = (std::max)(0.0f, (std::min)(y, float(height - 1)));
    const uint32_t x0 = uint32_t(x), y0 = uint32_t(y);
    const uint32_t x1 = (std::min)(x0 + 1, width - 1), y1 = (std::min)(y0 + 1, height - 1);
    const float fx = x - x0, fy = y - y0;

    const uint8_t* row0 = plane + size_t(y0) * stride;
    const uint8_t* row1 = plane + size_t(y1) * stride;
    const float a = row0[x0 * channels + channel], b = row0[x1 * channels + channel];
    const float c = row1[x0 * channels + channel], d = row1[x1 * channels + channel];
    const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
    return uint8_t(top + (bottom - top) * fy + 0.5f);
}

} // namespace

Homography stabilizationHomography(const CameraGeometry& geometry, const Quat& correction,
                                   uint32_t outWidth, uint32_t outHeight, float crop) {
    // Virtual output camera: same centre, focal length scaled so the cropped field
    // of view exactly fills the output frame.
    const float scale = float(outWidth) / (float(geometry.width) * crop);
    const float fxOut = geometry.fx * scale, fyOut = geometry.fy * scale;
    const float cxOut = outWidth * 0.5f, cyOut = outHeight * 0.5f;

    const float kOutInverse[9] = {
        1.0f / fxOut, 0.0f, -cxOut / fxOut,
        0.0f, 1.0f / fyOut, -cyOut / fyOut,
        0.0f, 0.0f, 1.0f,
    };
    const float k[9] = {
        geometry.fx, 0.0f, geometry.cx,
        0.0f, geometry.fy, geometry.cy,
        0.0f, 0.0f, 1.0f,
    };

    float r[9];
    rotationMatrix(correction, r);
    const float transposed[9] = {r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8]};

    float temp[9], result[9];
    multiply(k, transposed, temp);
    multiply(temp, kOutInverse, result);

    Homography h;
    std::copy(result, result + 9, h.m);
    return h;
}

Homography cropHomography(uint32_t inWidth, uint32_t inHeight,
                          uint32_t outWidth, uint32_t outHeight, float crop) {
    const float sx = float(inWidth) * crop / float(outWidth);
    const float sy = float(inHeight) * crop / float(outHeight);
    const float offsetX = (float(inWidth) - float(inWidth) * crop) * 0.5f;
    const float offsetY = (float(inHeight) - float(inHeight) * crop) * 0.5f;
    Homography h;
    h.m[0] = sx; h.m[1] = 0;  h.m[2] = offsetX;
    h.m[3] = 0;  h.m[4] = sy; h.m[5] = offsetY;
    h.m[6] = 0;  h.m[7] = 0;  h.m[8] = 1;
    return h;
}

void warpNv12Banded(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight,
                    uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight,
                    const Homography* bands, uint32_t bandCount, unsigned threads) {
    if (bandCount < 2) {
        warpNv12(src, srcWidth, srcHeight, dst, dstWidth, dstHeight,
                 bandCount == 1 ? bands[0] : Homography{}, threads);
        return;
    }

    const uint8_t* srcLuma = src;
    const uint8_t* srcChroma = src + size_t(srcWidth) * srcHeight;
    uint8_t* dstLuma = dst;
    uint8_t* dstChroma = dst + size_t(dstWidth) * dstHeight;
    const uint32_t srcChromaWidth = srcWidth / 2, srcChromaHeight = srcHeight / 2;
    const uint32_t dstChromaWidth = dstWidth / 2, dstChromaHeight = dstHeight / 2;

    // Homography for a row, interpolated between the two nearest band boundaries.
    auto rowMatrix = [&](float rowFraction, float out[9]) {
        const float position = rowFraction * float(bandCount - 1);
        const uint32_t index = (std::min)(uint32_t(position), bandCount - 2);
        const float f = position - float(index);
        const float* a = bands[index].m;
        const float* b = bands[index + 1].m;
        for (int i = 0; i < 9; ++i) out[i] = a[i] + (b[i] - a[i]) * f;
    };

    auto lumaRows = [&](uint32_t from, uint32_t to) {
        float m[9];
        for (uint32_t v = from; v < to; ++v) {
            rowMatrix(dstHeight > 1 ? float(v) / float(dstHeight - 1) : 0.f, m);
            const float y = v + 0.5f;
            uint8_t* row = dstLuma + size_t(v) * dstWidth;
            for (uint32_t u = 0; u < dstWidth; ++u) {
                const float x = u + 0.5f;
                const float w = m[6] * x + m[7] * y + m[8];
                const float inv = w != 0.0f ? 1.0f / w : 0.0f;
                const float sx = (m[0] * x + m[1] * y + m[2]) * inv - 0.5f;
                const float sy = (m[3] * x + m[4] * y + m[5]) * inv - 0.5f;
                row[u] = sample(srcLuma, srcWidth, srcHeight, srcWidth, sx, sy, 0, 1);
            }
        }
    };

    auto chromaRows = [&](uint32_t from, uint32_t to) {
        float m[9];
        for (uint32_t v = from; v < to; ++v) {
            const float y = v * 2.0f + 1.0f;
            rowMatrix(dstHeight > 1 ? y / float(dstHeight - 1) : 0.f, m);
            uint8_t* row = dstChroma + size_t(v) * dstWidth;
            for (uint32_t u = 0; u < dstChromaWidth; ++u) {
                const float x = u * 2.0f + 1.0f;
                const float w = m[6] * x + m[7] * y + m[8];
                const float inv = w != 0.0f ? 1.0f / w : 0.0f;
                const float sx = ((m[0] * x + m[1] * y + m[2]) * inv) * 0.5f - 0.5f;
                const float sy = ((m[3] * x + m[4] * y + m[5]) * inv) * 0.5f - 0.5f;
                row[u * 2] = sample(srcChroma, srcChromaWidth, srcChromaHeight, srcWidth,
                                    sx, sy, 0, 2);
                row[u * 2 + 1] = sample(srcChroma, srcChromaWidth, srcChromaHeight, srcWidth,
                                        sx, sy, 1, 2);
            }
        }
    };

    const unsigned workers = (std::max)(1u, (std::min)(threads, 16u));
    std::vector<std::thread> pool;
    pool.reserve(workers * 2);
    const uint32_t lumaChunk = (dstHeight + workers - 1) / workers;
    for (unsigned i = 0; i < workers; ++i) {
        const uint32_t from = i * lumaChunk, to = (std::min)(from + lumaChunk, dstHeight);
        if (from < to) pool.emplace_back(lumaRows, from, to);
    }
    const uint32_t chromaChunk = (dstChromaHeight + workers - 1) / workers;
    for (unsigned i = 0; i < workers; ++i) {
        const uint32_t from = i * chromaChunk, to = (std::min)(from + chromaChunk, dstChromaHeight);
        if (from < to) pool.emplace_back(chromaRows, from, to);
    }
    for (std::thread& t : pool) t.join();
}

void warpNv12(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight,
              uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight,
              const Homography& h, unsigned threads) {
    const uint8_t* srcLuma = src;
    const uint8_t* srcChroma = src + size_t(srcWidth) * srcHeight;
    uint8_t* dstLuma = dst;
    uint8_t* dstChroma = dst + size_t(dstWidth) * dstHeight;
    const uint32_t srcChromaWidth = srcWidth / 2, srcChromaHeight = srcHeight / 2;
    const uint32_t dstChromaWidth = dstWidth / 2, dstChromaHeight = dstHeight / 2;
    const float* m = h.m;

    auto lumaRows = [&](uint32_t from, uint32_t to) {
        for (uint32_t v = from; v < to; ++v) {
            const float y = v + 0.5f;
            uint8_t* row = dstLuma + size_t(v) * dstWidth;
            for (uint32_t u = 0; u < dstWidth; ++u) {
                const float x = u + 0.5f;
                const float w = m[6] * x + m[7] * y + m[8];
                const float inv = w != 0.0f ? 1.0f / w : 0.0f;
                const float sx = (m[0] * x + m[1] * y + m[2]) * inv - 0.5f;
                const float sy = (m[3] * x + m[4] * y + m[5]) * inv - 0.5f;
                row[u] = sample(srcLuma, srcWidth, srcHeight, srcWidth, sx, sy, 0, 1);
            }
        }
    };

    auto chromaRows = [&](uint32_t from, uint32_t to) {
        for (uint32_t v = from; v < to; ++v) {
            const float y = v * 2.0f + 1.0f;                    // centre of the 2x2 block
            uint8_t* row = dstChroma + size_t(v) * dstWidth;
            for (uint32_t u = 0; u < dstChromaWidth; ++u) {
                const float x = u * 2.0f + 1.0f;
                const float w = m[6] * x + m[7] * y + m[8];
                const float inv = w != 0.0f ? 1.0f / w : 0.0f;
                const float sx = ((m[0] * x + m[1] * y + m[2]) * inv) * 0.5f - 0.5f;
                const float sy = ((m[3] * x + m[4] * y + m[5]) * inv) * 0.5f - 0.5f;
                row[u * 2] = sample(srcChroma, srcChromaWidth, srcChromaHeight, srcWidth,
                                    sx, sy, 0, 2);
                row[u * 2 + 1] = sample(srcChroma, srcChromaWidth, srcChromaHeight, srcWidth,
                                        sx, sy, 1, 2);
            }
        }
    };

    const unsigned workers = (std::max)(1u, (std::min)(threads, 16u));
    std::vector<std::thread> pool;
    pool.reserve(workers * 2);

    const uint32_t lumaChunk = (dstHeight + workers - 1) / workers;
    for (unsigned i = 0; i < workers; ++i) {
        const uint32_t from = i * lumaChunk;
        const uint32_t to = (std::min)(from + lumaChunk, dstHeight);
        if (from < to) pool.emplace_back(lumaRows, from, to);
    }
    const uint32_t chromaChunk = (dstChromaHeight + workers - 1) / workers;
    for (unsigned i = 0; i < workers; ++i) {
        const uint32_t from = i * chromaChunk;
        const uint32_t to = (std::min)(from + chromaChunk, dstChromaHeight);
        if (from < to) pool.emplace_back(chromaRows, from, to);
    }
    for (std::thread& t : pool) t.join();
}

} // namespace awc
