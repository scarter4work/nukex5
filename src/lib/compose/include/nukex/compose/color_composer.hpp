#ifndef NUKEX_COMPOSE_COLOR_COMPOSER_HPP
#define NUKEX_COMPOSE_COLOR_COMPOSER_HPP

#include "nukex/compose/palette.hpp"

#include <cstdint>

namespace nukex {

// All slot values are sRGB-encoded intensities in [0, 1]. Producers
// (Phase B derived-slot accumulator) are responsible for staying in
// range; negative values fall through compose_pixel's gamut counter
// and are silently clipped to 0.
struct DerivedSlots {
    // Broadband channels (any may be zero if not in batch)
    double L = 0.0;
    double R = 0.0;
    double G = 0.0;
    double B = 0.0;
    // Emission-line channels (any may be zero if not in batch)
    double Ha   = 0.0;
    double OIII = 0.0;
    double SII  = 0.0;
};

struct sRGBPixel {
    double r = 0.0, g = 0.0, b = 0.0;
};

struct ContinuumK {
    double k_Ha   = 0.0;
    double k_OIII = 0.0;
    double k_SII  = 0.0;
};

class ColorComposer {
public:
    enum class Mode { LAB_LCH_DEFAULT, CONTINUUM_SUBTRACT };

    ColorComposer() = default;

    void set_mode(Mode m)                                { mode_ = m; }
    void set_continuum_coefficients(const ContinuumK& k) { continuum_ = k; }

    sRGBPixel compose_pixel(const DerivedSlots& s);

    // Chroma gate. Normalising the emission chrominance by the total
    // emission weight makes hue a pure line ratio (Lupton et al. 2004,
    // PASP 116, 133), but it also means an arbitrarily faint pixel
    // normalises to the full palette vector. This gate ramps chrominance
    // linearly to zero below `snr_floor`, so noise is not rendered as
    // saturated colour -- the deliberate version of the behaviour the
    // missing division used to provide by accident.
    //
    // Two parameters, because the derived slots carry a sky pedestal: a
    // ramp from zero would leave the background almost fully saturated.
    // `background` is the total emission weight of blank sky and
    // `full_scale` the weight at which chrominance reaches full strength.
    // Set both from the stack's own measured statistics, never from a
    // constant. full_scale <= background disables the gate.
    void set_chroma_gate(double background, double full_scale) {
        gate_background_ = background;
        gate_full_scale_ = full_scale;
    }
    double gate_background() const { return gate_background_; }
    double gate_full_scale() const { return gate_full_scale_; }

    // Test seam: gamut handling is a pure function of its arguments.
    bool clip_to_gamut_for_test(double& r, double& g, double& b) {
        return clip_to_gamut(r, g, b);
    }

    // Stats
    std::int64_t gamut_clipped_count() const { return gamut_clipped_; }

    // Diagnostics for tests
    double last_pixel_emission_a() const { return last_emission_a_; }
    double last_pixel_emission_b() const { return last_emission_b_; }
    double last_pixel_chroma_scale() const { return last_chroma_scale_; }

private:
    Mode         mode_      = Mode::LAB_LCH_DEFAULT;
    ContinuumK   continuum_ = {};
    std::int64_t gamut_clipped_ = 0;
    double       last_emission_a_ = 0.0;
    double       last_emission_b_ = 0.0;
    double       gate_background_ = 0.0;
    double       gate_full_scale_ = 0.0;
    double       last_chroma_scale_ = 0.0;

    static LabColor    rgb_to_lab(double r, double g, double b);
    static sRGBPixel   lab_to_srgb(const LabColor& lab);
    static double      signal_weight(double v);
    bool               clip_to_gamut(double& r, double& g, double& b);
};

} // namespace nukex

#endif
