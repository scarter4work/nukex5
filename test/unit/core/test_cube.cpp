#include "catch_amalgamated.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/filter.hpp"

using namespace nukex;

static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}

TEST_CASE("Cube: construction with dimensions", "[cube]") {
    auto cfg = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    Cube cube(100, 80, cfg);
    REQUIRE(cube.width == 100);
    REQUIRE(cube.height == 80);
    REQUIRE(cube.channel_config.n_channels == 4);
    REQUIRE(cube.n_frames_loaded == 0);
    REQUIRE(cube.total_pixels() == 8000);
}

TEST_CASE("Cube: voxel access by coordinates", "[cube]") {
    auto cfg = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(10, 10, cfg);
    cube.at(3, 5).n_frames = 42;
    cube.at(3, 5).confidence = 0.95f;
    REQUIRE(cube.at(3, 5).n_frames == 42);
    REQUIRE(cube.at(3, 5).confidence == Catch::Approx(0.95f));
    REQUIRE(cube.at(0, 0).n_frames == 0);
}

TEST_CASE("Cube: voxels initialized with correct channel count", "[cube]") {
    auto cfg = cfg_for(FilterClass::DUAL_NB_OSC, "HaO3");
    Cube cube(4, 4, cfg);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            REQUIRE(cube.at(x, y).n_channels == 3);
}

TEST_CASE("Cube: const access", "[cube]") {
    auto cfg = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    Cube cube(5, 5, cfg);
    cube.at(2, 3).confidence = 0.8f;
    const Cube& c = cube;
    REQUIRE(c.at(2, 3).confidence == Catch::Approx(0.8f));
}

TEST_CASE("Cube: is_valid_coord", "[cube]") {
    auto cfg = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(10, 8, cfg);
    REQUIRE(cube.is_valid_coord(0, 0) == true);
    REQUIRE(cube.is_valid_coord(9, 7) == true);
    REQUIRE(cube.is_valid_coord(10, 0) == false);
    REQUIRE(cube.is_valid_coord(0, 8) == false);
    REQUIRE(cube.is_valid_coord(-1, 0) == false);
}

// ── Runtime-sized allocation ─────────────────────────────────────────

TEST_CASE("Cube: per-voxel footprint scales with the channel count", "[cube]") {
    auto mono = cfg_for(FilterClass::BROADBAND_L, "L");
    auto osc  = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    Cube m(64, 64, mono);
    Cube o(64, 64, osc);

    REQUIRE(m.voxel_stride()
            == sizeof(SubcubeVoxel) + mono.n_channels * sizeof(VoxelChannel));
    REQUIRE(o.voxel_stride()
            == sizeof(SubcubeVoxel) + osc.n_channels * sizeof(VoxelChannel));
    REQUIRE(m.voxel_stride() < o.voxel_stride());
    REQUIRE(m.bytes_allocated() == m.voxel_stride() * 64 * 64);
}

TEST_CASE("Cube: a 1-channel cube does not pay for MAX_CHANNELS", "[cube]") {
    auto mono = cfg_for(FilterClass::BROADBAND_L, "L");
    REQUIRE(mono.n_channels == 1);
    Cube m(64, 64, mono);

    const size_t fixed_provisioning =
        (sizeof(SubcubeVoxel) + MAX_CHANNELS * sizeof(VoxelChannel))
        * size_t(64) * size_t(64);
    REQUIRE(m.bytes_allocated() < fixed_provisioning / 4);
}

TEST_CASE("Cube: each voxel's channels are private to that voxel", "[cube]") {
    // Stride arithmetic is manual, so a wrong stride would let one voxel's
    // last channel overlap the next voxel's first.
    auto cfg = cfg_for(FilterClass::DUAL_NB_OSC, "HaO3");
    REQUIRE(cfg.n_channels == 3);
    Cube cube(8, 4, cfg);

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            for (int c = 0; c < 3; c++)
                cube.at(x, y).channel(c).welford.update(
                    static_cast<float>((y * 8 + x) * 3 + c));

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            for (int c = 0; c < 3; c++) {
                INFO("voxel (" << x << "," << y << ") channel " << c);
                REQUIRE(cube.at(x, y).channel(c).welford.count() == 1);
                REQUIRE(cube.at(x, y).channel(c).welford.mean
                        == Catch::Approx(static_cast<float>((y * 8 + x) * 3 + c)));
            }
}

TEST_CASE("Cube: fixed voxel fields survive channel writes", "[cube]") {
    auto cfg = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    Cube cube(4, 4, cfg);
    cube.at(1, 1).confidence = 0.75f;
    cube.at(1, 1).n_frames   = 9;
    for (int c = 0; c < cfg.n_channels; c++)
        cube.at(2, 1).channel(c).snr = 12.0f;
    REQUIRE(cube.at(1, 1).confidence == Catch::Approx(0.75f));
    REQUIRE(cube.at(1, 1).n_frames == 9);
    REQUIRE(cube.at(1, 1).channel(0).snr == Catch::Approx(0.0f));
}
