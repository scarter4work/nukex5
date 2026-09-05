#include "catch_amalgamated.hpp"
#include "nukex/gpu/gpu_context.hpp"
#include <iostream>

using namespace nukex;

TEST_CASE("GPU: enumerate devices finds at least one GPU", "[gpu]") {
    auto devices = GPUContext::enumerate_devices();
    std::cout << "\nFound " << devices.size() << " GPU device(s):\n";
    for (const auto& d : devices) {
        std::cout << "  " << d.name << " (" << d.vendor << ")"
                  << " VRAM=" << (d.global_mem_bytes / (1024*1024)) << "MB"
                  << " CUs=" << d.max_compute_units
                  << " WG=" << d.max_work_group_size
                  << " fp64=" << d.supports_double << "\n";
    }
    REQUIRE(devices.size() >= 1);
}

TEST_CASE("GPU: create context succeeds with auto-select", "[gpu]") {
    auto ctx = GPUContext::create();
    std::cout << "Backend: " << (ctx.is_gpu_available() ? "OPENCL" : "CPU_FALLBACK") << "\n";
    if (ctx.is_gpu_available()) {
        std::cout << "Device: " << ctx.device_info().name << "\n";
        std::cout << "VRAM: " << (ctx.device_info().global_mem_bytes / (1024*1024)) << " MB\n";
    }
    REQUIRE(ctx.is_gpu_available());
}

TEST_CASE("GPU: force CPU fallback", "[gpu]") {
    GPUExecutorConfig config;
    config.force_cpu_fallback = true;
    auto ctx = GPUContext::create(config);
    REQUIRE(!ctx.is_gpu_available());
    REQUIRE(ctx.backend() == GPUBackend::CPU_FALLBACK);
}

TEST_CASE("GPU: batch size estimation is reasonable", "[gpu]") {
    auto ctx = GPUContext::create();
    int batch = ctx.estimate_batch_size(100, 3);  // 100 frames, 3 channels
    std::cout << "Estimated batch size (100 frames, 3 ch): " << batch << " voxels\n";
    // With 16 GB VRAM and ~2.5 KB per voxel, should be millions
    if (ctx.is_gpu_available()) {
        REQUIRE(batch > 100000);
    }
    // CPU fallback with 2 GB should still be substantial
    REQUIRE(batch > 1000);
}

TEST_CASE("GPUContext: host memory is readable on this platform", "[gpu][context]") {
    // The cap is only real if the budget can actually be measured. If this
    // ever returns 0 the batch silently reverts to the VRAM-only figure,
    // which is the behaviour that sent a 30 GB box into swap.
    const std::size_t avail = GPUContext::host_available_bytes();
#if defined(__linux__)
    REQUIRE(avail > 0);
#else
    SUCCEED("host_available_bytes not implemented for this platform");
#endif
}

TEST_CASE("GPUContext: the batch is capped by host memory, not just VRAM",
          "[gpu][context]") {
    GPUContext ctx = GPUContext::create({});

    // A deliberately small host budget must produce a smaller batch than a
    // large one, whatever the device reports for VRAM.
    ctx.set_host_memory_budget(64ull * 1024 * 1024);      // 64 MB
    const int small = ctx.estimate_batch_size(30, 3);

    ctx.set_host_memory_budget(16ull * 1024 * 1024 * 1024); // 16 GB
    const int large = ctx.estimate_batch_size(30, 3);

    INFO("small=" << small << " large=" << large);
    REQUIRE(small >= 1);
    REQUIRE(small < large);

    // And the cap must actually bind: 64 MB of shadow buffers for 30 frames
    // and 3 channels cannot hold anywhere near a million voxels.
    REQUIRE(small < 1000000);
}

TEST_CASE("GPUContext: a zero budget restores the measured default",
          "[gpu][context]") {
    GPUContext ctx = GPUContext::create({});
    ctx.set_host_memory_budget(64ull * 1024 * 1024);
    const int capped = ctx.estimate_batch_size(30, 3);
    ctx.set_host_memory_budget(0);
    const int measured = ctx.estimate_batch_size(30, 3);
    REQUIRE(ctx.host_memory_budget() == 0);
    REQUIRE(measured > capped);
}
