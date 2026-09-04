#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/alignment/channel_registration.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// High-level frame alignment: detect → match → homography → warp.
///
/// The reference is normally chosen by the caller via set_reference() from a
/// measurement pass over every frame (see reference_selector.hpp). If none is
/// set, the first frame to arrive becomes the reference — the historical
/// behaviour, kept as a fallback for callers with a single frame or no
/// measurements. Subsequent frames are aligned to the reference catalog.
/// Meridian flips are detected and corrected automatically. Failed alignments
/// keep weight × 0.5.
class FrameAligner {
public:
    struct Config {
        StarDetector::Config star_config;
        StarMatcher::Config  match_config;
        HomographyComputer::Config homography_config;
        ChannelRegistrationConfig channel_config;

        /// Register the colour channels to each other within each frame.
        ///
        /// On by default and with no user-facing threshold, deliberately:
        /// almost nobody knows they have lateral chromatic aberration, so an
        /// opt-in would not reach the people it helps. The cost is bounded by
        /// the near-identity skip -- a rig with no colour error pays nothing.
        /// The flag exists so tests can isolate the old behaviour.
        bool register_channels = true;
    };

    FrameAligner() = default;
    explicit FrameAligner(const Config& config);

    /// Align a frame to the reference. Returns the aligned image and alignment result.
    /// If this is the first frame, it becomes the reference (H = identity).
    ///
    /// The input image may be mono or colour. Star detection runs on the
    /// green channel for a colour frame (see default_reference_channel()).
    struct AlignedFrame {
        Image image;              // warped image, aligned to reference
        AlignmentResult alignment;
        StarCatalog stars;        // detected stars (pre-alignment coordinates)

        /// Per-channel transforms measured on this frame, before warping.
        /// Empty for a mono frame, when registration is off, or when nothing
        /// could be measured.
        ChannelTransforms channels;

        int frame_index;
    };

    AlignedFrame align(const Image& frame, int frame_index);

    /// Install `frame` as the alignment reference before any align() call.
    ///
    /// `frame_index` is the index align() will later see for this same frame;
    /// that call short-circuits to the identity rather than matching the frame
    /// against its own catalog.
    void set_reference(const Image& frame, int frame_index);

    /// Get the reference catalog (for inspection/debugging).
    const StarCatalog& reference_catalog() const { return ref_catalog_; }

    /// Has a reference frame been set?
    bool has_reference() const { return has_ref_; }

    /// Reset the aligner (clear reference).
    void reset();

private:
    /// Adopt `frame` as the reference. Shared by set_reference() and the
    /// first-frame fallback inside align().
    void adopt_reference(const Image& frame, const StarCatalog& stars,
                         int frame_index);

    Config config_;
    StarCatalog ref_catalog_;
    bool has_ref_ = false;
    int ref_width_ = 0;
    int ref_height_ = 0;
    int ref_index_ = -1;
};

} // namespace nukex
