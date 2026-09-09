#include "nukex/calibration/background_gradient.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nukex {

namespace {

// A tilt is a large-scale property, so a decimated sample describes it as well
// as every pixel would and costs a fraction as much on a 24 MP frame.
constexpr int   SAMPLE_STEP     = 8;
constexpr int   MAX_ITERATIONS  = 6;
// Asymmetric clipping. Astronomical objects are POSITIVE excursions, so cutting
// positive residuals hard and negative ones loosely settles the plane onto the
// sky rather than onto the sky plus whatever is sitting on it.
constexpr float CLIP_HIGH_SIGMA = 1.0f;
constexpr float CLIP_LOW_SIGMA  = 3.0f;

float median_of(std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    const std::size_t k = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

} // namespace

GradientModel BackgroundGradient::fit_planar(const Image& img, int channel) {
    GradientModel model;
    if (img.empty() || channel < 0 || channel >= img.n_channels()) return model;

    const int w = img.width(), h = img.height();
    if (w < 4 * SAMPLE_STEP || h < 4 * SAMPLE_STEP) return model;

    struct Sample { double x, y, z; };
    std::vector<Sample> pts;
    pts.reserve(static_cast<std::size_t>((w / SAMPLE_STEP + 1)) * (h / SAMPLE_STEP + 1));
    for (int y = 0; y < h; y += SAMPLE_STEP)
        for (int x = 0; x < w; x += SAMPLE_STEP) {
            const float v = img.at(x, y, channel);
            if (!std::isfinite(v)) continue;
            // A zero is an uncovered voxel, not a dark measurement.
            if (v == 0.0f) continue;
            pts.push_back({static_cast<double>(x) / w - 0.5,
                           static_cast<double>(y) / h - 0.5,
                           static_cast<double>(v)});
        }
    if (pts.size() < 64) return model;

    std::vector<char> keep(pts.size(), 1);
    double c0 = 0.0, cx = 0.0, cy = 0.0;

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        // Normal equations for z = c0 + cx*x + cy*y over the kept samples.
        double n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        double sz = 0, sxz = 0, syz = 0;
        for (std::size_t i = 0; i < pts.size(); i++) {
            if (!keep[i]) continue;
            const double x = pts[i].x, y = pts[i].y, z = pts[i].z;
            n += 1; sx += x; sy += y;
            sxx += x * x; syy += y * y; sxy += x * y;
            sz += z; sxz += x * z; syz += y * z;
        }
        if (n < 32) break;

        // 3x3 solve by Cramer's rule; the design is well conditioned because
        // the samples span the frame.
        const double m[3][3] = {{n, sx, sy}, {sx, sxx, sxy}, {sy, sxy, syy}};
        const double det =
            m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
          - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
          + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (std::fabs(det) < 1e-18) break;

        const double b[3] = {sz, sxz, syz};
        auto solve = [&](int col) {
            double a[3][3];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) a[r][c] = (c == col) ? b[r] : m[r][c];
            return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                  - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                  + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;
        };
        c0 = solve(0); cx = solve(1); cy = solve(2);

        std::vector<float> resid;
        resid.reserve(pts.size());
        for (std::size_t i = 0; i < pts.size(); i++)
            resid.push_back(static_cast<float>(pts[i].z - (c0 + cx * pts[i].x + cy * pts[i].y)));

        std::vector<float> tmp = resid;
        const float med = median_of(tmp);
        for (auto& r : tmp) r = std::fabs(r - med);
        const float sigma = 1.4826f * median_of(tmp);
        if (!(sigma > 0.0f)) break;

        std::size_t kept = 0;
        for (std::size_t i = 0; i < pts.size(); i++) {
            keep[i] = (resid[i] < med + CLIP_HIGH_SIGMA * sigma &&
                       resid[i] > med - CLIP_LOW_SIGMA * sigma) ? 1 : 0;
            kept += keep[i];
        }
        if (kept < pts.size() / 10) break;
    }

    model.level = c0;
    model.dx    = cx;      // x spans -0.5..+0.5, so cx IS the edge-to-edge change
    model.dy    = cy;
    model.valid = true;
    return model;
}

void BackgroundGradient::subtract(Image& img, int channel, const GradientModel& model) {
    if (!model.valid || img.empty()) return;
    if (channel < 0 || channel >= img.n_channels()) return;

    const int w = img.width(), h = img.height();
    for (int y = 0; y < h; y++) {
        const double fy = static_cast<double>(y) / h - 0.5;
        for (int x = 0; x < w; x++) {
            float& v = img.at(x, y, channel);
            if (v == 0.0f) continue;          // uncovered stays uncovered
            const double fx = static_cast<double>(x) / w - 0.5;
            // The TILT only: `level` is deliberately left in place.
            v = static_cast<float>(v - (model.dx * fx + model.dy * fy));
        }
    }
}

double BackgroundGradient::amplitude(const GradientModel& model) {
    if (!model.valid) return 0.0;
    return std::hypot(model.dx, model.dy);
}

} // namespace nukex
