#include "nukex/compose/color_composer.hpp"

#include <algorithm>
#include <cmath>

namespace nukex {

namespace {

// sRGB transfer functions (IEC 61966-2-1)
double srgb_to_linear(double v) {
    return (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double linear_to_srgb(double v) {
    return (v <= 0.0031308) ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// D65 white point in XYZ
constexpr double Xn = 0.95047, Yn = 1.0, Zn = 1.08883;

// CIE Lab f / f^-1
double f_lab(double t) {
    return (t > 0.008856) ? std::cbrt(t) : (7.787 * t + 16.0 / 116.0);
}
double f_lab_inv(double t) {
    double t3 = t * t * t;
    return (t3 > 0.008856) ? t3 : (t - 16.0 / 116.0) / 7.787;
}

void srgb_to_xyz(double r, double g, double b, double& X, double& Y, double& Z) {
    double rl = srgb_to_linear(r), gl = srgb_to_linear(g), bl = srgb_to_linear(b);
    X = 0.4124564 * rl + 0.3575761 * gl + 0.1804375 * bl;
    Y = 0.2126729 * rl + 0.7151522 * gl + 0.0721750 * bl;
    Z = 0.0193339 * rl + 0.1191920 * gl + 0.9503041 * bl;
}
void xyz_to_srgb(double X, double Y, double Z, double& r, double& g, double& b) {
    double rl =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double gl = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double bl =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    // Don't clamp negatives here — let clip_to_gamut see them so the
    // gamut_clipped_ counter records under-saturation as well as over.
    r = linear_to_srgb(rl);
    g = linear_to_srgb(gl);
    b = linear_to_srgb(bl);
}

} // namespace

LabColor ColorComposer::rgb_to_lab(double r, double g, double b) {
    double X, Y, Z;
    srgb_to_xyz(r, g, b, X, Y, Z);
    double fx = f_lab(X / Xn), fy = f_lab(Y / Yn), fz = f_lab(Z / Zn);
    return { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

sRGBPixel ColorComposer::lab_to_srgb(const LabColor& lab) {
    double fy = (lab.L + 16.0) / 116.0;
    double fx = fy + lab.a / 500.0;
    double fz = fy - lab.b / 200.0;
    double X = Xn * f_lab_inv(fx);
    double Y = Yn * f_lab_inv(fy);
    double Z = Zn * f_lab_inv(fz);
    sRGBPixel out;
    xyz_to_srgb(X, Y, Z, out.r, out.g, out.b);
    return out;
}

double ColorComposer::signal_weight(double v) {
    return std::max(0.0, v); // simple linear weight; clamp negatives
}

bool ColorComposer::clip_to_gamut(double& r, double& g, double& b) {
    bool clipped = false;

    // Negative energy is not a hue, it is an absence: floor it first.
    if (r < 0.0) { r = 0.0; clipped = true; }
    if (g < 0.0) { g = 0.0; clipped = true; }
    if (b < 0.0) { b = 0.0; clipped = true; }

    // Lupton et al. 2004, PASP 116, 133: rescale all three channels by
    // their common maximum, so the intensity is clipped at unity while the
    // colour stays correct. Clamping each channel independently changes the
    // hue of every over-bright pixel, which on a narrowband stack is most
    // of the frame.
    const double m = std::max(r, std::max(g, b));
    if (m > 1.0) {
        const double inv = 1.0 / m;
        r *= inv; g *= inv; b *= inv;
        clipped = true;
    }
    return clipped;
}

sRGBPixel ColorComposer::compose_pixel(const DerivedSlots& s) {
    // 1) L_broadband: native L, or rec709 from RGB if no L
    double L_broad = (s.L > 0.0) ? s.L
                                 : (0.299 * s.R + 0.587 * s.G + 0.114 * s.B);

    // 2) Continuum subtract (opt-in)
    double ha = s.Ha, oiii = s.OIII, sii = s.SII;
    if (mode_ == Mode::CONTINUUM_SUBTRACT) {
        ha   -= continuum_.k_Ha   * s.R;
        oiii -= continuum_.k_OIII * (s.G + s.B) * 0.5;
        sii  -= continuum_.k_SII  * s.R;
    }

    // 3) Natural Lab: chrominance from RGB when broadband present, then
    // override luminance with L_broad converted to CIE L* via the sRGB
    // transfer function. This preserves LRGBSHO design intent (dedicated
    // L drives luminance, RGB drives chrominance) AND yields identity
    // roundtrip on the gray axis, because srgb_to_linear(L) → Y → L* via
    // f_lab matches the natural L* of equal-RGB sRGB exactly.
    bool have_broadband = (s.R + s.G + s.B) > 0.0;
    LabColor lab_natural = have_broadband
        ? rgb_to_lab(s.R, s.G, s.B)
        : LabColor{ 0.0, 0.0, 0.0 };
    {
        double linear_y = srgb_to_linear(L_broad);
        lab_natural.L = 116.0 * f_lab(linear_y) - 16.0;
    }

    // 4) Emission contribution to chrominance
    double w_ha   = signal_weight(ha);
    double w_oiii = signal_weight(oiii);
    double w_sii  = signal_weight(sii);
    double total_w = w_ha + w_oiii + w_sii;

    LabColor pal_ha   = Palette::for_line(EmissionLineId::Ha);
    LabColor pal_oiii = Palette::for_line(EmissionLineId::OIII);
    LabColor pal_sii  = Palette::for_line(EmissionLineId::SII);

    double emission_a = 0.0, emission_b = 0.0, chroma_scale = 0.0;
    if (total_w > 0.0) {
        // Hue is the RATIO of the emission lines, never their absolute
        // strength. Lupton et al. 2004 (PASP 116, 133) state the rule this
        // once violated: under any non-linear mapping an object's colour
        // must not depend on its brightness. Brightness is carried by L
        // alone, which is how STScI colorizes a narrowband layer -- fixed
        // hue, fixed saturation, value carries the signal (Rector, Levay,
        // Frattare et al. 2004, AJ).
        const double inv_w = 1.0 / total_w;
        emission_a = (w_ha * pal_ha.a + w_oiii * pal_oiii.a + w_sii * pal_sii.a) * inv_w;
        emission_b = (w_ha * pal_ha.b + w_oiii * pal_oiii.b + w_sii * pal_sii.b) * inv_w;

        // Normalisation alone would hand a pixel at total_w = 1e-9 the full
        // palette vector. Ramp the chrominance down towards the measured
        // sky level so faint data desaturates on purpose rather than by
        // accident, and so blank sky is not painted a bold colour.
        if (gate_full_scale_ > gate_background_) {
            const double span = gate_full_scale_ - gate_background_;
            chroma_scale = std::min(1.0,
                std::max(0.0, (total_w - gate_background_) / span));
        } else {
            chroma_scale = 1.0;
        }
        emission_a *= chroma_scale;
        emission_b *= chroma_scale;
    }
    last_emission_a_ = emission_a;
    last_emission_b_ = emission_b;
    last_chroma_scale_ = chroma_scale;

    LabColor lab_final{
        lab_natural.L,
        lab_natural.a + emission_a,
        lab_natural.b + emission_b
    };

    // 5) Convert to sRGB and soft-clip
    sRGBPixel out = lab_to_srgb(lab_final);
    if (clip_to_gamut(out.r, out.g, out.b)) {
        ++gamut_clipped_;
    }
    return out;
}

} // namespace nukex
