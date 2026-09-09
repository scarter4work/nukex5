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

double OutputAssembler::measured_vs_predicted_ratio(const Image& measured,
                                                    const Image& predicted) {
    if (measured.empty() || predicted.empty()) return 0.0;
    if (measured.width() != predicted.width() ||
        measured.height() != predicted.height()) return 0.0;

    // The spatial kernel measures noise on a LUMINANCE window, so a colour
    // prediction has to be combined the same way before the two are
    // comparable. Channels are treated as independent, which is what the
    // per-channel fits assume.
    const int nc = predicted.n_channels();
    std::vector<double> ratios;
    ratios.reserve(static_cast<std::size_t>(measured.width()) * measured.height());

    for (int y = 0; y < measured.height(); y++) {
        for (int x = 0; x < measured.width(); x++) {
            const float m = measured.at(x, y, 0);
            if (!(m > 0.0f) || !std::isfinite(m)) continue;

            double p;
            if (nc >= 3) {
                const double wr = 0.2126 * predicted.at(x, y, 0);
                const double wg = 0.7152 * predicted.at(x, y, 1);
                const double wb = 0.0722 * predicted.at(x, y, 2);
                p = std::sqrt(wr * wr + wg * wg + wb * wb);
            } else {
                p = predicted.at(x, y, 0);
            }
            if (!(p > 0.0) || !std::isfinite(p)) continue;
            ratios.push_back(static_cast<double>(m) / p);
        }
    }
    if (ratios.empty()) return 0.0;

    const std::size_t mid = ratios.size() / 2;
    std::nth_element(ratios.begin(), ratios.begin() + mid, ratios.end());
    double med = ratios[mid];
    if (ratios.size() % 2 == 0) {
        double lo = *std::max_element(ratios.begin(), ratios.begin() + mid);
        med = 0.5 * (lo + med);
    }
    return med;
}

} // namespace nukex
