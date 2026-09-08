#include "catch_amalgamated.hpp"
#include "nukex/calibration/frame_normalization.hpp"

#include <cmath>
#include <vector>

using namespace nukex;

// A frame's sky as the measuring pass reports it: a robust location and a
// robust scale.
static FrameSky sky(double loc, double scale) { return FrameSky{loc, scale, true}; }

TEST_CASE("normalization: a stable session is left alone", "[normalization]") {
    // THE property that matters most. Most sessions are stable, and on those
    // this must be an exact no-op -- if it is not, every existing stack moves
    // for no reason. Measured on bayer_rgb_m27_2023 the model race and a plain
    // robust estimator agree to 1.00x, i.e. that corpus has nothing to correct.
    std::vector<FrameSky> frames(20, sky(0.0278, 0.00090));
    const auto c = solve_frame_normalization(frames);
    REQUIRE(c.size() == frames.size());
    for (const auto& k : c) {
        REQUIRE(k.scale  == Catch::Approx(1.0).margin(1e-12));
        REQUIRE(k.offset == Catch::Approx(0.0).margin(1e-12));
    }
}

TEST_CASE("normalization: a transparency dip is scaled back up",
          "[normalization]") {
    // Cloud or haze: the sky level AND the signal both drop. Measured on the
    // user's M63, per-frame sky offsets run -0.8 to +3.2 sigma across a
    // session -- real variation the stacker currently sees as per-pixel
    // bimodality instead of a per-frame level.
    std::vector<FrameSky> frames(9, sky(0.0300, 0.00100));
    frames[4] = sky(0.0150, 0.00050);            // half the level and the scale
    const auto c = solve_frame_normalization(frames);

    // The majority is untouched...
    REQUIRE(c[0].scale  == Catch::Approx(1.0).margin(1e-9));
    REQUIRE(c[0].offset == Catch::Approx(0.0).margin(1e-9));
    // ...and the dim frame is brought onto the same level and spread.
    REQUIRE(c[4].scale == Catch::Approx(2.0).margin(1e-9));
    const double mapped_loc   = c[4].scale * 0.0150 + c[4].offset;
    const double mapped_upper = c[4].scale * (0.0150 + 0.00050) + c[4].offset;
    REQUIRE(mapped_loc   == Catch::Approx(0.0300).margin(1e-9));
    REQUIRE(mapped_upper == Catch::Approx(0.0300 + 0.00100).margin(1e-9));
}

TEST_CASE("normalization: a longer exposure is brought onto the same scale",
          "[normalization]") {
    // The user's own M63 batch mixes 120 s and 300 s frames: sky 2685 vs 6813
    // ADU, a ratio of 2.54 against the 2.50 exposure ratio at identical gain.
    // Nothing in the pipeline normalises for it today, so the per-voxel fit
    // sees a bimodal population and rejects the minority.
    std::vector<FrameSky> frames(10, sky(2685.0, 30.0));
    for (int i = 6; i < 10; ++i) frames[i] = sky(6813.0, 76.2);   // 300 s
    const auto c = solve_frame_normalization(frames);
    for (int i = 6; i < 10; ++i) {
        const double mapped = c[i].scale * 6813.0 + c[i].offset;
        INFO("frame " << i << " scale " << c[i].scale << " offset " << c[i].offset);
        REQUIRE(mapped == Catch::Approx(2685.0).margin(1e-6));
    }
}

TEST_CASE("normalization: one wild frame does not move the reference",
          "[normalization]") {
    // The reference must be robust. A single blown or fogged frame that
    // dragged the reference would rescale every good frame to match it.
    std::vector<FrameSky> frames(15, sky(0.0300, 0.00100));
    frames[7] = sky(0.9000, 0.05000);
    const auto c = solve_frame_normalization(frames);
    for (int i = 0; i < 15; ++i) {
        if (i == 7) continue;
        REQUIRE(c[i].scale  == Catch::Approx(1.0).margin(1e-9));
        REQUIRE(c[i].offset == Catch::Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("normalization: unusable and degenerate frames are no-ops",
          "[normalization]") {
    std::vector<FrameSky> frames(5, sky(0.0300, 0.00100));
    frames[1].usable = false;
    frames[2] = sky(0.0300, 0.0);      // zero scale: nothing to measure
    const auto c = solve_frame_normalization(frames);
    for (int i : {1, 2}) {
        REQUIRE(c[i].scale  == Catch::Approx(1.0).margin(1e-12));
        REQUIRE(c[i].offset == Catch::Approx(0.0).margin(1e-12));
    }
}
