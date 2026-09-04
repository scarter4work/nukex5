#include "nukex/io/filter_classifier.hpp"
#include "nukex/core/frame_metadata.hpp"

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace nukex {

namespace {

struct KnownFilter {
    FilterClass    cls;
    const char*    canonical;
    double         center_nm;
    double         fwhm_nm;
};

const std::unordered_map<std::string, KnownFilter>& known_table() {
    static const std::unordered_map<std::string, KnownFilter> table = {
        {"r",         {FilterClass::BROADBAND_RGB,     "R",   620.0, 100.0}},
        {"red",       {FilterClass::BROADBAND_RGB,     "R",   620.0, 100.0}},
        {"g",         {FilterClass::BROADBAND_RGB,     "G",   540.0, 100.0}},
        {"green",     {FilterClass::BROADBAND_RGB,     "G",   540.0, 100.0}},
        {"b",         {FilterClass::BROADBAND_RGB,     "B",   460.0, 100.0}},
        {"blue",      {FilterClass::BROADBAND_RGB,     "B",   460.0, 100.0}},

        {"ha",        {FilterClass::NARROWBAND_SINGLE, "Ha",   656.3,   7.0}},
        {"halpha",    {FilterClass::NARROWBAND_SINGLE, "Ha",   656.3,   7.0}},
        {"oiii",      {FilterClass::NARROWBAND_SINGLE, "OIII", 500.7,   7.0}},
        {"o3",        {FilterClass::NARROWBAND_SINGLE, "OIII", 500.7,   7.0}},
        {"sii",       {FilterClass::NARROWBAND_SINGLE, "SII",  672.4,   7.0}},
        {"s2",        {FilterClass::NARROWBAND_SINGLE, "SII",  672.4,   7.0}},

        {"hao3",      {FilterClass::DUAL_NB_OSC,       "HaO3", 578.5, 155.0}},
        {"haoiii",    {FilterClass::DUAL_NB_OSC,       "HaO3", 578.5, 155.0}},
        {"s2o3",      {FilterClass::DUAL_NB_OSC,       "S2O3", 586.0, 170.0}},
        {"siioiii",   {FilterClass::DUAL_NB_OSC,       "S2O3", 586.0, 170.0}},
        {"lextreme",  {FilterClass::DUAL_NB_OSC,       "L-eXtreme",  578.5,  7.0}},
        {"lenhance",  {FilterClass::DUAL_NB_OSC,       "L-eNhance",  578.5, 25.0}},
        {"lultimate", {FilterClass::DUAL_NB_OSC,       "L-Ultimate", 578.5,  3.0}},
        {"alpt",      {FilterClass::DUAL_NB_OSC,       "ALP-T",      578.5,  5.0}},

        // Three-line filters. A quad-band product passes Hb as well, but Hb is
        // not a Q-solve line -- at 486 nm it lands on the same B/G photosites
        // as OIII and would make the Q matrix rank-deficient -- so what these
        // contribute is Ha + OIII + SII. That is exactly determined on an RGB
        // sensor. Until 2026-09-04 the set had no canonical name, so a real
        // batch of L-Quad Enhance frames stopped at start with its measured QE
        // sitting in the database, unreachable.
        {"hao3s2",    {FilterClass::DUAL_NB_OSC,       "HaO3S2",     578.5, 175.0}},
        {"haoiiisii", {FilterClass::DUAL_NB_OSC,       "HaO3S2",     578.5, 175.0}},
        {"lqef",      {FilterClass::DUAL_NB_OSC,       "HaO3S2",     578.5, 175.0}},
        {"lquad",     {FilterClass::DUAL_NB_OSC,       "HaO3S2",     578.5, 175.0}},
        {"lquadenhance",
                      {FilterClass::DUAL_NB_OSC,       "HaO3S2",     578.5, 175.0}},
        {"optolonglquadenhance",
                      {FilterClass::DUAL_NB_OSC,       "HaO3S2",     578.5, 175.0}},
        {"lsynergy",  {FilterClass::DUAL_NB_OSC,       "HaO3S2",     578.5, 175.0}},
    };
    return table;
}

// Broadband names whose class depends on the sensor: OSC on a Bayer frame,
// luminance on a mono frame. Normalised (lowercase, alphanumerics only).
// Sources: research/qe_database_research.json types `luminance` and
// `broadband-LPR`, plus the bare L aliases moved out of known_table().
const std::unordered_set<std::string>& broadband_any_names() {
    static const std::unordered_set<std::string> names = {
        "l", "lum", "luminance",
        "lpro", "lpr", "lps", "lpsd1", "lpsd2", "lpsd3", "lpsv4",
        "uvir", "uvircut", "uvirblock", "irblock", "uvcut",
        "cls", "clsccd",
        "l1", "l2", "l3",           // Astronomik L1/L2/L3 UV-IR block
    };
    return names;
}

} // namespace

std::string FilterClassifier::normalize_name(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

FilterClass FilterClassifier::lookup_known(const std::string& normalized,
                                           std::string& canonical_name_out,
                                           BandwidthSpec& bandwidth_out) {
    const auto& table = known_table();
    auto it = table.find(normalized);
    if (it == table.end()) return FilterClass::UNKNOWN;
    canonical_name_out      = it->second.canonical;
    bandwidth_out.center_nm = it->second.center_nm;
    bandwidth_out.fwhm_nm   = it->second.fwhm_nm;
    return it->second.cls;
}

Filter FilterClassifier::classify(const FrameMetadata& meta) {
    last_warning_.clear();

    const bool        is_bayer  = !meta.bayer_pattern.empty();
    const std::string normalized = normalize_name(meta.filter);

    Filter out;
    out.camera = meta.instrument;

    if (normalized.empty()) {
        if (is_bayer) {
            out.cls       = FilterClass::BROADBAND_OSC;
            out.name      = "OSC";
            out.bandwidth = BandwidthSpec{550.0, 300.0};
        } else {
            out.cls       = FilterClass::BROADBAND_L;
            out.name      = "L_unnamed";
            out.bandwidth = BandwidthSpec{550.0, 300.0};
        }
        return out;
    }

    if (broadband_any_names().count(normalized)) {
        if (is_bayer) {
            out.cls  = FilterClass::BROADBAND_OSC;
            out.name = "OSC";
        } else {
            out.cls  = FilterClass::BROADBAND_L;
            out.name = "L";
        }
        out.bandwidth = BandwidthSpec{550.0, 300.0};
        return out;
    }

    std::string   canonical;
    BandwidthSpec bw;
    FilterClass   cls = lookup_known(normalized, canonical, bw);
    if (cls != FilterClass::UNKNOWN) {
        out.cls       = cls;
        out.name      = canonical;
        out.bandwidth = bw;
        return out;
    }

    // Not in the shipped table. Before giving up, consult what the user has
    // taught us: the interface offers to learn an unrecognised name by asking
    // which emission lines it passes, and records the answer against the
    // normalised header value. Checked here, last, so a taught name can only
    // add a spelling -- it can never redefine Ha or HaO3.
    const std::string taught = aliases_.lookup(normalized);
    if (!taught.empty()) {
        cls = lookup_known(FilterAliasStore::normalize(taught), canonical, bw);
        if (cls != FilterClass::UNKNOWN) {
            out.cls       = cls;
            out.name      = canonical;
            out.bandwidth = bw;
            return out;
        }
    }

    if (is_bayer) {
        out.cls  = FilterClass::UNKNOWN;
        out.name = meta.filter;
    } else {
        out.cls       = FilterClass::BROADBAND_L;
        out.name      = meta.filter;
        out.bandwidth = BandwidthSpec{550.0, 300.0};
        last_warning_ = "Unknown filter '" + meta.filter
                      + "' for mono frame -- treating as generic luminance. "
                      + "If this is a narrowband filter, rename FILTER to Ha/OIII/SII or add it to a qe_overrides.json selected in the NukeX interface.";
    }
    return out;
}

} // namespace nukex
