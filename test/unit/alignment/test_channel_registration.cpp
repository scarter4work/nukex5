#include "catch_amalgamated.hpp"
#include "nukex/alignment/channel_registration.hpp"

#include <cmath>
#include <vector>

using namespace nukex;

namespace {

// A Gaussian star drawn at a sub-pixel position. Amplitude and sigma are
// fixed; what the tests vary is where it lands.
void draw_star(Image& img, int ch, double x, double y,
               double amplitude = 0.5, double sigma = 1.6) {
    const int r = 6;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int px = ix + dx, py = iy + dy;
            if (px < 0 || px >= img.width() || py < 0 || py >= img.height())
                continue;
            double ex = px - x, ey = py - y;
            img.at(px, py, ch) += static_cast<float>(
                amplitude * std::exp(-(ex * ex + ey * ey) / (2.0 * sigma * sigma)));
        }
    }
}

// A grid of star positions well inside the frame, avoiding the border so the
// centroid box always fits.
std::vector<std::pair<double, double>> star_grid(int w, int h, int n_side) {
    std::vector<std::pair<double, double>> out;
    for (int j = 0; j < n_side; j++)
        for (int i = 0; i < n_side; i++) {
            // The offsets keep centroids off exact integers, which is where a
            // broken centroid estimator would accidentally look correct.
            out.emplace_back(20.0 + 0.37 + i * (w - 40.0) / (n_side - 1),
                             20.0 + 0.61 + j * (h - 40.0) / (n_side - 1));
        }
    return out;
}

struct Synth {
    Image       image;
    StarCatalog catalog;
};

// Builds a 3-channel frame. Green carries stars at `pos`; red carries the same
// stars displaced by (s, tx, ty) about the image centre; blue matches green.
Synth make_frame(int w, int h, double s, double tx, double ty) {
    Synth out;
    out.image = Image(w, h, 3);
    out.image.fill(0.002f);

    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

    for (auto [x, y] : star_grid(w, h, 5)) {
        draw_star(out.image, 1, x, y);                 // green: reference
        draw_star(out.image, 2, x, y);                 // blue: agrees with green
        draw_star(out.image, 0,                        // red: displaced
                  s * (x - cx) + tx + cx,
                  s * (y - cy) + ty + cy);

        Star st;
        st.x = static_cast<float>(x);
        st.y = static_cast<float>(y);
        st.flux = 1.0f;
        out.catalog.stars.push_back(st);
    }
    return out;
}

} // namespace

TEST_CASE("measure_channel_transforms recovers a known scale and shift",
          "[channel_registration]") {
    // +500 ppm and a third of a pixel. At the corner of a 800x800 frame the
    // scale term alone is 500e-6 * 565 = 0.28 px, so both terms matter.
    const double s = 1.0005, tx = 0.33, ty = -0.21;
    Synth f = make_frame(800, 800, s, tx, ty);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    REQUIRE(ct.per_channel.size() == 3);
    REQUIRE(ct.reference_channel == 1);

    const ChannelTransform& red = ct.per_channel[0];
    REQUIRE(red.fit == ChannelTransform::Fit::Affine);
    REQUIRE(red.n_stars >= 20);

    // The acceptance the spec asks for is positional, so assert positionally:
    // the modelled position must land within 0.02 px of the truth at the
    // most demanding place, which is the field corner.
    const double R = std::hypot(399.5, 399.5);
    CHECK(std::abs(red.s - s) * R < 0.02);
    CHECK(std::abs(red.tx - tx) < 0.02);
    CHECK(std::abs(red.ty - ty) < 0.02);
    CHECK(red.residual < 0.02);
}

TEST_CASE("the reference channel is exactly identity",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0005, 0.33, -0.21);
    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    const ChannelTransform& green = ct.per_channel[1];
    REQUIRE(green.fit == ChannelTransform::Fit::Identity);
    REQUIRE(green.is_identity());
    REQUIRE(green.s == 1.0);
    REQUIRE(green.tx == 0.0);
    REQUIRE(green.ty == 0.0);
}

TEST_CASE("a channel that already agrees measures as near-identity",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0005, 0.33, -0.21);
    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    // Blue was drawn at the same positions as green. Its fit is not forced to
    // identity -- it is measured -- so it must come out small, not zero.
    const ChannelTransform& blue = ct.per_channel[2];
    const double R = std::hypot(399.5, 399.5);
    CHECK(std::abs(blue.s - 1.0) * R < 0.02);
    CHECK(std::abs(blue.tx) < 0.02);
    CHECK(std::abs(blue.ty) < 0.02);
}

TEST_CASE("a single-channel image registers nothing",
          "[channel_registration]") {
    Image mono(200, 200, 1);
    mono.fill(0.002f);
    StarCatalog cat;
    for (auto [x, y] : star_grid(200, 200, 4)) {
        draw_star(mono, 0, x, y);
        Star st; st.x = float(x); st.y = float(y); cat.stars.push_back(st);
    }

    ChannelTransforms ct = measure_channel_transforms(mono, cat, 0);
    REQUIRE(ct.empty());
}

TEST_CASE("negligible() is true only when every channel is near identity "
          "at the given radius", "[channel_registration]") {
    ChannelTransforms ct;
    ct.cx = 400; ct.cy = 400; ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[1].fit = ChannelTransform::Fit::Identity;

    // 1e-6 of scale over 565 px is 0.00056 px -- below any threshold worth
    // resampling for.
    ct.per_channel[0] = ChannelTransform{1.000001, 0.0, 0.0, 50, 0.01,
                                         ChannelTransform::Fit::Affine};
    CHECK(ct.negligible(565.0, 0.01));

    // 0.05 px of pure translation is not negligible at any radius.
    ct.per_channel[0].s  = 1.0;
    ct.per_channel[0].tx = 0.05;
    CHECK_FALSE(ct.negligible(565.0, 0.01));

    // 100 ppm is 0.056 px at the corner: negligible near the centre, not at
    // the edge. The radius argument is what makes that distinction.
    ct.per_channel[0].tx = 0.0;
    ct.per_channel[0].s  = 1.0001;
    CHECK_FALSE(ct.negligible(565.0, 0.01));
    CHECK(ct.negligible(50.0, 0.01));
}
