// NukeX v5 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
#pragma once

#include <vector>

namespace nukex {

/// One frame's sky, as the measuring pass reports it.
struct FrameSky {
    double location = 0.0;   ///< robust background level (median)
    double scale    = 0.0;   ///< robust spread (MAD x 1.4826)
    bool   usable   = true;
};

/// How to bring one frame onto the common scale: v' = scale*v + offset.
struct NormalizationCoefficients {
    double scale  = 1.0;
    double offset = 0.0;
};

/// Solve additive + multiplicative normalisation for a batch of frames.
///
/// Sky level and transparency vary across a session, and NukeX currently does
/// nothing about it: EXPTIME is read, stored and summed for bookkeeping, and
/// never used to scale anything. The per-voxel fit then meets a bimodal
/// population and treats the minority as outliers.
///
/// Measured on the user's own data, both halves of that:
///   * a 156-frame M63 batch mixes 120 s and 300 s frames -- sky 2685 vs 6813
///     ADU, a ratio of 2.54 against the 2.50 exposure ratio at identical gain;
///   * within the 114-frame subset, per-frame sky offsets still run -0.8 to
///     +3.2 sigma, and upper-mixture-component membership is 60x more
///     frame-structured than chance.
///
/// The reference is the MEDIAN location and scale over usable frames, not a
/// chosen frame: one fogged or blown exposure must not rescale the batch.
///
/// A frame with no measurable scale, or one marked unusable, gets the identity
/// -- normalising on a scale of zero would be division by noise.
std::vector<NormalizationCoefficients>
solve_frame_normalization(const std::vector<FrameSky>& frames);

} // namespace nukex
