#include "catch_amalgamated.hpp"
#include "nukex/compose/compose_image.hpp"
#include "nukex/compose/color_composer.hpp"

#include <cmath>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace nukex;

// The dual-narrowband colour regression, in one sentence: the composer
// gamut-maps the emission palette at the LINEAR luminance of the data, where
// L* is a few units and sRGB holds almost no chroma, so the palette is walked
// to grey before the stretch ever runs. Measured on M16: stacked saturation
// 0.099 -> composed 0.018 -> stretched 0.011, against 0.085-0.095 when the
// stretch still ran on the raw channels.
//
// The cure keeps hue and chroma where they belong -- the linear line ratios --
// and assigns L* from the STRETCHED luminance, mapping to the gamut once,
// there. These tests pin the two halves: with the composer's own luminance
// the new route is the old one bit for bit, and with a stretched luminance a
// saturated emission pixel keeps its colour.

namespace {

using Slots = std::unordered_map<std::string, std::vector<float>>;

double hsv_saturation(const sRGBPixel& p) {
    const double mx = std::max(p.r, std::max(p.g, p.b));
    const double mn = std::min(p.r, std::min(p.g, p.b));
    return mx > 0.0 ? (mx - mn) / mx : 0.0;
}

} // namespace

TEST_CASE("compose_slots_with_luminance at the composer's own luminance is "
          "compose_slots_to_image, to the float rounding of the luminance plane", "[compose]") {
    const int W = 24, H = 16, N = W * H;
    Slots slots;
    slots["Ha"].resize(N); slots["OIII"].resize(N);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(0.0f, 0.12f);
    for (int i = 0; i < N; i++) { slots["Ha"][i] = U(rng); slots["OIII"][i] = U(rng); }

    ColorComposer a, b;
    a.set_chroma_gate(0.02, 0.08);
    b.set_chroma_gate(0.02, 0.08);
    const Image direct = compose_slots_to_image(W, H, slots, a);
    const Image lum    = compose_luminance_image(W, H, slots);
    REQUIRE(lum.n_channels() == 1);
    const Image routed = compose_slots_with_luminance(W, H, slots, b, lum);

    REQUIRE(routed.n_channels() == 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 3; c++)
                // The luminance plane is float, so L* is computed from a
                // float-rounded value: differences live in the 7th decimal.
                REQUIRE(routed.at(x, y, c) == Catch::Approx(direct.at(x, y, c)).margin(1e-6));
    REQUIRE(a.gamut_clipped_count() == b.gamut_clipped_count());
}

TEST_CASE("a stretched luminance gives the emission colour the brightness the "
          "linear one cannot", "[compose]") {
    // An Ha-dominant nebula pixel at real narrowband levels. At its own
    // luminance the composer can only produce a DARK colour -- L* of a few
    // units -- which is why the composed window of a dual-narrowband stack
    // looks black with a tint. At the luminance an auto-stretch assigns the
    // nebula, the same hue and chroma fit as composed, bright.
    const int W = 1, H = 1;
    Slots slots;
    slots["Ha"]   = {0.06f};
    slots["OIII"] = {0.02f};

    ColorComposer composer;   // gate disabled: full palette chroma
    const Image linear_lum = compose_luminance_image(W, H, slots);
    REQUIRE(linear_lum.at(0, 0, 0) == Catch::Approx(0.08f));

    const Image at_linear = compose_slots_with_luminance(W, H, slots, composer, linear_lum);
    const sRGBPixel p_lin{at_linear.at(0, 0, 0), at_linear.at(0, 0, 1), at_linear.at(0, 0, 2)};

    Image stretched_lum(W, H, 1);
    stretched_lum.at(0, 0, 0) = 0.55f;           // what an auto-stretch puts a nebula at
    const Image at_stretched = compose_slots_with_luminance(W, H, slots, composer, stretched_lum);
    const double fit_stretched = composer.last_pixel_gamut_chroma_scale();
    const sRGBPixel p_str{at_stretched.at(0, 0, 0), at_stretched.at(0, 0, 1), at_stretched.at(0, 0, 2)};

    INFO("linear: rgb " << p_lin.r << " " << p_lin.g << " " << p_lin.b);
    INFO("stretched: rgb " << p_str.r << " " << p_str.g << " " << p_str.b << " chroma fit " << fit_stretched);
    // Linear: dark. Nothing a stretch of THIS image could brighten without
    // also stretching its clipped chroma.
    REQUIRE(std::max(p_lin.r, std::max(p_lin.g, p_lin.b)) < 0.25);
    // Stretched: bright, red-dominant, saturated, and the palette fitted as
    // composed with no gamut walk.
    REQUIRE(std::max(p_str.r, std::max(p_str.g, p_str.b)) >= 0.55);
    REQUIRE(fit_stretched == Catch::Approx(1.0));
    REQUIRE(hsv_saturation(p_str) > 0.3);
    REQUIRE(p_str.r > p_str.g);
    REQUIRE(p_str.r > p_str.b);
}

TEST_CASE("the chroma gate still holds at a stretched luminance: sky stays neutral",
          "[compose]") {
    // The gate is evaluated on the LINEAR emission total, where sky and
    // signal are told apart. Raising the luminance must not paint the sky.
    const int W = 1, H = 1;
    Slots slots;
    slots["Ha"]   = {0.012f};
    slots["OIII"] = {0.010f};      // total 0.022: barely above the gate's sky
    ColorComposer composer;
    composer.set_chroma_gate(0.02, 0.08);
    Image lum(W, H, 1);
    lum.at(0, 0, 0) = 0.12f;       // the auto-stretch's sky target
    const Image out = compose_slots_with_luminance(W, H, slots, composer, lum);
    const sRGBPixel p{out.at(0, 0, 0), out.at(0, 0, 1), out.at(0, 0, 2)};
    REQUIRE(composer.last_pixel_chroma_scale() < 0.05);
    REQUIRE(hsv_saturation(p) < 0.05);
    REQUIRE(p.g == Catch::Approx(0.12f).margin(0.01));
}

TEST_CASE("an external luminance moves the grey axis and nothing else", "[compose]") {
    // Broadband grey has no chroma; overriding the luminance must yield the
    // same grey at the new level, exactly as the transfer round trip promises.
    const int W = 1, H = 1;
    Slots slots;
    slots["R"] = {0.2f}; slots["G"] = {0.2f}; slots["B"] = {0.2f};
    ColorComposer composer;
    Image lum(W, H, 1);
    lum.at(0, 0, 0) = 0.6f;
    const Image out = compose_slots_with_luminance(W, H, slots, composer, lum);
    for (int c = 0; c < 3; c++)
        REQUIRE(out.at(0, 0, c) == Catch::Approx(0.6f).margin(1e-5));
}

TEST_CASE("compose_slots_with_luminance refuses a luminance of the wrong shape", "[compose]") {
    Slots slots;
    slots["Ha"] = {0.1f, 0.1f};
    ColorComposer composer;
    Image wrong(3, 1, 1);
    REQUIRE(compose_slots_with_luminance(2, 1, slots, composer, wrong).empty());
    Image colour(2, 1, 3);
    REQUIRE(compose_slots_with_luminance(2, 1, slots, composer, colour).empty());
}
