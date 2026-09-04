#include "nukex/core/cube.hpp"

namespace nukex {

Cube::Cube(int w, int h, const ChannelConfig& config)
    : width(w)
    , height(h)
    , channel_config(config)
    , n_frames_loaded(0)
    , stride_(voxel_record_size(config.n_channels))
    // Default-init, not value-init: the bytes are about to be constructed over
    // and zeroing them first would be a second full pass over what can be tens
    // of gigabytes. std::make_unique would value-initialize, so `new` is used
    // directly here.
    , storage_(new std::byte[stride_ * static_cast<std::size_t>(w)
                                     * static_cast<std::size_t>(h)])
{
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; i++) {
        construct_voxel(storage_.get() + i * stride_, config.n_channels);
    }
}

} // namespace nukex
