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

namespace {

// Resolve every slot pointer ONCE; shared by the three composers below.
struct SlotPlanes {
    const float *L = nullptr, *R = nullptr, *G = nullptr, *B = nullptr;
    const float *Ha = nullptr, *OIII = nullptr, *SII = nullptr;
    bool any() const { return L || R || G || B || Ha || OIII || SII; }
    DerivedSlots at(std::size_t p) const {
        DerivedSlots ds;
        if (L)    ds.L    = static_cast<double>(L[p]);
        if (R)    ds.R    = static_cast<double>(R[p]);
        if (G)    ds.G    = static_cast<double>(G[p]);
        if (B)    ds.B    = static_cast<double>(B[p]);
        if (Ha)   ds.Ha   = static_cast<double>(Ha[p]);
        if (OIII) ds.OIII = static_cast<double>(OIII[p]);
        if (SII)  ds.SII  = static_cast<double>(SII[p]);
        return ds;
    }
};

SlotPlanes resolve(const std::unordered_map<std::string, std::vector<float>>& slots,
                   std::size_t n) {
    auto plane = [&](const char* name) -> const float* {
        auto it = slots.find(name);
        if (it == slots.end() || it->second.size() < n) return nullptr;
        return it->second.data();
    };
    SlotPlanes sp;
    sp.L = plane("L"); sp.R = plane("R"); sp.G = plane("G"); sp.B = plane("B");
    sp.Ha = plane("Ha"); sp.OIII = plane("OIII"); sp.SII = plane("SII");
    return sp;
}

} // namespace

Image compose_luminance_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots) {
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || slots.empty() || n == 0) return Image{};
    const SlotPlanes sp = resolve(slots, n);
    if (!sp.any()) return Image{};
    Image out(width, height, 1);
    float* dst = out.channel_data(0);
    for (std::size_t p = 0; p < n; ++p)
        dst[p] = static_cast<float>(ColorComposer::luminance_of(sp.at(p)));
    return out;
}

Image compose_slots_with_luminance(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer,
    const Image& luminance) {
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || slots.empty() || n == 0) return Image{};
    if (luminance.empty() || luminance.n_channels() != 1 ||
        luminance.width() != width || luminance.height() != height) return Image{};
    const SlotPlanes sp = resolve(slots, n);
    if (!sp.any()) return Image{};

    Image out(width, height, 3);
    float* dst_r = out.channel_data(0);
    float* dst_g = out.channel_data(1);
    float* dst_b = out.channel_data(2);
    const float* lum = luminance.channel_data(0);
    for (std::size_t p = 0; p < n; ++p) {
        LabColor lab = composer.compose_lab(sp.at(p));
        lab.L = ColorComposer::lab_L_from_luminance(static_cast<double>(lum[p]));
        const sRGBPixel c = composer.map_to_srgb(lab);
        dst_r[p] = static_cast<float>(c.r);
        dst_g[p] = static_cast<float>(c.g);
        dst_b[p] = static_cast<float>(c.b);
    }
    return out;
}

} // namespace nukex
