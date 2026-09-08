#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/coverage_mask.hpp"
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
    /// `coverage` says which pixels of `aligned` actually carry source data.
    /// An empty mask means the frame was cloned rather than warped and covers
    /// itself completely.
    ///
    /// Coverage has to live in the cache, not only in the Phase A
    /// accumulators: `stacked` is produced by Phase B, which reads its
    /// per-frame samples back from here. Storing the warped zeros without
    /// saying they are absent is what left a rim at 94% of interior
    /// brightness even after the accumulators were guarded.
    int write_frame(const Image& aligned, int global_index,
                    const CoverageMask& coverage = CoverageMask{});

    /// The batch-global frame number stored at `local`. -1 if out of range.
    int global_frame(int local) const {
        return (local >= 0 && local < static_cast<int>(frame_map_.size()))
             ? frame_map_[local] : -1;
    }

    /// Phase B: Read all frame values at one pixel/channel, decoded to float.
    /// out_values must have space for at least n_frames_ floats.
    /// Returns number of frames written so far.
    int read_pixel(int x, int y, int ch, float* out_values) const;

    /// As read_pixel, and additionally reports which of those frames actually
    /// covered this pixel. `out_valid[f]` is 1 when frame slot f has real
    /// data here, 0 when the warp left it outside the source.
    int read_pixel(int x, int y, int ch, float* out_values,
                   std::uint8_t* out_valid) const;

    /// Phase B: read one frame slot's channel over a contiguous pixel run.
    ///
    /// Pixels are addressed in raster order: `start_pixel` is `y*width + x`,
    /// and `out_values[i]` receives pixel `start_pixel + i` of frame slot `f`.
    /// `out_valid` may be null; when given it reports, per pixel, whether that
    /// frame actually covered it.
    ///
    /// This is the granularity Phase B batches at, and the granularity the
    /// storage layout is built for. Reading one pixel across all frames --
    /// what read_pixel did -- is the access pattern a frame-major layout is
    /// worst at, and it is why that method no longer exists.
    ///
    /// Returns false and writes NOTHING if `f` is not a written slot or the
    /// range leaves the image. Refusing totally rather than partially matters:
    /// a caller handed half a row would fit a distribution to whatever was
    /// left in its buffer.
    bool read_frame_range(int f, int start_pixel, int count, int ch,
                          float* out_values,
                          std::uint8_t* out_valid = nullptr) const;

    /// Number of frames written so far.
    int n_frames_written() const { return n_frames_written_.load(std::memory_order_relaxed); }

    int width() const { return width_; }
    int height() const { return height_; }
    int n_channels() const { return n_channels_; }
    int max_frames() const { return max_frames_; }

    /// Schedule writeback of everything written so far.
    ///
    /// Call at the end of Phase A. Not needed for correctness -- an mmap is
    /// coherent within the process, so Phase B sees every write regardless --
    /// only to bound how many dirty pages the machine is holding.
    void flush();

    /// How many times writeback has been scheduled. Diagnostic: with a
    /// pixel-major layout each frame dirties the whole mapping, so this
    /// number multiplied by the cache size is roughly the volume written.
    int sync_count() const { return sync_count_; }

    /// Encode float to uint16.
    static uint16_t encode(float value);

    /// Decode uint16 to float.
    static float decode(uint16_t stored);

private:
    int fd_ = -1;
    uint16_t* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    /// Coverage bitplane, one bit per (pixel, channel, frame), laid out with
    /// the same index as offset(). Lives after the value region in the same
    /// mapping.
    std::uint8_t* coverage_bits_ = nullptr;
    size_t coverage_bytes_ = 0;
    std::string filepath_;

    int width_ = 0;
    int height_ = 0;
    int n_channels_ = 0;
    int max_frames_ = 0;
    int sync_count_ = 0;
    /// Frames between scheduled writebacks. See flush() and write_frame().
    static constexpr int kSyncEveryNFrames = 8;
    std::atomic<int> n_frames_written_{0};
    std::vector<int> frame_map_;   ///< local slot -> batch-global frame index

    /// Element offset for pixel (x, y), channel ch, frame f.
    size_t offset(int x, int y, int ch, int f) const {
        return static_cast<size_t>(
            ((static_cast<int64_t>(y) * width_ + x) * n_channels_ + ch)
            * max_frames_ + f);
    }

    /// Bit index of (x, y, ch, f) in the coverage plane -- same ordering as
    /// offset(), so the two stay in step by construction.
    size_t cov_bit(int x, int y, int ch, int f) const { return offset(x, y, ch, f); }

    void cleanup();
};

} // namespace nukex
