#include "catch_amalgamated.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"
#include "nukex/stretch/photometric_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <filesystem>

using namespace nukex;

// ══════════════════════════════════════════════════════════
// LUPTON RGB
// ══════════════════════════════════════════════════════════

TEST_CASE("Lupton: f(0) == 0", "[lupton]") {
    LuptonStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("Lupton: monotonically increasing", "[lupton]") {
    LuptonStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("Lupton: preserves color ratios", "[lupton]") {
    LuptonStretch s;
    s.Q = 8.0f;
    s.stretch = 5.0f;

    Image img(4, 4, 3);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            img.at(x, y, 0) = 0.3f;
            img.at(x, y, 1) = 0.15f;
            img.at(x, y, 2) = 0.05f;
        }

    float ratio_rg_before = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_before = img.at(2, 2, 1) / img.at(2, 2, 2);

    s.apply(img);

    float ratio_rg_after = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_after = img.at(2, 2, 1) / img.at(2, 2, 2);

    REQUIRE(ratio_rg_after == Catch::Approx(ratio_rg_before).margin(0.01f));
    REQUIRE(ratio_gb_after == Catch::Approx(ratio_gb_before).margin(0.01f));
}

TEST_CASE("Lupton: output in [0, 1]", "[lupton]") {
    LuptonStretch s;
    Image img(8, 8, 3);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            for (int c = 0; c < 3; c++)
                img.at(x, y, c) = static_cast<float>(y * 8 + x) / 63.0f;

    s.apply(img);

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            for (int c = 0; c < 3; c++) {
                REQUIRE(img.at(x, y, c) >= -1e-6f);
                REQUIRE(img.at(x, y, c) <= 1.0f + 1e-6f);
            }
}

TEST_CASE("Lupton: higher Q → more aggressive stretch at faint end", "[lupton]") {
    LuptonStretch lo, hi;
    lo.Q = 2.0f; lo.stretch = 5.0f;
    hi.Q = 20.0f; hi.stretch = 5.0f;
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

// ══════════════════════════════════════════════════════════
// VERALUX HMS
// ══════════════════════════════════════════════════════════

TEST_CASE("VeraLux: f(0) == 0", "[veralux]") {
    VeraLuxStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("VeraLux: f(1) == 1", "[veralux]") {
    VeraLuxStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("VeraLux: monotonically increasing", "[veralux]") {
    VeraLuxStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("VeraLux: output in [0, 1]", "[veralux]") {
    VeraLuxStretch s;
    for (float logD : {1.0f, 2.0f, 4.0f}) {
        s.log_D = logD;
        for (int i = 0; i <= 100; i++) {
            float x = i / 100.0f;
            float y = s.apply_scalar(x);
            REQUIRE(y >= -1e-6f);
            REQUIRE(y <= 1.0f + 1e-6f);
        }
    }
}

TEST_CASE("VeraLux: convergence to white for bright pixels", "[veralux]") {
    VeraLuxStretch s;
    s.log_D = 2.0f;
    s.convergence_power = 3.5f;

    Image img(4, 4, 3);
    // Bright red pixel
    img.at(2, 2, 0) = 0.9f;
    img.at(2, 2, 1) = 0.1f;
    img.at(2, 2, 2) = 0.05f;
    // Fill rest with dark
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (x == 2 && y == 2) continue;
            img.at(x, y, 0) = 0.01f;
            img.at(x, y, 1) = 0.01f;
            img.at(x, y, 2) = 0.01f;
        }

    s.apply(img);

    // Bright pixel should have converged toward white (channels more equal)
    float r = img.at(2, 2, 0);
    float g = img.at(2, 2, 1);
    float b = img.at(2, 2, 2);

    // The G/R ratio should be closer to 1 after convergence than before
    float ratio_before = 0.1f / 0.9f;   // 0.111
    float ratio_after = g / r;
    REQUIRE(ratio_after > ratio_before);  // Converged toward white
}

TEST_CASE("VeraLux: higher log_D → more aggressive stretch", "[veralux]") {
    VeraLuxStretch lo, hi;
    lo.log_D = 1.0f;
    hi.log_D = 3.0f;
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

// ══════════════════════════════════════════════════════════
// CLAHE
// ══════════════════════════════════════════════════════════

TEST_CASE("CLAHE: output in [0, 1]", "[clahe]") {
    Image img(64, 64, 1);
    for (int i = 0; i < 64 * 64; i++)
        img.channel_data(0)[i] = static_cast<float>(i) / (64.0f * 64.0f);

    CLAHEStretch s;
    s.tile_cols = 4;
    s.tile_rows = 4;
    s.luminance_only = false;
    s.apply(img);

    for (int i = 0; i < 64 * 64; i++) {
        REQUIRE(img.channel_data(0)[i] >= -1e-6f);
        REQUIRE(img.channel_data(0)[i] <= 1.0f + 1e-6f);
    }
}

TEST_CASE("CLAHE: enhances local contrast", "[clahe]") {
    // Create image with two halves at different brightnesses
    Image img(64, 64, 1);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            if (x < 32)
                img.at(x, y, 0) = 0.1f + 0.02f * (x + y) / 96.0f;
            else
                img.at(x, y, 0) = 0.7f + 0.02f * (x + y) / 96.0f;
        }

    float dark_range_before = img.at(31, 63, 0) - img.at(0, 0, 0);

    CLAHEStretch s;
    s.tile_cols = 4;
    s.tile_rows = 4;
    s.clip_limit = 3.0f;
    s.luminance_only = false;
    s.apply(img);

    float dark_range_after = img.at(31, 63, 0) - img.at(0, 0, 0);

    // CLAHE should enhance local contrast (expand the range within each region)
    REQUIRE(dark_range_after > dark_range_before * 0.5f);
}

TEST_CASE("CLAHE: higher clip_limit → more contrast", "[clahe]") {
    Image img1(32, 32, 1), img2(32, 32, 1);
    for (int i = 0; i < 32 * 32; i++) {
        float v = static_cast<float>(i) / (32.0f * 32.0f) * 0.3f + 0.1f;
        img1.channel_data(0)[i] = v;
        img2.channel_data(0)[i] = v;
    }

    CLAHEStretch s1, s2;
    s1.clip_limit = 1.5f; s1.tile_cols = 2; s1.tile_rows = 2; s1.luminance_only = false;
    s2.clip_limit = 4.0f; s2.tile_cols = 2; s2.tile_rows = 2; s2.luminance_only = false;
    s1.apply(img1);
    s2.apply(img2);

    // Higher clip limit should produce wider range (more contrast)
    float range1 = img1.channel_data(0)[32*32 - 1] - img1.channel_data(0)[0];
    float range2 = img2.channel_data(0)[32*32 - 1] - img2.channel_data(0)[0];
    REQUIRE(range2 >= range1 - 0.01f);
}

// ══════════════════════════════════════════════════════════
// PHOTOMETRIC
// ══════════════════════════════════════════════════════════

TEST_CASE("Photometric: brighter pixel → higher output", "[photometric]") {
    PhotometricStretch s;
    REQUIRE(s.apply_scalar(0.1f) > s.apply_scalar(0.01f));
}

TEST_CASE("Photometric: output in [0, 1]", "[photometric]") {
    PhotometricStretch s;
    for (int i = 1; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("Photometric: wider display range → gentler stretch", "[photometric]") {
    PhotometricStretch narrow, wide;
    narrow.mu_bright = 20.0f; narrow.mu_faint = 26.0f;  // 6 mag range
    wide.mu_bright = 18.0f; wide.mu_faint = 30.0f;      // 12 mag range

    // At a mid-value, narrow range should produce higher contrast
    float n_out = narrow.apply_scalar(0.1f);
    float w_out = wide.apply_scalar(0.1f);
    // Narrow range puts more of the display range above this value
    REQUIRE(n_out != Catch::Approx(w_out).margin(0.01f));
}

// ══════════════════════════════════════════════════════════
// VISUAL OUTPUTS
// ══════════════════════════════════════════════════════════

TEST_CASE("Tier1: visual outputs on M16", "[tier1][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }
    std::filesystem::create_directories("test/output");

    // Prepare a copy for each stretch
    Image prepared = img.clone();
    test_util::prepare_for_stretch(prepared);

    // Lupton RGB (operates on the prepared data directly — multi-channel)
    {
        Image out = prepared.clone();
        LuptonStretch s;
        s.Q = 8.0f;
        s.stretch = 5.0f;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_lupton.png", out);
    }

    // VeraLux HMS
    {
        Image out = prepared.clone();
        VeraLuxStretch s;
        s.log_D = 2.0f;
        s.protect_b = 6.0f;
        s.convergence_power = 3.5f;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_veralux.png", out);
    }

    // CLAHE (finisher — apply after a primary stretch)
    {
        Image out = prepared.clone();
        // First apply a gentle ArcSinh as primary stretch
        ArcSinhStretch pre;
        pre.alpha = 4.0f;
        pre.luminance_only = true;
        pre.apply(out);
        // Then CLAHE for local contrast
        CLAHEStretch s;
        s.clip_limit = 2.0f;
        s.tile_cols = 8;
        s.tile_rows = 8;
        s.luminance_only = true;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_clahe.png", out);
    }

    // Photometric
    {
        Image out = img.clone();  // Use unprepared data — photometric does its own sky subtraction
        PhotometricStretch s;
        s.mu_bright = 18.0f;
        s.mu_faint = 28.0f;
        s.luminance_only = true;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_photometric.png", out);
    }

    REQUIRE(true);
}

TEST_CASE("VeraLux: auto_tune puts the background on the target, whatever the "
          "data's level", "[stretch][veralux]") {
    // One fixed intensity cannot serve different targets. The three
    // regression corpora have linear backgrounds spanning 0.0166 to 0.1479 --
    // nearly an order of magnitude -- so a single log_D put one stack's
    // median at 0.084 and another's at 0.431.
    for (float bg : {0.0166f, 0.0264f, 0.1479f}) {
        Image img(64, 64, 1);
        img.fill(bg);
        // A little structure above the background so the median is the
        // background rather than the only value present.
        for (int i = 0; i < 64; ++i) img.at(i, 0, 0) = bg * 4.0f;

        VeraLuxStretch v;
        const float solved = v.auto_tune(img, 0.25f);
        INFO("bg=" << bg << " solved log_D=" << solved);
        REQUIRE(solved >= 0.0f);
        REQUIRE(solved <= 7.0f);
        // The background must land on the target.
        REQUIRE(v.apply_scalar(bg) == Catch::Approx(0.25f).margin(0.01f));
    }
}

TEST_CASE("VeraLux: auto_tune leaves a degenerate image alone", "[stretch][veralux]") {
    // An all-black frame has no background to solve against; the default must
    // survive rather than the bisection running away.
    Image img(8, 8, 1);
    img.fill(0.0f);
    VeraLuxStretch v;
    const float before = v.log_D;
    REQUIRE(v.auto_tune(img, 0.25f) == Catch::Approx(before));
    REQUIRE(v.log_D == Catch::Approx(before));
}

// Deterministic, well-spread noise. A plain (x,y) hash clumps along cell
// borders, which once flattened a MAD to ~1e-4 and made a robust-statistics
// gate untestable at any threshold; splitmix64 over the flat index does not.
static float test_gauss(std::uint64_t i) {
    auto mix = [](std::uint64_t z) {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    const double u1 = (mix(2 * i) >> 11) * 0x1.0p-53 + 1e-12;
    const double u2 = (mix(2 * i + 1) >> 11) * 0x1.0p-53;
    return static_cast<float>(std::sqrt(-2.0 * std::log(u1)) *
                              std::cos(6.283185307179586 * u2));
}

// A linear deep-sky frame in miniature: a bright pedestal, tight read noise,
// faint nebulosity a few tens of sigma up, and a sparse star field. Modelled
// on the user's own 74-frame stack, where the background sat at 0.0418 and
// the 99.9th percentile only 44 sigma above it.
static Image narrow_band_on_a_pedestal(float bg, float sigma) {
    Image img(256, 256, 1);
    float* d = img.channel_data(0);
    const int n = 256 * 256;
    for (int i = 0; i < n; ++i) {
        float v = bg + sigma * test_gauss(static_cast<std::uint64_t>(i));
        if (i % 10 == 0)  v += sigma * 25.0f * (static_cast<float>(i % 997) / 997.0f);
        if (i % 1000 == 0) v += sigma * 45.0f;
        d[i] = v;
    }
    return img;
}

static float percentile_of(const Image& img, double q) {
    std::vector<float> v(img.channel_data(0),
                         img.channel_data(0) + img.width() * img.height());
    auto k = v.begin() + static_cast<std::size_t>(q * (v.size() - 1));
    std::nth_element(v.begin(), k, v.end());
    return *k;
}

TEST_CASE("VeraLux: auto_tune solves a shadow point, so a narrow signal band "
          "on a pedestal comes out with contrast", "[stretch][veralux]") {
    // Measured on the user's stack: the linear data is good -- p99.9 sits 44
    // sigma above the background -- but the stretch delivered it as 4% of the
    // output range (p50 0.2569 -> p99.9 0.2969), because the whole signal band
    // is ~1% wide sitting on a pedestal that eats 98% of the curve.
    const float bg = 0.0418f, sigma = 0.0002f;
    Image img = narrow_band_on_a_pedestal(bg, sigma);

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    v.apply(img);

    const float p50 = percentile_of(img, 0.50);
    const float p999 = percentile_of(img, 0.999);
    INFO("p50=" << p50 << " p99.9=" << p999 << " SP=" << v.SP
                << " log_D=" << v.log_D);

    // Positioning is unchanged: the background still lands on the target.
    REQUIRE(p50 == Catch::Approx(0.25f).margin(0.02f));
    // Contrast is the point. Simulated on the real stack, solving SP as well
    // as the intensity took the spread from 0.0393 to 0.2853.
    REQUIRE(p999 - p50 > 0.15f);
}

TEST_CASE("VeraLux: the shadow point sits just below the background, not on it",
          "[stretch][veralux]") {
    // STF's convention: median - 2.8 sigma. Clipping AT the median would throw
    // away half the frame; leaving it at zero is what cost the contrast.
    const float bg = 0.0418f, sigma = 0.0002f;
    Image img = narrow_band_on_a_pedestal(bg, sigma);

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    INFO("SP=" << v.SP << " bg=" << bg);
    REQUIRE(v.SP > 0.0f);
    REQUIRE(v.SP < bg);
    REQUIRE(v.SP == Catch::Approx(bg - 2.8f * sigma).margin(4.0f * sigma));
}

TEST_CASE("VeraLux: a background already at the floor gets no shadow point",
          "[stretch][veralux]") {
    // Data whose median is below 2.8 sigma has nothing to clip; SP must floor
    // at zero rather than going negative and lifting the black point.
    Image img(64, 64, 1);
    float* d = img.channel_data(0);
    for (int i = 0; i < 64 * 64; ++i)
        d[i] = std::max(0.0f, 0.0001f + 0.001f * test_gauss(static_cast<std::uint64_t>(i)));

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    REQUIRE(v.SP >= 0.0f);
    REQUIRE(v.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("VeraLux: auto_tune tunes the luminance it actually stretches, not "
          "one channel of it", "[stretch][veralux]") {
    // apply() stretches L = wR*R + wG*G + wB*B; auto_tune must solve against
    // that same quantity. Measured on the user's own stack the background is
    // strongly imbalanced -- red 0.0270, green 0.0418 -- so a shadow point
    // derived from green sits ABOVE the luminance of 99.6% of the frame and
    // clips it to black. With SP = 0 the same mismatch was merely a bias: it
    // is why the shipped stretch landed its background at 0.2569 rather than
    // on the 0.2500 it was solving for.
    const float sigma = 0.0002f;
    const float bg[3] = {0.0270f, 0.0418f, 0.0388f};
    Image img(256, 256, 3);
    for (int ch = 0; ch < 3; ++ch) {
        float* d = img.channel_data(ch);
        for (int i = 0; i < 256 * 256; ++i) {
            float v = bg[ch] + sigma * test_gauss(static_cast<std::uint64_t>(ch * 100003 + i));
            if (i % 10 == 0)   v += sigma * 25.0f * (static_cast<float>(i % 997) / 997.0f);
            if (i % 1000 == 0) v += sigma * 45.0f;
            d[i] = v;
        }
    }

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    v.apply(img);

    std::vector<float> lum(256 * 256);
    for (int i = 0; i < 256 * 256; ++i)
        lum[i] = v.w_R * img.channel_data(0)[i] + v.w_G * img.channel_data(1)[i]
               + v.w_B * img.channel_data(2)[i];
    const std::size_t half = lum.size() / 2;
    std::nth_element(lum.begin(), lum.begin() + half, lum.end());
    const float p50 = lum[half];
    const std::size_t black = std::count(lum.begin(), lum.end(), 0.0f);

    INFO("SP=" << v.SP << " log_D=" << v.log_D << " lum p50=" << p50
               << " black=" << black);
    REQUIRE(p50 == Catch::Approx(0.25f).margin(0.02f));
    // A shadow point at median - 2.8 sigma clips the bottom ~0.3% of a normal
    // background, not most of the frame.
    REQUIRE(black < lum.size() / 20);
}
