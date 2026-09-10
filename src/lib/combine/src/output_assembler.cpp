#include "nukex/combine/output_assembler.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace nukex {

Image OutputAssembler::assemble_quality_map(const Cube& cube) {
    Image quality(cube.width, cube.height, 4);
    int n_ch = cube.channel_config.n_channels;

    for (int y = 0; y < cube.height; y++) {
        for (int x = 0; x < cube.width; x++) {
            const auto& v = cube.at(x, y);

            float avg_signal = 0.0f;
            float max_uncertainty = 0.0f;
            float min_confidence = 1.0f;

            for (int ch = 0; ch < n_ch; ch++) {
                const auto& dist = v.channel(ch).distribution;
                avg_signal += dist.true_signal_estimate;
                max_uncertainty = std::max(max_uncertainty, dist.signal_uncertainty);
                min_confidence = std::min(min_confidence, dist.confidence);
            }
            avg_signal /= n_ch;

            quality.at(x, y, 0) = avg_signal;
            quality.at(x, y, 1) = max_uncertainty;
            quality.at(x, y, 2) = min_confidence;
            quality.at(x, y, 3) = static_cast<float>(
                static_cast<uint8_t>(v.dominant_shape));
        }
    }

    return quality;
}

Image OutputAssembler::assemble_measured_noise(const Cube& cube) {
    Image measured(cube.width, cube.height, 1);
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++)
            measured.at(x, y, 0) = cube.at(x, y).local_rms;
    return measured;
}

OutputAssembler::NoiseCheck OutputAssembler::noise_check(const Image& measured,
                                                          const Image& predicted,
                                                          LuminanceSpec luminance) {
    NoiseCheck out;
    if (measured.empty() || predicted.empty()) return out;
    const LuminanceSpec lum = luminance.resolved(predicted.n_channels());
    if (lum.c0 >= predicted.n_channels() ||
        (lum.mode == LuminanceSpec::REC709 &&
         (lum.c1 >= predicted.n_channels() || lum.c2 >= predicted.n_channels()))) return out;

    auto median_positive = [](std::vector<float>& v) -> double {
        if (v.empty()) return 0.0;
        const std::size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + mid, v.end());
        return v[mid];
    };

    std::vector<float> mv;
    mv.reserve(static_cast<std::size_t>(measured.width()) * measured.height());
    for (int y = 0; y < measured.height(); y++)
        for (int x = 0; x < measured.width(); x++) {
            const float m = measured.at(x, y, 0);
            if (m > 0.0f && std::isfinite(m)) mv.push_back(m);
        }

    std::vector<float> pv;
    pv.reserve(static_cast<std::size_t>(predicted.width()) * predicted.height());
    for (int y = 0; y < predicted.height(); y++)
        for (int x = 0; x < predicted.width(); x++) {
            float p;
            if (lum.mode == LuminanceSpec::REC709) {
                // The kernel measures on a rec709 window of these planes; the
                // prediction combines the same planes in quadrature.
                const float wr = 0.2126f * predicted.at(x, y, lum.c0);
                const float wg = 0.7152f * predicted.at(x, y, lum.c1);
                const float wb = 0.0722f * predicted.at(x, y, lum.c2);
                p = std::sqrt(wr * wr + wg * wg + wb * wb);
            } else {
                p = predicted.at(x, y, lum.c0);
            }
            if (p > 0.0f && std::isfinite(p)) pv.push_back(p);
        }

    out.measured_median  = median_positive(mv);
    out.predicted_median = median_positive(pv);
    out.comparable = out.measured_median > 0.0 && out.predicted_median > 0.0;
    out.ratio = out.comparable ? out.measured_median / out.predicted_median : 0.0;
    return out;
}

} // namespace nukex
