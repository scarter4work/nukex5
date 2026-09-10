#include "nukex/gpu/gpu_cpu_fallback.hpp"
#include "nukex/core/noise_model.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

// ── Helper: insertion sort for small arrays (matches OpenCL kernel) ──
static void insertion_sort(float* arr, int n) {
    for (int i = 1; i < n; i++) {
        float key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

// ── Helper: median via insertion sort (matches OpenCL kernel) ──
static float sorted_median(float* sorted, int n) {
    if (n <= 0) return 0.0f;
    if (n % 2 == 1) return sorted[n / 2];
    return 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]);
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 1: classify_weights
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::classify_weights(
    ShadowBuffers& buf,
    const FrameStats* frame_stats,
    const WeightConfig& config,
    int batch_size, int n_channels, int n_frames) {

    int B = batch_size;
    int N = n_frames;
    int C = n_channels;

    for (int vi = 0; vi < B; vi++) {
        // Per-channel frame counts: two slots can read different caches with
        // different frame sets, so there is no single count for the voxel.
        int nf_any = 0;
        for (int ch = 0; ch < C; ch++) nf_any = std::max(nf_any, (int)buf.n_frames[ch * B + vi]);
        if (nf_any == 0) continue;

        float worst_sigma = 0.0f;
        float best_sigma = 1e30f;
        float weight_sum = 0.0f;
        float total_exp = 0.0f;
        uint16_t cloud_count = 0;
        uint16_t trail_count = 0;

        for (int ch = 0; ch < C; ch++) {
            const int nf = buf.n_frames[ch * B + vi];
            // Welford stats for this voxel-channel
            float w_mean = buf.welford_mean[ch * B + vi];
            float w_M2   = buf.welford_M2[ch * B + vi];
            uint32_t w_n = buf.welford_n[ch * B + vi];

            float variance = (w_n > 1) ? std::max(0.0f, w_M2) / static_cast<float>(w_n - 1) : 0.0f;
            float stddev = std::sqrt(variance);

            for (int fi = 0; fi < nf; fi++) {
                float value = buf.pixel_values[ch * N * B + fi * B + vi];

                // Weight computation (mirrors weight_computer.cpp)
                const int gf = buf.global_frame_of.empty()
                             ? fi : buf.global_frame_of[ch * N + fi];
                if (gf < 0) continue;   // this channel has no frame here

                // An uncovered sample is an absence, not a dark measurement.
                // Weight it to EXACTLY zero -- before weight_floor, which
                // would otherwise give it a vote -- so every later stage
                // ignores it without needing to know why.
                if (!buf.sample_valid(ch, fi, vi, B)) {
                    buf.pixel_weights[ch * N * B + fi * B + vi] = 0.0f;
                    continue;
                }
                const FrameStats& fst = frame_stats[gf];
                float w = fst.frame_weight * fst.psf_weight;

                if (stddev > 1e-30f) {
                    float sigma_score = std::fabs(value - w_mean) / stddev;
                    float excess = std::max(0.0f, sigma_score - config.sigma_threshold);
                    float sigma_factor = std::exp(-0.5f * excess * excess
                                                  / (config.sigma_scale * config.sigma_scale));
                    w *= sigma_factor;

                    // Track sigma scores for channel 0 only
                    if (ch == 0) {
                        if (sigma_score > worst_sigma) worst_sigma = sigma_score;
                        if (sigma_score < best_sigma) best_sigma = sigma_score;
                    }
                }

                w *= fst.cloud_score;
                w = std::max(w, config.weight_floor);

                buf.pixel_weights[ch * N * B + fi * B + vi] = w;

                // Accumulate summaries from channel 0
                if (ch == 0) {
                    weight_sum += w;
                    total_exp += fst.exposure;
                    if (fst.cloud_score < 0.5f) cloud_count++;
                }
            }
        }

        buf.cloud_frame_count[vi] = cloud_count;
        buf.trail_frame_count[vi] = trail_count;
        buf.worst_sigma_score[vi] = worst_sigma;
        buf.best_sigma_score[vi]  = (best_sigma < 1e29f) ? best_sigma : 0.0f;
        // weight_sum, total_exp and the counts are accumulated from channel 0
        // only (see the `if (ch == 0)` guard above), so the divisor is
        // channel 0's own frame count -- not the voxel's, which no longer
        // exists as a single number.
        const int nf0 = buf.n_frames[0 * B + vi];
        buf.mean_weight_out[vi]   = (nf0 > 0) ? weight_sum / static_cast<float>(nf0) : 0.0f;
        buf.total_exposure_out[vi] = total_exp;
    }
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 2: robust_stats
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::robust_stats(
    ShadowBuffers& buf,
    int batch_size, int n_channels, int n_frames) {

    int B = batch_size;
    int N = n_frames;
    int C = n_channels;

    for (int vi = 0; vi < B; vi++) {
        for (int ch = 0; ch < C; ch++) {
            // Per-channel frame count: a channel with fewer than two samples
            // has no spread to measure, and that is now a per-channel
            // question rather than a per-voxel one.
            const int nf = buf.n_frames[ch * B + vi];
            if (nf < 2) {
                buf.mad_out[ch * B + vi] = 0.0f;
                buf.biweight_midvar_out[ch * B + vi] = 0.0f;
                buf.iqr_out[ch * B + vi] = 0.0f;
                continue;
            }
            // Collect the COVERED values for this voxel-channel. An
            // uncovered sample would otherwise drag the median and the MAD
            // toward zero exactly at the frame edges.
            float vals[GPU_MAX_FRAMES];
            float sorted[GPU_MAX_FRAMES];
            int n = 0;
            {
                const int navail = std::min(nf, static_cast<int>(GPU_MAX_FRAMES));
                for (int fi = 0; fi < navail; fi++)
                    if (buf.sample_valid(ch, fi, vi, B))
                        vals[n++] = buf.pixel_values[ch * N * B + fi * B + vi];
            }
            if (n < 2) {
                buf.mad_out[ch * B + vi] = 0.0f;
                buf.biweight_midvar_out[ch * B + vi] = 0.0f;
                buf.iqr_out[ch * B + vi] = 0.0f;
                continue;
            }

            // ── MAD ──
            // Sort to find median
            for (int i = 0; i < n; i++) sorted[i] = vals[i];
            insertion_sort(sorted, n);
            float med = sorted_median(sorted, n);

            // Absolute deviations from median
            float abs_devs[GPU_MAX_FRAMES];
            for (int i = 0; i < n; i++)
                abs_devs[i] = std::fabs(vals[i] - med);
            insertion_sort(abs_devs, n);
            float mad_val = sorted_median(abs_devs, n);
            buf.mad_out[ch * B + vi] = mad_val;

            // ── IQR ──
            int q1_idx = n / 4;
            int q3_idx = (3 * n) / 4;
            buf.iqr_out[ch * B + vi] = (n >= 4) ? sorted[q3_idx] - sorted[q1_idx] : 0.0f;

            // ── Biweight midvariance ──
            constexpr float c_bw = 9.0f;
            if (mad_val < 1e-30f) {
                buf.biweight_midvar_out[ch * B + vi] = 0.0f;
                continue;
            }

            double num = 0.0, den = 0.0;
            for (int i = 0; i < n; i++) {
                float u = (vals[i] - med) / (c_bw * mad_val);
                if (std::fabs(u) < 1.0f) {
                    float u2 = u * u;
                    float diff = vals[i] - med;
                    num += diff * diff * std::pow(1.0f - u2, 4);
                    den += (1.0f - u2) * (1.0f - 5.0f * u2);
                }
            }
            float bwmv = (std::fabs(den) > 1e-30)
                ? static_cast<float>(n * num / (den * den))
                : 0.0f;
            buf.biweight_midvar_out[ch * B + vi] = bwmv;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 3: select_pixels
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::select_pixels(
    ShadowBuffers& buf,
    const FrameStats* frame_stats,
    int batch_size, int n_channels, int n_frames) {

    int B = batch_size;
    int N = n_frames;
    int C = n_channels;

    for (int vi = 0; vi < B; vi++) {
        for (int ch = 0; ch < C; ch++) {
            const int nf = buf.n_frames[ch * B + vi];
            float out_val = buf.dist_true_signal[ch * B + vi];

            // Noise propagation
            double weight_sum = 0.0;
            double variance_sum = 0.0;

            // Across-frame fallback scale, used when a frame carries no
            // usable GAIN/RDNOISE keywords.
            //
            // Welford variance is NOT robust: a single satellite trail or
            // cosmic ray inflates it, so the PREDICTED noise rises to meet
            // whatever the estimator actually produced and the
            // measured-vs-predicted check goes blind to estimator-injected
            // noise. Kernel 2 has already computed a robust MAD for this
            // voxel-channel, so use it, and keep Welford only where the robust
            // scale is degenerate (identical samples, or too few to form one).
            float w_M2 = buf.welford_M2[ch * B + vi];
            uint32_t w_n = buf.welford_n[ch * B + vi];
            float welford_var = (w_n > 1)
                ? std::max(0.0f, w_M2) / static_cast<float>(w_n - 1)
                : 0.0f;
            const float robust_sigma = buf.mad_out[ch * B + vi] * 1.4826f;
            const float fallback_var = (robust_sigma > 0.0f)
                                     ? robust_sigma * robust_sigma
                                     : welford_var;

            for (int fi = 0; fi < nf; fi++) {
                float w = buf.pixel_weights[ch * N * B + fi * B + vi];
                float value = buf.pixel_values[ch * N * B + fi * B + vi];

                const int gf = buf.global_frame_of.empty()
                             ? fi : buf.global_frame_of[ch * N + fi];
                if (gf < 0) continue;
                const FrameStats& fst = frame_stats[gf];
                // CCD noise model, or the across-frame fallback when the frame
                // has no usable keywords. `ch` is the cube slot. The model is
                // NoiseModel::sample_variance -- the one CPU copy, and the one
                // the unit tests cover; select_pixels.cl restates the same
                // arithmetic for the GPU and test_gpu_agreement holds the two
                // together.
                const float sigma2 = NoiseModel::sample_variance(value, fst, ch, fallback_var);

                weight_sum += static_cast<double>(w);
                variance_sum += static_cast<double>(w) * static_cast<double>(w)
                              * static_cast<double>(sigma2);
            }

            float noise = 0.0f;
            if (weight_sum > 1e-30) {
                noise = static_cast<float>(std::sqrt(variance_sum) / weight_sum);
            }

            float snr = (noise > 1e-30f)
                ? std::clamp(out_val / noise, 0.0f, 9999.0f)
                : 0.0f;

            buf.output_value[ch * B + vi] = out_val;
            buf.noise_sigma[ch * B + vi] = noise;
            buf.snr_out[ch * B + vi] = snr;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 4: spatial_context
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::spatial_context(
    const float* stacked_data,
    int width, int height, int n_channels,
    float* gradient_mag,
    float* local_background,
    float* local_rms) {

    constexpr int WINDOW_RADIUS = 7;  // 15x15 window

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int pi = y * width + x;

            // ── Sobel gradient (max across channels) ──
            float max_grad = 0.0f;
            for (int ch = 0; ch < n_channels; ch++) {
                const float* img = stacked_data + ch * width * height;

                // Clamp neighbors
                int xm = std::max(0, x - 1), xp = std::min(width - 1, x + 1);
                int ym = std::max(0, y - 1), yp = std::min(height - 1, y + 1);

                float gx = -img[ym * width + xm] + img[ym * width + xp]
                          - 2.0f * img[y * width + xm] + 2.0f * img[y * width + xp]
                          - img[yp * width + xm] + img[yp * width + xp];

                float gy = -img[ym * width + xm] - 2.0f * img[ym * width + x] - img[ym * width + xp]
                          + img[yp * width + xm] + 2.0f * img[yp * width + x] + img[yp * width + xp];

                float g = std::sqrt(gx * gx + gy * gy);
                if (g > max_grad) max_grad = g;
            }
            gradient_mag[pi] = max_grad;

            // ── Local background and RMS (15×15 window, biweight) ──
            // Collect luminance values in the window
            float window[225];  // 15×15 max
            int wn = 0;

            int y0 = std::max(0, y - WINDOW_RADIUS);
            int y1 = std::min(height - 1, y + WINDOW_RADIUS);
            int x0 = std::max(0, x - WINDOW_RADIUS);
            int x1 = std::min(width - 1, x + WINDOW_RADIUS);

            for (int wy = y0; wy <= y1; wy++) {
                for (int wx = x0; wx <= x1; wx++) {
                    float lum = 0.0f;
                    if (n_channels >= 3) {
                        lum = 0.2126f * stacked_data[0 * width * height + wy * width + wx]
                            + 0.7152f * stacked_data[1 * width * height + wy * width + wx]
                            + 0.0722f * stacked_data[2 * width * height + wy * width + wx];
                    } else {
                        lum = stacked_data[wy * width + wx];
                    }
                    window[wn++] = lum;
                }
            }

            // Biweight location (iterative, matches robust_stats.cpp)
            constexpr float c_bw = 6.0f;
            constexpr int max_iter = 10;
            constexpr float tol = 1e-7f;

            float sorted_win[225];
            for (int i = 0; i < wn; i++) sorted_win[i] = window[i];
            insertion_sort(sorted_win, wn);
            float location = sorted_median(sorted_win, wn);

            // MAD of window
            float abs_devs_win[225];
            for (int i = 0; i < wn; i++)
                abs_devs_win[i] = std::fabs(window[i] - location);
            insertion_sort(abs_devs_win, wn);
            float mad_val = sorted_median(abs_devs_win, wn);

            if (mad_val > 1e-30f) {
                float scale = mad_val * 1.4826f;

                for (int iter = 0; iter < max_iter; iter++) {
                    double num = 0.0, den = 0.0;
                    for (int i = 0; i < wn; i++) {
                        float u = (window[i] - location) / (c_bw * scale);
                        if (std::fabs(u) < 1.0f) {
                            float u2 = u * u;
                            float w = (1.0f - u2) * (1.0f - u2);
                            num += w * window[i];
                            den += w;
                        }
                    }
                    if (den < 1e-30) break;
                    float new_loc = static_cast<float>(num / den);
                    if (std::fabs(new_loc - location) < tol * scale) {
                        location = new_loc;
                        break;
                    }
                    location = new_loc;
                }
            }

            local_background[pi] = location;

            // ── Local RMS from neighbour differences ──
            //
            // MAD about the local median reports smooth STRUCTURE as noise: on
            // a noiseless ramp of slope s it returns 5.93*s, and on a real sky
            // gradient it inflates the noise by tens of percent. Differencing
            // horizontally adjacent pixels cancels anything smooth and leaves
            // only what changes from one pixel to the next, which is what
            // "how noisy is this stack here" actually means.
            //
            // The window is gathered row-major, so entries adjacent within a
            // row are adjacent on the image; row boundaries are skipped.
            const int win_w = x1 - x0 + 1;
            const int win_h = y1 - y0 + 1;
            float diffs[225];
            int dn = 0;
            for (int ry = 0; ry < win_h; ry++)
                for (int rx = 0; rx + 1 < win_w; rx++)
                    diffs[dn++] = window[ry * win_w + rx + 1] - window[ry * win_w + rx];

            float rms_val = 0.0f;
            if (dn > 1) {
                float sorted_d[225];
                for (int i = 0; i < dn; i++) sorted_d[i] = diffs[i];
                insertion_sort(sorted_d, dn);
                const float med_d = sorted_median(sorted_d, dn);

                float abs_dev_d[225];
                for (int i = 0; i < dn; i++) abs_dev_d[i] = std::fabs(diffs[i] - med_d);
                insertion_sort(abs_dev_d, dn);

                // 1.4826 converts MAD to a Gaussian sigma; 1/sqrt(2) undoes the
                // differencing of two independent samples.
                rms_val = sorted_median(abs_dev_d, dn) * 1.4826f * 0.70710678f;
            }
            local_rms[pi] = rms_val;
        }
    }
}

} // namespace nukex
