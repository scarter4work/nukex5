#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/stacker/cache_sig.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace nukex {

void ShadowBuffers::allocate(int bs, int nc, int mf) {
    batch_size = bs;
    n_channels = nc;
    max_frames = mf;

    int B = bs, C = nc, N = mf;

    // Input buffers
    welford_mean.resize(C * B, 0.0f);
    welford_M2.resize(C * B, 0.0f);
    welford_n.resize(C * B, 0);
    pixel_values.resize(C * N * B, 0.0f);
    n_frames.resize(C * B, 0);
    // Deliberately NOT sized here. extract_from_cube builds it from the slot
    // refs; leaving it empty is the signal that the kernels are being driven
    // directly (unit tests), where a local slot index IS the global frame.

    // Intermediate
    pixel_weights.resize(C * N * B, 0.0f);
    // Left EMPTY on purpose, like global_frame_of: empty means "no coverage
    // information, everything is a real sample", which is what a unit test
    // driving the kernels directly means. extract_from_cube sizes it.

    // Classification output
    cloud_frame_count.resize(B, 0);
    trail_frame_count.resize(B, 0);
    worst_sigma_score.resize(B, 0.0f);
    best_sigma_score.resize(B, 0.0f);
    mean_weight_out.resize(B, 0.0f);
    total_exposure_out.resize(B, 0.0f);

    // Robust stats output
    mad_out.resize(C * B, 0.0f);
    biweight_midvar_out.resize(C * B, 0.0f);
    iqr_out.resize(C * B, 0.0f);

    // Distribution input
    dist_true_signal.resize(C * B, 0.0f);
    dist_uncertainty.resize(C * B, 0.0f);
    dist_confidence.resize(C * B, 0.0f);

    // Selection output
    output_value.resize(C * B, 0.0f);
    noise_sigma.resize(C * B, 0.0f);
    snr_out.resize(C * B, 0.0f);
}

void ShadowBuffers::extract_from_cube(
    const Cube& cube,
    const std::vector<ChannelCacheRef>& slot_refs,
    int start_voxel, int count, int nc) {

    int B = count;
    int C = nc;
    int N = max_frames;
    int w = cube.width;

    // Built here rather than by the caller so there is no ordering hazard:
    // the kernels cannot see pixel_values without also seeing the map that
    // says which global frame each local slot came from.
    map_frames(slot_refs, C);
    pixel_valid.assign((static_cast<std::size_t>(C) * N * B + 7) / 8, 0);

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        const auto& voxel = cube.at(px, py);

        for (int ch = 0; ch < C; ch++) {
            welford_mean[ch * B + vi] = voxel.channel(ch).welford.mean;
            welford_M2[ch * B + vi]   = voxel.channel(ch).welford.M2;
            welford_n[ch * B + vi]    = voxel.channel(ch).welford.n;

            // Route per-frame pixel values through the slot ref.
            // ref.cache == nullptr means no per-frame source for this slot
            // (e.g. an unmapped synthesised slot in a degenerate config).
            // pixel_values was zeroed by allocate() so no fill needed here —
            // just skip; distribution fitting will use welford-only stats.
            if (ch >= static_cast<int>(slot_refs.size())) continue;
            const ChannelCacheRef& ref = slot_refs[ch];
            if (ref.cache == nullptr) continue;

            float frame_vals[GPU_MAX_FRAMES];
            int nf_read = 0;

            std::uint8_t frame_ok[GPU_MAX_FRAMES] = {0};

            if (ref.kind == SlotSynthesis::DIRECT) {
                nf_read = ref.cache->read_pixel(px, py, ref.cache_ch,
                                                frame_vals, frame_ok);

            } else if (ref.kind == SlotSynthesis::REC709_LUMA) {
                // Synthesise L per-frame from cached R, G, B channels.
                // Matches Phase A's per-pixel: l = 0.299R + 0.587G + 0.114B.
                float r_vals[GPU_MAX_FRAMES], g_vals[GPU_MAX_FRAMES], b_vals[GPU_MAX_FRAMES];
                std::uint8_t r_ok[GPU_MAX_FRAMES], g_ok[GPU_MAX_FRAMES], b_ok[GPU_MAX_FRAMES];
                int n_r = ref.cache->read_pixel(px, py, 0, r_vals, r_ok);
                int n_g = ref.cache->read_pixel(px, py, 1, g_vals, g_ok);
                int n_b = ref.cache->read_pixel(px, py, 2, b_vals, b_ok);
                nf_read = std::min({n_r, n_g, n_b});
                for (int fi = 0; fi < nf_read; ++fi) {
                    frame_vals[fi] = 0.299f * r_vals[fi]
                                   + 0.587f * g_vals[fi]
                                   + 0.114f * b_vals[fi];
                    // Synthetic luminance mixes all three planes, so it is a
                    // measurement only where all three are.
                    frame_ok[fi] = (r_ok[fi] && g_ok[fi] && b_ok[fi]) ? 1 : 0;
                }
            }

            // The channel's own frame count, not the voxel's. They differ
            // whenever two slots read different caches.
            int n_copy = std::min(nf_read, N);
            for (int fi = 0; fi < n_copy; fi++) {
                pixel_values[ch * N * B + fi * B + vi] = frame_vals[fi];
                set_sample_valid(ch, fi, vi, frame_ok[fi] != 0, B);
            }
            n_frames[ch * B + vi] = static_cast<uint16_t>(n_copy);
        }
    }
}

void ShadowBuffers::map_frames(const std::vector<ChannelCacheRef>& slot_refs,
                               int nc) {
    const int C = nc, N = max_frames;
    global_frame_of.assign(C * N, -1);
    for (int ch = 0; ch < C && ch < static_cast<int>(slot_refs.size()); ch++) {
        const ChannelCacheRef& ref = slot_refs[ch];
        if (ref.cache == nullptr) continue;
        const int n = std::min(ref.cache->n_frames_written(), N);
        for (int fi = 0; fi < n; fi++)
            global_frame_of[ch * N + fi] = ref.cache->global_frame(fi);
    }
}

void ShadowBuffers::writeback_classification(
    Cube& cube, int start_voxel, int count, int nc) const {

    int B = count;
    int C = nc;
    int w = cube.width;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        auto& voxel = cube.at(px, py);

        voxel.cloud_frame_count = cloud_frame_count[vi];
        voxel.trail_frame_count = trail_frame_count[vi];
        voxel.worst_sigma_score = worst_sigma_score[vi];
        voxel.best_sigma_score  = best_sigma_score[vi];
        voxel.mean_weight       = mean_weight_out[vi];
        voxel.total_exposure    = total_exposure_out[vi];

        for (int ch = 0; ch < C; ch++) {
            voxel.channel(ch).mad                  = mad_out[ch * B + vi];
            voxel.channel(ch).biweight_midvariance = biweight_midvar_out[ch * B + vi];
            voxel.channel(ch).iqr                  = iqr_out[ch * B + vi];
        }
    }
}

void ShadowBuffers::extract_distributions(
    const Cube& cube, int start_voxel, int count, int nc) {

    int B = count;
    int C = nc;
    int w = cube.width;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        const auto& voxel = cube.at(px, py);

        for (int ch = 0; ch < C; ch++) {
            dist_true_signal[ch * B + vi] = voxel.channel(ch).distribution.true_signal_estimate;
            dist_uncertainty[ch * B + vi] = voxel.channel(ch).distribution.signal_uncertainty;
            dist_confidence[ch * B + vi]  = voxel.channel(ch).distribution.confidence;
        }
    }
}

void ShadowBuffers::writeback_selection(
    Cube& cube, int start_voxel, int count, int nc,
    float* output_image, float* noise_image) const {

    int B = count;
    int C = nc;
    int w = cube.width;
    int h = cube.height;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        auto& voxel = cube.at(px, py);

        for (int ch = 0; ch < C; ch++) {
            float val = output_value[ch * B + vi];
            float noise = noise_sigma[ch * B + vi];
            float snr = snr_out[ch * B + vi];

            voxel.channel(ch).snr = snr;

            // Write to output images (channel-by-channel, row-major)
            if (output_image)
                output_image[ch * w * h + py * w + px] = val;
            if (noise_image)
                noise_image[ch * w * h + py * w + px] = noise;
        }
    }
}

} // namespace nukex
