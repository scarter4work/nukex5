#include "nukex/calibration/frame_normalization.hpp"

#include <algorithm>
#include <vector>

namespace nukex {

namespace {

/// Lower median of a non-empty vector, by partial sort. Used rather than a
/// mean so that one fogged or blown frame cannot drag the reference and
/// rescale the whole batch to match it.
double median_of(std::vector<double>& v) {
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

} // namespace

std::vector<NormalizationCoefficients>
solve_frame_normalization(const std::vector<FrameSky>& frames) {
    std::vector<NormalizationCoefficients> out(frames.size());

    std::vector<double> locs, scales;
    locs.reserve(frames.size());
    scales.reserve(frames.size());
    for (const auto& f : frames)
        if (f.usable && f.scale > 0.0) {
            locs.push_back(f.location);
            scales.push_back(f.scale);
        }

    // Nothing measurable: every frame keeps the identity. Normalising against
    // a reference we could not measure would be worse than not normalising.
    if (locs.empty()) return out;

    const double ref_loc   = median_of(locs);
    const double ref_scale = median_of(scales);
    if (!(ref_scale > 0.0)) return out;

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        if (!f.usable || !(f.scale > 0.0)) continue;   // identity
        // v' = (v - loc_f) * (s_ref / s_f) + loc_ref
        const double a = ref_scale / f.scale;
        out[i].scale  = a;
        out[i].offset = ref_loc - f.location * a;
    }
    return out;
}

} // namespace nukex
