#include "nukex/alignment/frame_aligner.hpp"

#include <cmath>

namespace nukex {

namespace {
/// A channel correction smaller than this at the field corner is not worth a
/// resample: it is below the centroid noise floor measured on real data
/// (0.058 px between blue and green on the M3 set) by a wide margin.
constexpr double kNegligibleChannelShiftPx = 0.01;
}

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

    // Measure the channel disagreement on the UNWARPED frame, using the stars
    // we already have. Lateral colour is a property of this exposure through
    // this optic at this altitude; measuring it after warping would mix it
    // with the frame-to-frame transform.
    if (config_.register_channels && frame.n_channels() >= 2
        && !result.stars.empty()) {
        result.channels = measure_channel_transforms(
            frame, result.stars,
            default_reference_channel(frame.n_channels()),
            config_.channel_config);
    }

    // The near-identity skip. A well-corrected rig should not pay for
    // interpolation it does not need, and this is what makes "always on"
    // affordable without exposing a threshold for users to argue about.
    // The radius is the frame's own corner, where the scale term is largest.
    const double corner_radius =
        std::hypot(frame.width() / 2.0, frame.height() / 2.0);
    const bool channels_matter =
        !result.channels.empty()
        && !result.channels.negligible(corner_radius,
                                       kNegligibleChannelShiftPx);
    if (!channels_matter) result.channels = ChannelTransforms{};

    if (frame_index == ref_index_) {
        // This IS the reference. Matching it against its own catalog would
        // only reintroduce fit noise into a transform that is exactly the
        // identity by construction.
        //
        // Its CHANNELS are a different matter. Every other frame registers to
        // this one, so if its own channels disagree the whole stack inherits
        // that. Warp it with the identity homography when there is a channel
        // correction to make, and clone it when there is not.
        result.alignment.H = HomographyMatrix::identity();
        result.alignment.match.success = true;
        result.alignment.match.n_inliers = result.stars.size();
        result.image = channels_matter
            ? HomographyComputer::warp(frame, result.alignment.H,
                                       frame.width(), frame.height(),
                                       result.channels)
            : frame.clone();
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

    if (!result.alignment.alignment_failed) {
        result.image = HomographyComputer::warp(
            frame, result.alignment.H, ref_width_, ref_height_,
            result.channels);
    } else if (channels_matter) {
        // Failed alignment: the frame is still stacked, with its weight
        // penalised, so its channels still have to be put right. The
        // homography is the identity because there isn't a usable one.
        result.image = HomographyComputer::warp(
            frame, HomographyMatrix::identity(),
            frame.width(), frame.height(), result.channels);
    } else {
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
