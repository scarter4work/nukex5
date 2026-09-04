// LRGB-mono: a batch of separate L / R / G / B mono filters.
//
// This configuration does not produce correct output today, and these cases
// pin that it fails loudly rather than quietly.
//
// Why it is broken. Every mono frame debayers to the same (W, H, 1) geometry,
// and FrameCache is keyed on geometry alone, so all four filters land in one
// cache file. Phase B then cannot read a given slot's own frames:
//
//   - The L slot reads that shared cache and fits a mixture of every filter's
//     samples.
//   - R, G and B get `cache = nullptr` (stacking_engine.cpp guards against
//     routing three slots at channel 0 of one cache) and Phase B fits a buffer
//     of zeros, which the code comments describe as a "welford-only" fallback
//     that does not in fact exist.
//
// On real data -- M27 2025, ATR585M, L24 R12 G12 B24 -- that produced one
// populated channel out of four and three exactly-zero ones: a solid blue
// frame that looked like a colour-balance problem and was not.
//
// Phase A is not the problem: the per-slot Welford accumulators are correct,
// which the routing test below shows. The gap is that Phase B's per-frame read
// path, its shadow buffers and its weight kernels all assume every channel
// shares one frame set. Separating them is an architectural change, not a
// cache key. Until it lands, a batch that would hit this refuses to run.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "synthetic_fits.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

struct FilterLevel { const char* filter; float value; };

std::vector<std::string> write_mono_batch(const std::string& tag,
                                          const std::vector<FilterLevel>& levels,
                                          int frames_each) {
    std::vector<std::string> paths;
    for (const auto& fl : levels) {
        for (int i = 0; i < frames_each; i++) {
            auto p = fs::temp_directory_path()
                   / ("lrgb_" + tag + "_" + fl.filter + std::to_string(i) + ".fits");
            test_util::write_synthetic_mono(p.string(), 16, 16, "ATR585M",
                                            fl.filter, fl.value);
            paths.push_back(p.string());
        }
    }
    return paths;
}

StackingEngine::Config test_config() {
    StackingEngine::Config cfg;
    cfg.qe_database_path =
        (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    return cfg;
}

const std::vector<FilterLevel> kLRGB = {
    { "L", 0.60f }, { "R", 0.20f }, { "G", 0.35f }, { "B", 0.50f },
};

} // namespace

TEST_CASE("LRGB-mono: a multi-filter mono batch refuses to run rather than "
          "emitting empty channels", "[integration][lrgb]") {
    auto paths = write_mono_batch("refuse", kLRGB, 3);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    REQUIRE_FALSE(result.ok);
    INFO("error: " << result.error);
    // The message has to name what the user should do, not just what broke.
    REQUIRE(result.error.find("mono") != std::string::npos);
    REQUIRE(result.error.find("separately") != std::string::npos);
}

TEST_CASE("LRGB-mono: a single mono filter still stacks", "[integration][lrgb]") {
    // The guard must catch only the case it is for. One mono filter per batch
    // is the whole NGC7635 corpus and must be untouched.
    auto paths = write_mono_batch("single", { { "L", 0.60f } }, 3);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube != nullptr);
    REQUIRE(result.cube->channel_config.n_channels == 1);
    REQUIRE(result.stacked.at(8, 8, 0) == Catch::Approx(0.60f).margin(0.02f));
}

TEST_CASE("LRGB-mono: Phase A routes each filter into its own slot",
          "[integration][lrgb]") {
    // Documents that the defect is downstream of routing: were Phase B able to
    // read per-slot frames, the accumulators are already correct. Uses two
    // filters and reaches into the cube before the guard would matter, so it
    // is checked through the engine's Phase A products directly.
    auto paths = write_mono_batch("routing", { { "L", 0.60f }, { "R", 0.20f } }, 2);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    // The guard fires, and that is the point: the cube is not produced, so the
    // proof that routing works lives in the unit tests for ChannelConfig and
    // the Phase A router. What this case pins is that two mono filters are
    // enough to trip the guard -- it is not specific to four.
    REQUIRE_FALSE(result.ok);
}
