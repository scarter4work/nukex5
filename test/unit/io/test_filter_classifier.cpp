#include "catch_amalgamated.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/io/filter_alias.hpp"
#include "nukex/core/frame_metadata.hpp"

using namespace nukex;

static FrameMetadata make_meta(const std::string& filter,
                               const std::string& bayer = "",
                               const std::string& instrument = "ASI585MC") {
    FrameMetadata m;
    m.filter        = filter;
    m.bayer_pattern = bayer;
    m.instrument    = instrument;
    return m;
}

TEST_CASE("FilterClassifier: HaO3 dual-NB on Bayer", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("HaO3", "RGGB"));
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "HaO3");
    REQUIRE(f.camera == "ASI585MC");
}

TEST_CASE("FilterClassifier: S2O3 dual-NB on Bayer", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("S2O3", "RGGB"));
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "S2O3");
}

TEST_CASE("FilterClassifier: H-alpha single-line narrowband on mono", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("Ha"))      .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("Halpha"))  .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("H-alpha")) .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("OIII"))    .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("O3"))      .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("SII"))     .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("S2"))      .cls == FilterClass::NARROWBAND_SINGLE);
}

TEST_CASE("FilterClassifier: L / R / G / B broadband on mono", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("L"))         .cls == FilterClass::BROADBAND_L);
    REQUIRE(c.classify(make_meta("Luminance")) .cls == FilterClass::BROADBAND_L);
    REQUIRE(c.classify(make_meta("R"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Red"))       .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("G"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Green"))     .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("B"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Blue"))      .cls == FilterClass::BROADBAND_RGB);
}

TEST_CASE("FilterClassifier: alias normalization", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("L-eXtreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("L_eXtreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("L Extreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("LExtreme") ).cls == FilterClass::DUAL_NB_OSC);
}

TEST_CASE("FilterClassifier: missing FILTER + Bayer -> BROADBAND_OSC silent", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("", "RGGB"));
    REQUIRE(f.cls == FilterClass::BROADBAND_OSC);
    REQUIRE(f.name == "OSC");
}

TEST_CASE("FilterClassifier: missing FILTER + mono -> BROADBAND_L L_unnamed silent",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("", ""));
    REQUIRE(f.cls == FilterClass::BROADBAND_L);
    REQUIRE(f.name == "L_unnamed");
}

TEST_CASE("FilterClassifier: unknown FILTER + Bayer -> UNKNOWN sentinel",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("ALP-T-fake-2026", "RGGB"));
    REQUIRE(f.cls == FilterClass::UNKNOWN);
    REQUIRE(f.name == "ALP-T-fake-2026");
}

TEST_CASE("FilterClassifier: unknown FILTER + mono -> BROADBAND_L preserve name + warn",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("custom_Hbeta", ""));
    REQUIRE(f.cls == FilterClass::BROADBAND_L);
    REQUIRE(f.name == "custom_Hbeta");
    REQUIRE(c.last_warning().find("Unknown filter") != std::string::npos);
    REQUIRE(c.last_warning().find("custom_Hbeta")   != std::string::npos);
}

TEST_CASE("FilterClassifier: broadband names on Bayer resolve to BROADBAND_OSC", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"L", "Luminance", "LPro", "L-Pro", "UV/IR Cut", "CLS-CCD", "LPS-D1", "L1"}) {
        Filter f = c.classify(make_meta(name, "RGGB", "ZWO ASI2400MC Pro"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_OSC);
        REQUIRE(f.name == "OSC");
        REQUIRE(c.last_warning().empty());
    }
}

TEST_CASE("FilterClassifier: broadband LPR names on mono resolve to BROADBAND_L named L", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"LPro", "UV-IR-Cut", "CLS", "LPS-D2"}) {
        Filter f = c.classify(make_meta(name, "", "ASI2600MM"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_L);
        REQUIRE(f.name == "L");
        REQUIRE(c.last_warning().empty());
    }
}

TEST_CASE("FilterClassifier: L-Quad Enhance resolves to the three-line canonical",
          "[filter_classifier]") {
    // Reported from a real stack, 2026-09-04: 53 Bayer frames with
    // FILTER='Lqef' stopped the batch at start. The QE data shipped all along,
    // as Optolong-L-Quad-Enhance, but the classifier had no spelling that
    // reached it -- the filter covers Ha, OIII and SII, and until now only
    // two-line sets had canonical names.
    FilterClassifier c;
    for (const char* spelling : { "Lqef", "L-QEF", "LQEF", "L-Quad", "L Quad Enhance",
                                  "Optolong L-Quad Enhance", "HaO3S2", "HaOIIISII" }) {
        INFO("FILTER = " << spelling);
        Filter f = c.classify(make_meta(spelling, "RGGB"));
        REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
        REQUIRE(f.name == "HaO3S2");
    }
}

TEST_CASE("FilterClassifier: a three-line filter on mono is still narrowband, not luminance",
          "[filter_classifier]") {
    // The broadband-on-mono fallback must not swallow it: this is a real
    // multi-line filter, not an unrecognised name to treat as luminance.
    FilterClassifier c;
    Filter f = c.classify(make_meta("Lqef"));
    REQUIRE(f.name == "HaO3S2");
}

TEST_CASE("FilterClassifier: a taught alias resolves a name the table does not know",
          "[filter_classifier]") {
    // The open-ended half of the problem: FITS FILTER values are whatever the
    // capture software wrote, so no shipped table can list them all. When the
    // interface learns one, the classifier must honour it.
    FilterAliasStore aliases;
    aliases.set("MyDuoBand", "HaO3");

    FilterClassifier c;
    REQUIRE(c.classify(make_meta("MyDuoBand", "RGGB")).cls == FilterClass::UNKNOWN);

    c.set_aliases(aliases);
    Filter f = c.classify(make_meta("MyDuoBand", "RGGB"));
    REQUIRE(f.cls  == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "HaO3");
}

TEST_CASE("FilterClassifier: an alias cannot shadow a shipped name",
          "[filter_classifier]") {
    // Consulted only after the built-in table, so a user file can add
    // spellings but never redefine what Ha or HaO3 mean.
    FilterAliasStore aliases;
    aliases.set("Ha", "S2O3");

    FilterClassifier c;
    c.set_aliases(aliases);
    Filter f = c.classify(make_meta("Ha"));
    REQUIRE(f.name == "Ha");
}

TEST_CASE("FilterClassifier: an alias is matched on the normalised name",
          "[filter_classifier]") {
    FilterAliasStore aliases;
    aliases.set("lqef", "HaO3S2");

    FilterClassifier c;
    c.set_aliases(aliases);
    REQUIRE(c.classify(make_meta("L-QEF", "RGGB")).name == "HaO3S2");
    REQUIRE(c.classify(make_meta("l qef", "RGGB")).name == "HaO3S2");
}
