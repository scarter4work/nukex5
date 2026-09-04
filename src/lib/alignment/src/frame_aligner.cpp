#include "nukex/alignment/frame_aligner.hpp"

namespace nukex {

FrameAligner::FrameAligner(const Config& config) : config_(config) {}

FrameAligner::AlignedFrame FrameAligner::align(const Image& frame, int frame_index) {
    AlignedFrame result;
    result.frame_index = frame_index;

    // Detect stars
    result.stars = StarDetector::detect(frame, config_.star_config);

    if (!has_ref_) {
        // No reference was set: fall back to the first frame to arrive.
        adopt_reference(frame, result.stars, frame_index);
    }

    if (frame_index == ref_index_) {
        // This IS the reference. Matching it against its own catalog would
        // only reintroduce fit noise into a transform that is exactly the
        // identity by construction.
        result.image = frame.clone();
        result.alignment.H = HomographyMatrix::identity();
        result.alignment.match.success = true;
        result.alignment.match.n_inliers = result.stars.size();
        return result;
    }

    // Match stars to reference using triangle similarity matching.
    auto matches = StarMatcher::match(result.stars, ref_catalog_,
                                       config_.match_config);

    // Compute homography
    result.alignment = HomographyComputer::compute(
        result.stars, ref_catalog_, matches, config_.homography_config);

    // Handle meridian flip
    if (result.alignment.is_meridian_flipped && !result.alignment.alignment_failed) {
        result.alignment.H = HomographyComputer::correct_meridian_flip(
            result.alignment.H, ref_width_, ref_height_);
    }

    // Warp image to reference frame
    if (!result.alignment.alignment_failed) {
        result.image = HomographyComputer::warp(
            frame, result.alignment.H, ref_width_, ref_height_);
    } else {
        // Failed alignment: return unwarped frame, weight penalized
        result.image = frame.clone();
    }

    return result;
}

void FrameAligner::set_reference(const Image& frame, int frame_index) {
    adopt_reference(frame, StarDetector::detect(frame, config_.star_config),
                    frame_index);
}

void FrameAligner::adopt_reference(const Image& frame, const StarCatalog& stars,
                                   int frame_index) {
    ref_catalog_ = stars;
    ref_width_   = frame.width();
    ref_height_  = frame.height();
    ref_index_   = frame_index;
    has_ref_     = true;
}

void FrameAligner::reset() {
    ref_catalog_ = StarCatalog{};
    has_ref_ = false;
    ref_width_ = 0;
    ref_height_ = 0;
    ref_index_ = -1;
}

} // namespace nukex
