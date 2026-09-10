// NukeX v5 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
#pragma once

#include "nukex/compose/color_composer.hpp"
#include "nukex/io/image.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace nukex {

/// Compose the Phase B semantic slot planes into one 3-channel image.
///
/// This is the colour-science output: hue comes from the RATIO of the slots,
/// not from the raw per-channel accumulation the stacker also produces. The
/// difference is not cosmetic -- on a 156-frame OSC stack of M63 the raw
/// channels measured R 1.000 G 1.000 B 0.955 (saturation 0.080) while the
/// same target integrated in PixInsight measured 1.000 / 0.736 / 0.660 at
/// 0.337.
///
/// `compose_pixel` is an identity on the grey axis at every level (measured
/// at 0.5, 0.1, 0.05, 0.03, 0.027, 0.01 and 0.003 -- ratio 1.0000 for all of
/// them), so the result sits on the SAME linear scale as the slots and can be
/// handed to a stretch directly. There is no transfer function to undo.
///
/// `composer` is taken by reference because the caller configures its chroma
/// gate first, and because it accumulates the gamut-clip counter this run
/// reports.
///
/// Returns an empty Image when there is nothing to compose.
/// True when the slots carry chrominance -- anything beyond a lone L.
///
/// An L-only mono stack composes to a grey RGB triplet, which is right for
/// the composed DISPLAY but wrong as a stretch input: it would widen every
/// mono result from one channel to three, tripling the memory to say exactly
/// the same thing.
bool slots_have_colour(
    const std::unordered_map<std::string, std::vector<float>>& slots);

Image compose_slots_to_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer);

/// One-channel image of the composer's own luminance per pixel
/// (ColorComposer::luminance_of): native L, else rec709 of RGB, else the
/// emission total. This is what an emission-line stack hands to the stretch.
Image compose_luminance_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots);

/// Compose with an EXTERNAL luminance. Hue and chroma come from the slots
/// exactly as in compose_slots_to_image -- linear line ratios through the
/// chroma gate -- but L* is taken from `luminance` (one channel, same size),
/// and the gamut walk happens once, at that L*.
///
/// With `luminance` == compose_luminance_image(slots) this is
/// compose_slots_to_image to the float rounding of the luminance plane
/// (differences in the 7th decimal). With a STRETCHED luminance it is the
/// cure for the dual-narrowband colour loss: the palette that had to be
/// walked to grey at L* 3 fits as composed at L* 50.
///
/// Returns an empty Image when `luminance` is not a one-channel image of the
/// given size.
Image compose_slots_with_luminance(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer,
    const Image& luminance);

} // namespace nukex
