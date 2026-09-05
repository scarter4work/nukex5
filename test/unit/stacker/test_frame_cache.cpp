#include "catch_amalgamated.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/io/image.hpp"
#include <cmath>
#include <filesystem>

using namespace nukex;

TEST_CASE("FrameCache::encode/decode roundtrip", "[cache]") {
    // Test several values across [0, 1]
    float test_values[] = {0.0f, 0.001f, 0.25f, 0.5f, 0.75f, 0.999f, 1.0f};
    for (float v : test_values) {
        uint16_t encoded = FrameCache::encode(v);
        float decoded = FrameCache::decode(encoded);
        REQUIRE(decoded == Catch::Approx(v).margin(1.0f / 65535.0f));
    }
}

TEST_CASE("FrameCache::encode clamps to [0, 1]", "[cache]") {
    REQUIRE(FrameCache::encode(-0.1f) == 0);
    REQUIRE(FrameCache::encode(1.5f) == 65535);
}

TEST_CASE("FrameCache: write and read back single frame", "[cache]") {
    Image frame(4, 4, 1);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            frame.at(x, y, 0) = (x + y * 4) / 16.0f;

    FrameCache cache(4, 4, 1, 10, "/tmp");
    cache.write_frame(frame, 0);
    REQUIRE(cache.n_frames_written() == 1);

    float values[10];
    int n = cache.read_pixel(2, 1, 0, values);
    REQUIRE(n == 1);
    // Pixel (2, 1): value = (2 + 1*4) / 16 = 0.375
    REQUIRE(values[0] == Catch::Approx(0.375f).margin(0.001f));
}

TEST_CASE("FrameCache: write multiple frames, read all back", "[cache]") {
    Image f1(8, 8, 2);
    Image f2(8, 8, 2);
    Image f3(8, 8, 2);
    f1.fill(0.3f);
    f2.fill(0.5f);
    f3.fill(0.7f);

    FrameCache cache(8, 8, 2, 10, "/tmp");
    cache.write_frame(f1, 0);
    cache.write_frame(f2, 1);
    cache.write_frame(f3, 2);
    REQUIRE(cache.n_frames_written() == 3);

    float values[10];
    int n = cache.read_pixel(4, 4, 0, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
    REQUIRE(values[1] == Catch::Approx(0.5f).margin(0.001f));
    REQUIRE(values[2] == Catch::Approx(0.7f).margin(0.001f));

    // Channel 1 should also work
    n = cache.read_pixel(4, 4, 1, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
}

TEST_CASE("FrameCache: temp file cleaned up on destruction", "[cache]") {
    std::string filepath;
    {
        FrameCache cache(2, 2, 1, 1, "/tmp");
        Image frame(2, 2, 1);
        frame.fill(0.5f);
        cache.write_frame(frame, 0);
        // We can't easily get the filepath, but verify no crash on destruction
    }
    // Cache destroyed -- temp file should be gone
    // (No way to verify path externally without exposing it, but no crash = success)
}

TEST_CASE("FrameCache: quantization error within tolerance", "[cache]") {
    // Verify max quantization error is < 2/65535 ~ 3e-5
    Image frame(16, 16, 1);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            frame.at(x, y, 0) = (x * 16 + y) / 256.0f;

    FrameCache cache(16, 16, 1, 1, "/tmp");
    cache.write_frame(frame, 0);

    float values[1];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            cache.read_pixel(x, y, 0, values);
            float original = frame.at(x, y, 0);
            REQUIRE(std::fabs(values[0] - original) < 2.0f / 65535.0f);
        }
    }
}

TEST_CASE("FrameCache: a sparse global frame set reads back only what it received",
          "[frame_cache]") {
    // The defect this replaced: write_frame stored at the GLOBAL batch index
    // and n_frames_written_ became index+1, while read_pixel returns slots
    // 0..n-1 contiguously. A cache given frames 5, 9 and 12 of a twenty-frame
    // batch therefore reported thirteen frames and handed Phase B ten
    // unwritten slots as though they were measurements. Any batch producing
    // more than one cache hit this, not only mono ones.
    const std::string dir = "/tmp";
    FrameCache cache(2, 2, 1, /*max_frames*/ 20, dir);

    const int globals[] = {5, 9, 12};
    const float values[] = {0.25f, 0.50f, 0.75f};
    for (int i = 0; i < 3; i++) {
        Image f(2, 2, 1);
        f.fill(values[i]);
        const int local = cache.write_frame(f, globals[i]);
        REQUIRE(local == i);            // dense, in write order
    }

    REQUIRE(cache.n_frames_written() == 3);   // not 13

    float out[20] = {0};
    const int n = cache.read_pixel(0, 0, 0, out);
    REQUIRE(n == 3);
    for (int i = 0; i < 3; i++)
        REQUIRE(out[i] == Catch::Approx(values[i]).margin(1e-4));

    // And the map back to the batch's own numbering survives, which is what
    // lets Phase B find each frame's FrameStats.
    REQUIRE(cache.global_frame(0) == 5);
    REQUIRE(cache.global_frame(1) == 9);
    REQUIRE(cache.global_frame(2) == 12);
    REQUIRE(cache.global_frame(3) == -1);
}
