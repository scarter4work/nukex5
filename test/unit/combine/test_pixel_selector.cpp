#include "catch_amalgamated.hpp"
#include "nukex/combine/pixel_selector.hpp"
#include "nukex/core/distribution.hpp"
#include <cmath>

using namespace nukex;

namespace {
FrameStats make_frame(float rn, float g, bool has_kw = true) {
    FrameStats fs;
    fs.read_noise = rn;
    fs.gain = g;
    fs.has_noise_keywords = has_kw;
    fs.frame_weight = 1.0f;
    fs.psf_weight = 1.0f;
    fs.median_luminance = 0.5f;
    return fs;
}
} // anonymous namespace

TEST_CASE("PixelSelector::sample_variance: CCD noise model in normalized units", "[combine]") {
    FrameStats fs = make_frame(3.0f, 1.5f);
    float var = PixelSelector::sample_variance(0.5f, fs, 0, 0.0f);
    // value_adu = 0.5 * 65535 = 32767.5
    // shot_var_adu = 32767.5 / 1.5 = 21845.0
    // read_var_adu = 9 / 2.25 = 4.0
    // total in normalized^2 = 21849 / 65535^2 ≈ 5.087e-6
    float expected = 21849.0f / (65535.0f * 65535.0f);
    REQUIRE(var == Catch::Approx(expected).margin(1e-8f));
}

TEST_CASE("PixelSelector::sample_variance: no keywords → Welford fallback", "[combine]") {
    FrameStats fs = make_frame(3.0f, 1.5f, false);
    float var = PixelSelector::sample_variance(0.5f, fs, 0, 0.02f);
    REQUIRE(var == Catch::Approx(0.02f));
}

TEST_CASE("PixelSelector::select: output value from distribution", "[combine]") {
    ZDistribution dist{};
    dist.true_signal_estimate = 0.42f;

    float values[] = {0.40f, 0.42f, 0.44f};
    float weights[] = {1.0f, 1.0f, 1.0f};
    int frame_indices[] = {0, 1, 2};
    FrameStats fs_arr[] = {make_frame(3, 1.5), make_frame(3, 1.5), make_frame(3, 1.5)};

    PixelSelector sel;
    float val, noise, snr;
    sel.select(dist, values, weights, 3, fs_arr, frame_indices, /*slot=*/0, 0.0f,
               val, noise, snr);

    REQUIRE(val == Catch::Approx(0.42f));
    REQUIRE(noise > 0.0f);
    REQUIRE(snr > 0.0f);
}

TEST_CASE("PixelSelector::select: more samples → lower noise", "[combine]") {
    ZDistribution dist{};
    dist.true_signal_estimate = 0.5f;

    float values10[10]; float weights10[10]; int fi10[10];
    FrameStats fs10[10];
    for (int i = 0; i < 10; i++) {
        values10[i] = 0.5f; weights10[i] = 1.0f; fi10[i] = i;
        fs10[i] = make_frame(3, 1.5);
    }

    float values100[100]; float weights100[100]; int fi100[100];
    FrameStats fs100[100];
    for (int i = 0; i < 100; i++) {
        values100[i] = 0.5f; weights100[i] = 1.0f; fi100[i] = i;
        fs100[i] = make_frame(3, 1.5);
    }

    PixelSelector sel;
    float v1, n1, s1, v2, n2, s2;
    sel.select(dist, values10, weights10, 10, fs10, fi10, /*slot=*/0, 0.0f, v1, n1, s1);
    sel.select(dist, values100, weights100, 100, fs100, fi100, /*slot=*/0, 0.0f, v2, n2, s2);

    REQUIRE(n2 < n1);
    float ratio = n1 / n2;
    REQUIRE(ratio == Catch::Approx(std::sqrt(10.0f)).margin(0.5f));
}

TEST_CASE("PixelSelector::select: SNR clamped to 9999", "[combine]") {
    ZDistribution dist{};
    dist.true_signal_estimate = 100.0f;

    float values[] = {100.0f};
    float weights[] = {1.0f};
    int fi[] = {0};
    FrameStats fs = make_frame(0.001f, 100.0f);

    PixelSelector sel;
    float val, noise, snr;
    sel.select(dist, values, weights, 1, &fs, fi, /*slot=*/0, 0.0f, val, noise, snr);

    REQUIRE(snr <= 9999.0f);
}

TEST_CASE("PixelSelector::sample_variance: the identity normalisation is "
          "bit-for-bit the un-normalised model", "[combine][normalization]") {
    // The whole no-op argument rests on this. Every batch that needs no
    // correction carries scale 1 / offset 0, and (v - 0)/1 and 1*1*x are
    // exact in IEEE-754 -- so this is an equality, not an approximation. If
    // it ever becomes approximate, every existing golden moves.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    FrameStats plain = fs;   // norm_* default to the identity

    const float got = PixelSelector::sample_variance(0.5f, fs, 0, 0.0f);
    const float value_adu = 0.5f * 65535.0f;
    const float expect = (value_adu / 1.5f + (3.2f * 3.2f) / (1.5f * 1.5f))
                       / (65535.0f * 65535.0f);
    REQUIRE(got == expect);          // exact, deliberately not Approx
    REQUIRE(got == PixelSelector::sample_variance(0.5f, plain, 3, 0.0f));
}

TEST_CASE("PixelSelector::sample_variance: shot noise is read on the raw value, "
          "not the normalised one", "[combine][normalization]") {
    // A frame scaled up by 2 has had its noise scaled up by 2 as well, so its
    // variance is 4x -- NOT the variance of a genuinely brighter exposure,
    // which would only be 2x by Poisson. Reading the normalised value as ADU
    // would report the latter and quietly over-trust every rescaled frame.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    fs.norm_scale[1] = 2.0f; fs.norm_offset[1] = 0.0f;

    const float raw  = 0.25f;
    const float seen = 2.0f * raw;                    // what Phase A cached
    const float base = PixelSelector::sample_variance(raw, FrameStats{fs}, 0, 0.0f);
    const float got  = PixelSelector::sample_variance(seen, fs, 1, 0.0f);
    REQUIRE(got == Catch::Approx(4.0f * base).epsilon(1e-6));

    // The wrong answer, for contrast: treating `seen` as raw ADU.
    const float naive = PixelSelector::sample_variance(seen, FrameStats{fs}, 0, 0.0f);
    REQUIRE(naive < got);
}

TEST_CASE("PixelSelector::sample_variance: an additive offset is removed before "
          "the Poisson term", "[combine][normalization]") {
    // Sky pedestal differences are the additive half of normalisation. The
    // Poisson term is a function of collected photons, so a frame lifted by
    // an offset must not be charged shot noise for the lift.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    fs.norm_scale[0] = 1.0f; fs.norm_offset[0] = 0.10f;

    const float raw = 0.20f;
    const float got = PixelSelector::sample_variance(raw + 0.10f, fs, 0, 0.0f);
    FrameStats id;  id.has_noise_keywords = true; id.gain = 1.5f; id.read_noise = 3.2f;
    // Approx, not equality: (raw + 0.10f) - 0.10f is not raw in binary
    // floating point. Bit-exactness is claimed only for the identity, which
    // its own case above asserts.
    REQUIRE(got == Catch::Approx(PixelSelector::sample_variance(raw, id, 0, 0.0f))
                       .epsilon(1e-6));
}

TEST_CASE("PixelSelector::sample_variance: an unmeasurable scale falls back to "
          "the identity", "[combine][normalization]") {
    // scale <= 0 is how the solver reports "could not measure this frame".
    // Dividing by it would be division by noise, or by zero.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    FrameStats id = fs;
    fs.norm_scale[2] = 0.0f; fs.norm_offset[2] = 0.5f;
    REQUIRE(PixelSelector::sample_variance(0.5f, fs, 2, 0.0f)
            == PixelSelector::sample_variance(0.5f, id, 0, 0.0f));
}
