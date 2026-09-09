#pragma once

#include "nukex/io/image.hpp"

namespace nukex {

/// A planar sky model fitted to an image's BACKGROUND.
///
/// `dx` and `dy` are the change across the full frame width and height, in the
/// image's own units, so a model is directly comparable to the frame's noise.
struct GradientModel {
    double level = 0.0;     ///< fitted background at frame centre
    double dx    = 0.0;     ///< change from left edge to right edge
    double dy    = 0.0;     ///< change from top edge to bottom edge
    bool   valid = false;   ///< false when the image is empty or degenerate
};

/// Fixed-pattern sky gradient removal for the stacked output.
///
/// Per-frame normalisation cannot address this. Measured on the user's
/// 65-frame NGC7635 set, the per-frame TILT is identical across frames to
/// within 0.05-0.10 of one frame's noise, while the per-frame LEVEL varies by
/// 2.3x that. The level is what normalisation fixes; the tilt is a fixed
/// pattern -- optics, flats, or a static light-pollution direction -- and
/// stacking preserves it. In the finished stack it spans roughly 6 sigma from
/// one corner to the other.
///
/// Only a PLANE is fitted, deliberately. A higher-order surface can follow the
/// outskirts of a large nebula and subtract it, which is the classic way an
/// automatic background extraction quietly destroys signal. A plane cannot
/// curve into an object.
class BackgroundGradient {
public:
    /// Robustly fit a plane to `channel`'s background.
    ///
    /// Astronomical objects are POSITIVE excursions, so the clipping is
    /// asymmetric: positive residuals are cut hard and negative ones loosely,
    /// which keeps stars and nebulosity from tilting the plane toward
    /// themselves.
    static GradientModel fit_planar(const Image& img, int channel);

    /// Subtract the model's TILT from `channel`, leaving the sky level where
    /// it was. The level feeds the auto-stretch's sky target, and moving it is
    /// a separate decision from flattening.
    static void subtract(Image& img, int channel, const GradientModel& model);

    /// Peak-to-peak amplitude of the model across the frame's diagonal.
    static double amplitude(const GradientModel& model);
};

} // namespace nukex
