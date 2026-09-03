# QE override file

NukeX ships a quantum-efficiency database (`<PixInsight>/share/qe_database.json`,
56 cameras, 96 filters). To add a camera or filter it does not know, or to
replace shipped values with your own measurements, write a JSON file with
the same shape and select it with **QE override file… → Browse** in the
NukeX interface. Leave the field empty to use the shipped database only.

## Schema

```json
{
  "schema_version": 1,
  "cameras": {
    "<camera-key>": {
      "sensor": "IMX585",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "501": { "R": 0.03, "G": 0.85, "B": 0.50 },
        "656": { "R": 0.73, "G": 0.32, "B": 0.03 }
      },
      "confidence": "high"
    }
  },
  "filters": {
    "<filter-key>": {
      "type": "DUAL_NB",
      "lines": [
        { "name": "Ha",   "wavelength_nm": 656.3, "fwhm_nm": 7.0 },
        { "name": "OIII", "wavelength_nm": 500.7, "fwhm_nm": 7.0 }
      ]
    }
  }
}
```

- `qe` keys are wavelengths in nm; values are QE fractions 0–1 per photosite.
  OSC cameras use `R`, `G`, `B`; mono cameras use `mono_pk`.
- `confidence` is `high`, `medium` or `low`. It is informational.
- `type` on filters is informational (`DUAL_NB`, `NARROWBAND`, `BROADBAND`).

## How keys are matched

**Cameras** are matched against the FITS `INSTRUME` keyword after normalising
both to lowercase alphanumerics, and a database key may be a substring of the
header value. `INSTRUME = 'ZWO ASI2400MC Pro'` matches the shipped key
`asi2400mc`. Use the model name as the key (`asi2400mc`, `qhy268c`), not the
full header string. When no key matches, NukeX uses `generic_sony_imx_osc`,
prints a warning in the Process Console, and writes
`NUKEX_QE_CONFIDENCE = 'generic-fallback'` on the composed image.

**Filters** are matched by the *canonical* name the classifier derives from
the FITS `FILTER` keyword, not by the raw header text. Recognised spellings
(case and punctuation ignored): `HaO3`/`HaOIII` → `HaO3`; `S2O3`/`SIIOIII` →
`S2O3`; `L-eXtreme`, `L-eNhance`, `L-Ultimate`, `ALP-T`; `Ha`/`Halpha`,
`OIII`/`O3`, `SII`/`S2`; `L`/`Luminance`/`L-Pro`/`LPS`/`UV-IR-cut`/`CLS`
(broadband); `R`, `G`, `B`. A dual-narrowband filter with any other name on a
Bayer camera stops the batch at start — rename the `FILTER` keyword to one of
the spellings above, or set it to one of the canonical names and add a
matching `filters` entry here. Mono frames with an unknown `FILTER` are
treated as luminance with a warning.

## Override semantics

- `cameras` and `filters` merge with the shipped database.
- An entry whose key collides with a shipped key **replaces the whole entry**.
- New keys are added. `"cameras": {}` / `"filters": {}` is a valid no-op.

## Errors

Malformed JSON stops the batch with the parser's line and column. A camera
that exists but lacks QE at a needed wavelength yields a singular Q matrix
and stops Phase B with the camera and filter named.
