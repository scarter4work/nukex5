#pragma once

#include "nukex/io/image.hpp"
#include <string>
#include <cstdint>
#include <atomic>
#include <vector>

namespace nukex {

/// Disk-backed storage for aligned frames using memory-mapped uint16 encoding.
///
/// Pixel-major layout: read_pixel(x,y,ch) returns N contiguous uint16 values,
/// one per frame, for sequential disk reads during Phase B.
///
/// Encoding: float [0,1] -> uint16 via round(value * 65535)
/// Decoding: uint16 -> float via stored * (1.0f / 65535.0f)
/// Quantization error: +/-7.6e-6 (100x below noise floor).
///
/// The temp file is deleted when the FrameCache is destroyed.
class FrameCache {
public:
    /// Create a cache file in cache_dir. Pre-allocates for max_frames frames.
    FrameCache(int width, int height, int n_channels,
               int max_frames, const std::string& cache_dir);

    /// Unmaps and deletes the temp file.
    ~FrameCache();

    // Non-copyable, movable
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;
    FrameCache(FrameCache&& other) noexcept;
    FrameCache& operator=(FrameCache&& other) noexcept;

    /// Phase A: Append one aligned frame at the next LOCAL slot.
    ///
    /// Returns the local slot it was written to. `global_index` is the
    /// frame's number within the whole batch, recorded so Phase B can
    /// recover its FrameStats -- those are indexed globally, while every
    /// pixel read here is indexed locally.
    ///
    /// Local slots are dense by construction. They used to be the global
    /// index, which made read_pixel's contiguous read wrong for any cache
    /// that received a subset of the batch: a cache given frames 5, 9 and 12
    /// of twenty reported thirteen frames and handed Phase B ten unwritten
    /// slots as though they were measurements.
    int write_frame(const Image& aligned, int global_index);

    /// The batch-global frame number stored at `local`. -1 if out of range.
    int global_frame(int local) const {
        return (local >= 0 && local < static_cast<int>(frame_map_.size()))
             ? frame_map_[local] : -1;
    }

    /// Phase B: Read all frame values at one pixel/channel, decoded to float.
    /// out_values must have space for at least n_frames_ floats.
    /// Returns number of frames written so far.
    int read_pixel(int x, int y, int ch, float* out_values) const;

    /// Number of frames written so far.
    int n_frames_written() const { return n_frames_written_.load(std::memory_order_relaxed); }

    int width() const { return width_; }
    int height() const { return height_; }
    int n_channels() const { return n_channels_; }
    int max_frames() const { return max_frames_; }

    /// Encode float to uint16.
    static uint16_t encode(float value);

    /// Decode uint16 to float.
    static float decode(uint16_t stored);

private:
    int fd_ = -1;
    uint16_t* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    std::string filepath_;

    int width_ = 0;
    int height_ = 0;
    int n_channels_ = 0;
    int max_frames_ = 0;
    std::atomic<int> n_frames_written_{0};
    std::vector<int> frame_map_;   ///< local slot -> batch-global frame index

    /// Element offset for pixel (x, y), channel ch, frame f.
    size_t offset(int x, int y, int ch, int f) const {
        return static_cast<size_t>(
            ((static_cast<int64_t>(y) * width_ + x) * n_channels_ + ch)
            * max_frames_ + f);
    }

    void cleanup();
};

} // namespace nukex
