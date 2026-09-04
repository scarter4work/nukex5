#pragma once

#include "nukex/core/types.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/voxel.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace nukex {

/// The full-frame voxel record, one SubcubeVoxel per (x, y).
///
/// Records are runtime-sized: each is `voxel_record_size(n_channels)` bytes and
/// they are laid end to end in a single allocation, so a 1-channel stack costs
/// a fraction of what an 8-channel one does. The cube previously held a
/// std::vector<SubcubeVoxel> whose element carried MAX_CHANNELS worth of
/// per-channel arrays regardless of the stack.
class Cube {
public:
    int           width  = 0;
    int           height = 0;
    ChannelConfig channel_config;
    int           n_frames_loaded = 0;

    Cube(int w, int h, const ChannelConfig& config);
    Cube() = default;

    SubcubeVoxel& at(int x, int y) {
        return *std::launder(reinterpret_cast<SubcubeVoxel*>(
            storage_.get() + offset_of(x, y)));
    }
    const SubcubeVoxel& at(int x, int y) const {
        return *std::launder(reinterpret_cast<const SubcubeVoxel*>(
            storage_.get() + offset_of(x, y)));
    }

    int total_pixels() const { return width * height; }

    /// Channels every voxel was allocated with. channel_config may be
    /// re-merged during Phase A; it must never exceed this, because the
    /// per-voxel records are sized against it and a larger slot index writes
    /// past the allocation.
    int allocated_channels() const { return allocated_channels_; }

    /// Bytes one voxel occupies, including its trailing channel records.
    std::size_t voxel_stride() const { return stride_; }

    /// Total bytes held by the voxel record. Worth logging before a stack:
    /// this is the number that decides whether the run fits in RAM.
    std::size_t bytes_allocated() const {
        return stride_ * static_cast<std::size_t>(width)
                       * static_cast<std::size_t>(height);
    }

    bool is_valid_coord(int x, int y) const {
        return x >= 0 && x < width && y >= 0 && y < height;
    }

private:
    std::size_t offset_of(int x, int y) const {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x)) * stride_;
    }

    int                             allocated_channels_ = 0;
    std::size_t                     stride_ = 0;
    std::unique_ptr<std::byte[]>    storage_;
};

} // namespace nukex
