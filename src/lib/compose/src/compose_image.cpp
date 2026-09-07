#include "nukex/compose/compose_image.hpp"

namespace nukex {

bool slots_have_colour(
    const std::unordered_map<std::string, std::vector<float>>& slots) {
    for (const char* name : {"R", "G", "B", "Ha", "OIII", "SII"})
        if (slots.find(name) != slots.end()) return true;
    return false;
}

Image compose_slots_to_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer) {

    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || slots.empty() || n == 0) return Image{};

    // Resolve every slot pointer ONCE. Looking a name up per pixel would
    // re-hash the map seven times for each of 24 million pixels.
    auto plane = [&](const char* name) -> const float* {
        auto it = slots.find(name);
        if (it == slots.end() || it->second.size() < n) return nullptr;
        return it->second.data();
    };
    const float* L    = plane("L");
    const float* R    = plane("R");
    const float* G    = plane("G");
    const float* B    = plane("B");
    const float* Ha   = plane("Ha");
    const float* OIII = plane("OIII");
    const float* SII  = plane("SII");

    if (!L && !R && !G && !B && !Ha && !OIII && !SII) return Image{};

    Image out(width, height, 3);
    float* dst_r = out.channel_data(0);
    float* dst_g = out.channel_data(1);
    float* dst_b = out.channel_data(2);

    for (std::size_t p = 0; p < n; ++p) {
        DerivedSlots ds;
        if (L)    ds.L    = static_cast<double>(L[p]);
        if (R)    ds.R    = static_cast<double>(R[p]);
        if (G)    ds.G    = static_cast<double>(G[p]);
        if (B)    ds.B    = static_cast<double>(B[p]);
        if (Ha)   ds.Ha   = static_cast<double>(Ha[p]);
        if (OIII) ds.OIII = static_cast<double>(OIII[p]);
        if (SII)  ds.SII  = static_cast<double>(SII[p]);
        const sRGBPixel c = composer.compose_pixel(ds);
        dst_r[p] = static_cast<float>(c.r);
        dst_g[p] = static_cast<float>(c.g);
        dst_b[p] = static_cast<float>(c.b);
    }
    return out;
}

} // namespace nukex
