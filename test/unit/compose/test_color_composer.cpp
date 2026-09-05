#include "catch_amalgamated.hpp"
#include "nukex/compose/color_composer.hpp"

#include <cmath>

using namespace nukex;

static DerivedSlots zeroed() {
    DerivedSlots s;
    s.L = 0; s.R = 0; s.G = 0; s.B = 0;
    s.Ha = 0; s.OIII = 0; s.SII = 0;
    return s;
}

TEST_CASE("ColorComposer: pure broadband mid-gray maps to mid-gray sRGB",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.5; pix.G = 0.5; pix.B = 0.5;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.b == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("ColorComposer: pure Hα signal (no broadband) drives red",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5; // mid luminance
    pix.Ha = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r > out.g);
    REQUIRE(out.r > out.b);
}

TEST_CASE("ColorComposer: pure OIII signal (no broadband) drives cyan-blue",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5;
    pix.OIII = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.b > out.r);
    REQUIRE(out.g > out.r); // cyan = G+B > R
}

TEST_CASE("ColorComposer: pure SII signal (no broadband) drives deep red",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5;
    pix.SII = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r > out.g);
    REQUIRE(out.r > out.b);
}

TEST_CASE("ColorComposer: gamut soft-clip preserves hue, increments counter",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 1.0;
    pix.Ha = 5.0;   // huge over-saturation
    pix.OIII = 0.0;
    pix.SII = 0.0;

    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r >= 0.0); REQUIRE(out.r <= 1.0);
    REQUIRE(out.g >= 0.0); REQUIRE(out.g <= 1.0);
    REQUIRE(out.b >= 0.0); REQUIRE(out.b <= 1.0);
    REQUIRE(c.gamut_clipped_count() >= 1);

    // Hue preserved: red dominant
    REQUIRE(out.r >= out.g);
    REQUIRE(out.r >= out.b);
}

TEST_CASE("ColorComposer continuum-subtract: pure broadband stays unchanged",
          "[color_composer][continuum]") {
    ColorComposer c;
    c.set_mode(ColorComposer::Mode::CONTINUUM_SUBTRACT);
    c.set_continuum_coefficients({0.1, 0.1, 0.1});

    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.5; pix.G = 0.5; pix.B = 0.5;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.b == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("ColorComposer continuum-subtract: Hα with continuum is suppressed",
          "[color_composer][continuum]") {
    ColorComposer c;
    c.set_mode(ColorComposer::Mode::CONTINUUM_SUBTRACT);
    c.set_continuum_coefficients({1.0, 0.0, 0.0}); // k_Ha = 1

    // Pixel with continuum Ha + matching broadband R → subtraction zeroes Ha
    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.3; pix.G = 0; pix.B = 0;
    pix.Ha = 0.3; // exactly matches continuum

    sRGBPixel out = c.compose_pixel(pix);
    // Should look like a pure broadband pixel with no Hα chroma boost
    REQUIRE(c.last_pixel_emission_a() == Catch::Approx(0.0).margin(0.5));
    (void)out;
}

TEST_CASE("ColorComposer: zero signal everywhere → black", "[color_composer]") {
    ColorComposer c;
    sRGBPixel out = c.compose_pixel(zeroed());
    REQUIRE(out.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(out.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(out.b == Catch::Approx(0.0).margin(0.001));
}

// ---------------------------------------------------------------------------
// Brightness-invariant hue (Lupton et al. 2004, PASP 116, 133) and the
// explicit chroma gate that replaces the accident of an absent division.
// ---------------------------------------------------------------------------

TEST_CASE("ColorComposer: emission hue is a line ratio, not a function of brightness",
          "[color_composer]") {
    ColorComposer faint, bright;
    auto s_faint = zeroed();
    s_faint.Ha = 0.02; s_faint.OIII = 0.01;
    auto s_bright = zeroed();
    s_bright.Ha = 0.60; s_bright.OIII = 0.30;

    faint.compose_pixel(s_faint);
    bright.compose_pixel(s_bright);

    // Identical 2:1 Ha:OIII ratio, 30x apart in brightness -> same chrominance.
    REQUIRE(faint.last_pixel_emission_a()
            == Catch::Approx(bright.last_pixel_emission_a()).margin(1e-9));
    REQUIRE(faint.last_pixel_emission_b()
            == Catch::Approx(bright.last_pixel_emission_b()).margin(1e-9));
}

TEST_CASE("ColorComposer: OIII-dominant pixel renders teal, not blue-black",
          "[color_composer]") {
    ColorComposer c;
    auto s = zeroed();
    s.Ha = 0.01; s.OIII = 0.09; s.L = 0.35;
    sRGBPixel out = c.compose_pixel(s);
    // The M16 failure was green identically zero across 24.5M pixels.
    REQUIRE(out.g > 0.02);
    REQUIRE(out.b > out.r);
}

TEST_CASE("ColorComposer: out-of-gamut pixels keep their hue exactly",
          "[color_composer]") {
    ColorComposer c;
    double r = 1.6, g = 0.8, b = 0.4;
    REQUIRE(c.clip_to_gamut_for_test(r, g, b));
    // Lupton: rescale all three by their common maximum. Independent
    // clamping would give (1.0, 0.8, 0.4) and change the hue.
    REQUIRE(r == Catch::Approx(1.0));
    REQUIRE(g == Catch::Approx(0.5));
    REQUIRE(b == Catch::Approx(0.25));
}

TEST_CASE("ColorComposer: negative channels floor without rescaling the rest",
          "[color_composer]") {
    ColorComposer c;
    double r = 0.5, g = -0.2, b = 0.25;
    REQUIRE(c.clip_to_gamut_for_test(r, g, b));
    REQUIRE(r == Catch::Approx(0.5));
    REQUIRE(g == Catch::Approx(0.0));
    REQUIRE(b == Catch::Approx(0.25));
}

TEST_CASE("ColorComposer: a near-zero emission pixel is not fully saturated",
          "[color_composer]") {
    // The cliff that normalisation introduces: total_w = 1e-9 is > 0, so a
    // bare division would hand this pixel the entire palette vector and
    // manufacture saturated colour out of noise.
    ColorComposer c;
    c.set_chroma_gate(/*background*/ 0.0, /*full_scale*/ 0.01);
    auto s = zeroed();
    s.Ha = 1e-9;
    c.compose_pixel(s);

    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(0.0).margin(1e-6));
    REQUIRE(std::abs(c.last_pixel_emission_a()) < 1e-3);
}

TEST_CASE("ColorComposer: a pixel above the chroma gate keeps full saturation",
          "[color_composer]") {
    ColorComposer c;
    c.set_chroma_gate(/*background*/ 0.0, /*full_scale*/ 0.01);
    auto s = zeroed();
    s.Ha = 0.5;   // far above the floor
    c.compose_pixel(s);

    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(1.0));
    // Pure Ha normalises to exactly the palette entry.
    REQUIRE(c.last_pixel_emission_a() == Catch::Approx(50.0).margin(1e-9));
    REQUIRE(c.last_pixel_emission_b() == Catch::Approx(10.0).margin(1e-9));
}

TEST_CASE("ColorComposer: sky at the measured background is desaturated",
          "[color_composer]") {
    // The pedestal case. Blank sky sits at the background weight, so it
    // must come out neutral even though its total_w is far from zero.
    ColorComposer c;
    c.set_chroma_gate(/*background*/ 0.010, /*full_scale*/ 0.040);
    auto sky = zeroed();
    sky.Ha = 0.010;
    c.compose_pixel(sky);
    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(0.0).margin(1e-9));

    auto mid = zeroed();
    mid.Ha = 0.025;   // halfway between background and full scale
    c.compose_pixel(mid);
    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(0.5).margin(1e-9));

    auto bright = zeroed();
    bright.Ha = 0.30;
    c.compose_pixel(bright);
    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(1.0));
}
