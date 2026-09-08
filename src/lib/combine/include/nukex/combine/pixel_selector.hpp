#pragma once

#include "nukex/core/distribution.hpp"
#include "nukex/core/frame_stats.hpp"

namespace nukex {

class PixelSelector {
public:
    void select(const ZDistribution& dist,
                const float* values, const float* weights, int n,
                const FrameStats* frame_stats, const int* frame_indices,
                int slot, float welford_var,
                float& out_value, float& out_noise, float& out_snr) const;

    /// Variance of one sample, in normalized^2 units.
    ///
    /// `slot` is the cube slot the sample belongs to -- the same index the
    /// GPU kernels call `ch`. It selects the normalisation this frame had
    /// applied in Phase A, which must be undone before the Poisson term,
    /// since that term is only meaningful on raw ADU.
    static float sample_variance(float value, const FrameStats& fs, int slot,
                                 float welford_var);
};

} // namespace nukex
