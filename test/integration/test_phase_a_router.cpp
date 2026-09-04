// Phase A router integration tests (Task 9).
//
// These five [.integration]-tagged cases describe the END-TO-END
// behaviour of the new FilterClassifier-driven Phase A pipeline:
//
//   1. BROADBAND_OSC frame synthesises an "L" slot via rec709 luminance.
//   2. DUAL_NB_OSC HaO3 frame routes into R_HaO3 / G_HaO3 / B_HaO3.
//   3. UNKNOWN FILTER on a Bayer frame fails the batch loud at start.
//   4. Missing FILTER on a Bayer frame is silently treated as OSC.
//   5. Mixed L (mono) + HaO3 (Bayer) batch builds a union slot config.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "synthetic_fits.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("Phase A: BROADBAND_OSC frame synthesizes L slot",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_osc.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, /*progress*/nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("R") != -1);
    REQUIRE(result.cube->channel_config.slot_index("G") != -1);
    REQUIRE(result.cube->channel_config.slot_index("B") != -1);
    REQUIRE(result.cube->channel_config.slot_index("L") != -1);
    int  L_idx = result.cube->channel_config.slot_index("L");
    auto& px   = result.cube->at(8, 8);
    REQUIRE(px.channel(L_idx).welford.mean == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Phase A: HaO3 dual-NB frame routes into R_HaO3/G_HaO3/B_HaO3",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_hao3.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("R_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("G_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("B_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("R") == -1);
}

TEST_CASE("Phase A: unknown FILTER on Bayer fails the batch loud at start",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_unknown.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "ALP-T-fake-2026", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("ALP-T-fake-2026") != std::string::npos);
    REQUIRE(result.error.find("qe_overrides.json") != std::string::npos);
}

TEST_CASE("Phase A: missing FILTER on Bayer is silent BROADBAND_OSC",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_no_filter.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("L") != -1);
}

TEST_CASE("Phase A: mixed L + HaO3 batch builds union slot config",
          "[.integration][phase_a]") {
    auto t1 = fs::temp_directory_path() / "phase_a_l.fits";
    auto t2 = fs::temp_directory_path() / "phase_a_hao3_2.fits";
    test_util::write_synthetic_mono(t1.string(), 16, 16, "ASI2600MM", "L", 0.5f);
    test_util::write_synthetic_bayer(t2.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({t1.string(), t2.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("L")      != -1);
    REQUIRE(result.cube->channel_config.slot_index("R_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("G_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("B_HaO3") != -1);
}

TEST_CASE("Phase A: missing FILTER on mono (L_unnamed) routes into the L slot",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_mono_unnamed.fits";
    test_util::write_synthetic_mono(tmp.string(), 16, 16, "ASI2600MM", /*filter*/"", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    int L_idx = result.cube->channel_config.slot_index("L");
    REQUIRE(L_idx != -1);
    REQUIRE(result.cube->at(8, 8).channel(L_idx).welford.mean == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Phase A: unknown FILTER on mono routes into the L slot (spec 6.3, e.g. a wheel slot number)",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_mono_unknown.fits";
    test_util::write_synthetic_mono(tmp.string(), 16, 16, "ATR585M", /*filter*/"1", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    int L_idx = result.cube->channel_config.slot_index("L");
    REQUIRE(L_idx != -1);
    REQUIRE(result.cube->at(8, 8).channel(L_idx).welford.mean == Catch::Approx(0.5f).margin(0.05f));
}
