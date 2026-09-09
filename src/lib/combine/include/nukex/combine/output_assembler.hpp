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

    /// Median measured/predicted noise over pixels where both are finite and
    /// positive. A value above 1 means the stack is noisier than its own model
    /// says it should be -- which is what an estimator injecting noise looks
    /// like from outside. Returns 0 when nothing is comparable.
    static double measured_vs_predicted_ratio(const Image& measured,
                                              const Image& predicted);

    /// Median PREDICTED noise expressed as the luminance equivalent, so it is
    /// directly comparable with the measured map and with the ratio above.
    /// Channel 0's median is NOT interchangeable with this on a colour stack.
    static double predicted_luminance_median(const Image& predicted);
};
} // namespace nukex
