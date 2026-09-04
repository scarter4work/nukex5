#pragma once

#include <vector>

namespace nukex {

/// What the reference pre-pass measures about one frame.
struct FrameQuality {
    int   star_count  = 0;      ///< stars the detector kept (capped at max_stars)
    float median_fwhm = 0.0f;   ///< pixels; <= 0 when it could not be measured
    bool  usable      = true;   ///< false for blown-out / rejected frames
};

/// Fraction of the best star count a frame must reach to be considered as the
/// alignment reference. A frame below this cannot supply enough triangles for
/// the Groth matcher no matter how sharp it is.
constexpr float kReferenceCandidateBand = 0.75f;

/// Index of the frame that should become the alignment reference.
///
/// The reference used to be whichever frame the directory listing put first.
/// On an LRGB-mono set the per-filter star yield varies enormously, so that
/// was a coin flip: on M27 2025 it landed on a blue frame with 32 stars, the
/// matcher could not find a consistent triangle against the 200-star frames
/// that followed, and 71 of 72 frames aligned with zero inliers.
///
/// Selection is by measured quality instead. Every usable frame within
/// kReferenceCandidateBand of the best star count is a candidate, and the
/// sharpest candidate wins; detection caps at max_stars, so most frames of a
/// good night tie on count and FWHM is what actually separates them. Ties
/// resolve to the lowest index, because the E2E goldens require the choice to
/// be reproducible.
///
/// Returns -1 for an empty list, and 0 when no frame is usable — degrading to
/// the historical first-frame behaviour rather than failing the stack.
int select_reference_frame(const std::vector<FrameQuality>& frames);

} // namespace nukex
