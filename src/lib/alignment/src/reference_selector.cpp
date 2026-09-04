#include "nukex/alignment/reference_selector.hpp"

namespace nukex {

int select_reference_frame(const std::vector<FrameQuality>& frames) {
    if (frames.empty()) {
        return -1;
    }

    int best_count = -1;
    for (const auto& f : frames) {
        if (f.usable && f.star_count > best_count) {
            best_count = f.star_count;
        }
    }
    if (best_count < 0) {
        return 0;   // nothing usable — keep the historical first-frame choice
    }

    const float threshold =
        static_cast<float>(best_count) * kReferenceCandidateBand;

    int   chosen      = -1;
    float chosen_fwhm = 0.0f;
    for (std::size_t i = 0; i < frames.size(); i++) {
        const auto& f = frames[i];
        if (!f.usable || static_cast<float>(f.star_count) < threshold) {
            continue;
        }
        // An unmeasurable FWHM must not read as infinitely sharp, so it sorts
        // behind every measured one instead of ahead of them.
        const bool measured        = f.median_fwhm > 0.0f;
        const bool chosen_measured = chosen >= 0 && chosen_fwhm > 0.0f;

        bool better;
        if (chosen < 0) {
            better = true;
        } else if (measured != chosen_measured) {
            better = measured;
        } else if (!measured) {
            better = false;   // both unmeasurable — keep the lower index
        } else {
            better = f.median_fwhm < chosen_fwhm;
        }

        if (better) {
            chosen      = static_cast<int>(i);
            chosen_fwhm = f.median_fwhm;
        }
    }

    return chosen >= 0 ? chosen : 0;
}

} // namespace nukex
