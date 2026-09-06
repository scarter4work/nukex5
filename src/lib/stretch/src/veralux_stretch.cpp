#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

namespace {

/// The HMS core curve, normalised to [0,1] over [SP,1].
///   f(x) = [asinh(D*(x-SP)+b) - asinh(b)] / [asinh(D*(1-SP)+b) - asinh(b)]
/// `den` is the denominator, hoisted because apply() evaluates this 72
/// million times on a 24 MP colour frame and it does not vary per pixel.
inline float hms_curve(float x, float D, float b, float SP, float den) {
    if (x <= SP) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return std::clamp((std::asinh(D * (x - SP) + b) - std::asinh(b)) / den,
                      0.0f, 1.0f);
}

inline float hms_den(float D, float b, float SP) {
    return std::asinh(D * (1.0f - SP) + b) - std::asinh(b);
}

} // namespace

float VeraLuxStretch::apply_scalar(float x) const {
    const float D = std::pow(10.0f, log_D);
    const float den = hms_den(D, protect_b, SP);
    if (den < 1e-10f) return x;  // Degenerate case
    return hms_curve(x, D, protect_b, SP, den);
}

float VeraLuxStretch::auto_tune(const Image& img, float target_background) {
    // Robust background of the quantity apply() actually stretches: the
    // sensor-weighted luminance for a colour image, channel 0 for a mono one.
    // Tuning against a single channel instead is not a rounding difference --
    // on the user's own stack the background is imbalanced (red 0.0270, green
    // 0.0418), so a shadow point taken from green sits above the luminance of
    // 99.6% of the frame and blacks it out. Under the old SP = 0 curve the
    // same mismatch was a quiet bias: it is why that stretch put its
    // background at 0.2569 while solving for 0.2500.
    //
    // Sampled on a stride -- a full sort of a 24 MP frame costs more than the
    // whole stretch, to produce one number.
    const int n = img.width() * img.height();
    if (n <= 0) return log_D;
    const bool colour = img.n_channels() >= 3;
    const float* c0 = img.channel_data(0);
    const float* c1 = colour ? img.channel_data(1) : nullptr;
    const float* c2 = colour ? img.channel_data(2) : nullptr;

    const int stride = std::max(1, n / 200000);
    std::vector<float> sample;
    sample.reserve(static_cast<std::size_t>(n / stride) + 1);
    for (int i = 0; i < n; i += stride)
        sample.push_back(colour ? (w_R * c0[i] + w_G * c1[i] + w_B * c2[i])
                                : c0[i]);
    if (sample.empty()) return log_D;
    const std::size_t mid = sample.size() / 2;
    std::nth_element(sample.begin(), sample.begin() + mid, sample.end());
    const float bg = sample[mid];
    if (!(bg > 0.0f) || bg >= 1.0f) return log_D;

    // Solve the SHADOW POINT as well as the intensity. Positioning and
    // contrast are separate problems: solving the intensity MOVES the
    // histogram, only a shadow point WIDENS it. Measured on a 74-frame stack
    // whose p99.9 sits 44 sigma above the background, SP = 0 delivered that
    // signal as 4% of the output range (p50 0.2569 -> p99.9 0.2969) because
    // the whole band rides on a pedestal that eats 98% of the curve. Solving
    // both took the spread from 0.0393 to 0.2853 with the background still
    // exactly on target.
    //
    // median - 2.8 sigma is the convention PixInsight's own STF autostretch
    // uses, with sigma from the MAD (x1.4826 for consistency with a normal)
    // so a bright nebula cannot inflate the noise estimate.
    for (float& v : sample) v = std::abs(v - bg);
    std::nth_element(sample.begin(), sample.begin() + mid, sample.end());
    const float sigma = 1.4826f * sample[mid];

    const float saved_SP = SP;
    const float saved    = log_D;
    SP = (sigma > 0.0f) ? std::max(0.0f, bg - 2.8f * sigma) : 0.0f;

    // A band too tight to clip: if no intensity in range can still put the
    // background on target -- which happens when the noise floor is so narrow
    // that (bg - SP) underflows the curve -- clipping buys nothing and costs
    // the positioning. Fall back to the shadow point at black.
    log_D = 7.0f;
    if (apply_scalar(bg) < target_background) SP = 0.0f;

    // apply_scalar is monotonically increasing in log_D at fixed x, so a
    // bisection is exact enough in a handful of steps.
    float lo = 0.0f, hi = 7.0f;
    for (int i = 0; i < 40; ++i) {
        const float mid_ld = 0.5f * (lo + hi);
        log_D = mid_ld;
        if (apply_scalar(bg) < target_background) lo = mid_ld; else hi = mid_ld;
    }
    log_D = 0.5f * (lo + hi);
    if (!std::isfinite(log_D)) log_D = saved;
    if (!std::isfinite(SP) || SP < 0.0f || SP >= 1.0f) SP = saved_SP;
    return log_D;
}

void VeraLuxStretch::apply(Image& img) const {
    int n = img.width() * img.height();
    int nch = img.n_channels();

    float D = std::pow(10.0f, log_D);
    float b = protect_b;
    float den = hms_den(D, b, SP);
    if (den < 1e-10f) return;

    if (nch < 3) {
        // Single channel: apply scalar stretch. Inlined against the hoisted
        // D/den rather than calling apply_scalar, which would recompute a pow
        // and two asinh for every one of 24 million pixels.
        float* data = img.channel_data(0);
        for (int i = 0; i < n; i++)
            data[i] = hms_curve(data[i], D, b, SP, den);
        clamp_image(img);
        return;
    }

    // Multi-channel: color vector preservation with convergence-to-white
    float* R = img.channel_data(0);
    float* G = img.channel_data(1);
    float* B = img.channel_data(2);

    constexpr float eps = 1e-9f;

    for (int i = 0; i < n; i++) {
        float r = R[i], g = G[i], bv = B[i];

        // Sensor-weighted luminance
        float L = w_R * r + w_G * g + w_B * bv;
        // At or below the shadow point the pixel is background: it must go to
        // black, not keep its linear value.
        if (L <= SP || L < eps) { R[i] = G[i] = B[i] = 0.0f; continue; }

        // Chromaticity ratios (color direction)
        float r_ratio = r / (L + eps);
        float g_ratio = g / (L + eps);
        float b_ratio = bv / (L + eps);

        // Stretch luminance
        float L_stretched = hms_curve(L, D, b, SP, den);

        // Convergence factor: bright pixels transition toward white
        // k approaches 1 for bright pixels, 0 for faint pixels
        float k = std::pow(L_stretched, convergence_power);

        // Reconstruct: blend between original color ratio and white (1,1,1)
        R[i] = std::clamp(L_stretched * (r_ratio * (1.0f - k) + k), 0.0f, 1.0f);
        G[i] = std::clamp(L_stretched * (g_ratio * (1.0f - k) + k), 0.0f, 1.0f);
        B[i] = std::clamp(L_stretched * (b_ratio * (1.0f - k) + k), 0.0f, 1.0f);
    }
}

std::map<std::string, std::pair<float, float>> VeraLuxStretch::param_bounds() const {
    return {
        {"SP",                {0.0f,  1.0f}},
        {"log_D",             {0.0f,  7.0f}},
        {"protect_b",         {0.1f, 15.0f}},
        {"convergence_power", {1.0f, 10.0f}},
    };
}

bool VeraLuxStretch::set_param(const std::string& n, float v) {
    if (n == "SP")                { SP = v;                return true; }
    if (n == "log_D")             { log_D = v;             return true; }
    if (n == "protect_b")         { protect_b = v;         return true; }
    if (n == "convergence_power") { convergence_power = v; return true; }
    return false;
}

std::optional<float> VeraLuxStretch::get_param(const std::string& n) const {
    if (n == "SP")                return SP;
    if (n == "log_D")             return log_D;
    if (n == "protect_b")         return protect_b;
    if (n == "convergence_power") return convergence_power;
    return std::nullopt;
}

} // namespace nukex
