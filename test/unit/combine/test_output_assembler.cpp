#include "catch_amalgamated.hpp"
#include "nukex/combine/output_assembler.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"

using namespace nukex;

static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}

TEST_CASE("OutputAssembler: quality map has 4 channels", "[assembler]") {
    auto config = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(8, 8, config);

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            auto& v = cube.at(x, y);
            v.channel(0).distribution.shape = DistributionShape::GAUSSIAN;
            v.channel(0).distribution.true_signal_estimate = 0.5f;
            v.channel(0).distribution.signal_uncertainty = 0.01f;
            v.channel(0).distribution.confidence = 0.9f;
            v.dominant_shape = DistributionShape::GAUSSIAN;
        }

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.width() == 8);
    REQUIRE(quality.height() == 8);
    REQUIRE(quality.n_channels() == 4);
    REQUIRE(quality.at(4, 4, 0) == Catch::Approx(0.5f));
    REQUIRE(quality.at(4, 4, 1) == Catch::Approx(0.01f));
    REQUIRE(quality.at(4, 4, 2) == Catch::Approx(0.9f));
    REQUIRE(quality.at(4, 4, 3) == Catch::Approx(0.0f));
}

TEST_CASE("OutputAssembler: shape channel encodes dominant_shape correctly", "[assembler]") {
    auto config = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(4, 4, config);

    cube.at(0, 0).dominant_shape = DistributionShape::GAUSSIAN;
    cube.at(1, 0).dominant_shape = DistributionShape::BIMODAL;
    cube.at(2, 0).dominant_shape = DistributionShape::HEAVY_TAILED;
    cube.at(3, 0).dominant_shape = DistributionShape::CONTAMINATED;

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.at(0, 0, 3) == Catch::Approx(0.0f));
    REQUIRE(quality.at(1, 0, 3) == Catch::Approx(1.0f));
    REQUIRE(quality.at(2, 0, 3) == Catch::Approx(2.0f));
    REQUIRE(quality.at(3, 0, 3) == Catch::Approx(3.0f));
}

// ── Measured noise: the stack's realised scatter, surfaced ──
//
// NukeX's noise_map is the PREDICTED uncertainty of each estimate (a CCD
// model, or Welford across frames). It cannot see noise the estimator itself
// injects, which is how a 1.05-1.47x penalty survived several releases.
// assemble_measured_noise surfaces the realised pixel-to-pixel scatter beside
// it, and the ratio of the two is the instrument that would have caught it.

TEST_CASE("OutputAssembler: measured noise map carries local_rms", "[assembler]") {
    auto config = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(8, 8, config);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            cube.at(x, y).local_rms = 0.25f * static_cast<float>(x);

    auto measured = OutputAssembler::assemble_measured_noise(cube);
    REQUIRE(measured.width() == 8);
    REQUIRE(measured.height() == 8);
    REQUIRE(measured.n_channels() == 1);
    REQUIRE(measured.at(0, 3, 0) == Catch::Approx(0.0f));
    REQUIRE(measured.at(4, 3, 0) == Catch::Approx(1.0f));
}

TEST_CASE("OutputAssembler: measured/predicted ratio is 1 when they agree (mono)",
          "[assembler]") {
    Image measured(8, 8, 1);
    Image predicted(8, 8, 1);
    measured.fill(0.02f);
    predicted.fill(0.02f);
    REQUIRE(OutputAssembler::measured_vs_predicted_ratio(measured, predicted)
            == Catch::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("OutputAssembler: measured/predicted ratio reports an inflated measurement",
          "[assembler]") {
    Image measured(8, 8, 1);
    Image predicted(8, 8, 1);
    measured.fill(0.03f);
    predicted.fill(0.02f);
    REQUIRE(OutputAssembler::measured_vs_predicted_ratio(measured, predicted)
            == Catch::Approx(1.5).epsilon(1e-4));
}

TEST_CASE("OutputAssembler: ratio combines colour channels in quadrature",
          "[assembler]") {
    // Measured noise is a LUMINANCE quantity (the spatial kernel builds a
    // luminance window), so a 3-channel predicted map must be combined the
    // same way before the two are comparable:
    //   sigma_L = sqrt((0.2126 s_R)^2 + (0.7152 s_G)^2 + (0.0722 s_B)^2)
    // With every channel at 1.0 that is 0.74961, so a measured map holding
    // 0.74961 must read as a ratio of exactly 1.
    Image predicted(4, 4, 3);
    predicted.fill(1.0f);
    Image measured(4, 4, 1);
    measured.fill(0.74961f);
    REQUIRE(OutputAssembler::measured_vs_predicted_ratio(measured, predicted)
            == Catch::Approx(1.0).epsilon(1e-3));
}

// The console prints measured, predicted and their ratio. Those three numbers
// have to reconcile, or the instrument teaches the reader to distrust it. The
// ratio combines colour channels in quadrature to a luminance equivalent, so
// the printed "predicted" must be computed the same way -- printing channel
// 0's median instead makes a 1.05x read as 1.28x on a colour stack.

TEST_CASE("OutputAssembler: predicted luminance median matches the ratio's basis",
          "[assembler]") {
    Image predicted(8, 8, 3);
    predicted.fill(1.0f);
    // sqrt((0.2126)^2 + (0.7152)^2 + (0.0722)^2) = 0.74961
    REQUIRE(OutputAssembler::predicted_luminance_median(predicted)
            == Catch::Approx(0.74961).epsilon(1e-3));
}

TEST_CASE("OutputAssembler: predicted luminance median is the plain median on mono",
          "[assembler]") {
    Image predicted(8, 8, 1);
    predicted.fill(0.02f);
    REQUIRE(OutputAssembler::predicted_luminance_median(predicted)
            == Catch::Approx(0.02).epsilon(1e-4));
}

TEST_CASE("OutputAssembler: printed predicted and ratio agree on a colour stack",
          "[assembler]") {
    // The regression this exists to catch: measured / printed-predicted must
    // equal the reported ratio on a 3-channel image.
    Image predicted(8, 8, 3);
    predicted.fill(1.0f);
    Image measured(8, 8, 1);
    measured.fill(1.5f * 0.74961f);
    const double ratio = OutputAssembler::measured_vs_predicted_ratio(measured, predicted);
    const double printed_p = OutputAssembler::predicted_luminance_median(predicted);
    REQUIRE(ratio == Catch::Approx(1.5).epsilon(1e-3));
    REQUIRE(1.5f * 0.74961f / printed_p == Catch::Approx(ratio).epsilon(1e-3));
}
