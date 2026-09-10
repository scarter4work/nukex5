#pragma once
#include "nukex/io/image.hpp"
#include "nukex/core/cube.hpp"

namespace nukex {
class OutputAssembler {
public:
    struct OutputImages {
        Image stacked;
        Image noise_map;
        Image quality_map;
    };
    static Image assemble_quality_map(const Cube& cube);

    /// The stack's REALISED pixel-to-pixel scatter, one channel, from
    /// SubcubeVoxel::local_rms. Distinct from noise_map, which is the
    /// PREDICTED uncertainty of each estimate; the two answer different
    /// questions and only the predicted one existed before.
    static Image assemble_measured_noise(const Cube& cube);

    /// The three numbers the console prints, from ONE computation so they
    /// reconcile by construction: `ratio` IS measured_median / predicted_median.
    /// (A median of per-pixel ratios is not the ratio of medians; printing one
    /// beside the other two made the line contradict itself by 2-3%.)
    ///
    /// The measured map is the spatial kernel's scatter on its luminance
    /// window, so a 3-channel predicted map is combined the same way,
    /// sqrt((0.2126 s0)^2 + (0.7152 s1)^2 + (0.0722 s2)^2) on channels 0..2,
    /// before its median is taken. A ratio above 1 means the stack is noisier
    /// than its own model says -- what an estimator injecting noise looks like
    /// from outside.
    struct NoiseCheck {
        double measured_median  = 0.0;
        double predicted_median = 0.0;
        double ratio            = 0.0;
        bool   comparable       = false;   ///< false when either median is 0
    };
    static NoiseCheck noise_check(const Image& measured, const Image& predicted);
};
} // namespace nukex
