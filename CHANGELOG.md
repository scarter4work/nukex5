# NukeX — Changelog

## v5.0.3.0 — 2026-09-06

### Fixed
- **The auto-stretch now has contrast, not just brightness.** The stretch had
  no shadow point: it always started from black. The trouble is that the sky
  is not black. On a 74-frame stack of M63 the background sat at 0.042 and the
  brightest 0.1% of the galaxy reached only 0.051 — the entire picture lived in
  a band about 1% wide, riding on a pedestal that used up 98% of the curve.
  What came out spanned 4% of the available range and looked flat and faint,
  which is exactly what it was.

  NukeX now solves a shadow point for each image as well as an intensity,
  clipping just below the noise floor at the same place PixInsight's own screen
  autostretch does. On that stack the separation between the background and the
  highlights went from 0.040 to 0.299 — **seven and a half times the contrast**
  — with the background landing on the same target as before. Positioning and
  contrast are separate problems, and turning the intensity up only ever solved
  the first one.

  The same work found the stretch measuring one thing and stretching another:
  it read the background off the green channel but applied the curve to
  sensor-weighted luminance. That put every stretched background about 5% above
  where it was aiming.

- **Broadband stacks are no longer green.** The sky itself is green through a
  typical filter and sensor — light pollution weighted by where the camera is
  most sensitive — and that arrives as a level *added* to each channel, not as
  a colour in the signal. On the M63 stack the background measured 1.54 times
  brighter in green than in red, while the stars, measured against their own
  channel's background, agreed to within 7%. The picture was neutral; the sky
  underneath it was not.

  Nothing downstream can undo that, because every stretch preserves colour
  ratios faithfully — the finished image was still 1.52 to one after a full
  round of processing in PixInsight. NukeX now brings each colour channel's sky
  level down to the dimmest of the three before the stack leaves the program,
  which is the earliest point it can be fixed and the only one that helps the
  rest of your workflow. Backgrounds come out neutral to within 0.2%, star
  colour is untouched, and the Process Console and FITS header both record
  exactly how much came off each channel, so you can put it back if you want it.

### Changed
- **The stack is now cropped to the region every frame covered.** A dithered,
  drifting session does not cover a rectangle: the outer edge of the frame is
  reached by fewer and fewer exposures, and while those pixels were averaged
  correctly they were averaged over less data. Measured against an interior
  noise level of 0.000234, the top ten rows of a 74-frame stack carried 4.7
  times as much noise and the right-hand columns 3.6 times. At the contrast the
  new stretch delivers, that reads as a grubby border around the picture.

  NukeX now keeps the largest rectangle every frame contributed to — the same
  thing you would reach for DynamicCrop to do, done for you and done exactly. A
  channel that fell short anywhere disqualifies the pixel, so a colour plane
  pushed off the edge by channel registration is trimmed rather than left as a
  dead line. On a 65-frame session this took a 3840×2160 stack to 3628×2019.
  The kept region is recorded in the FITS header.


## v5.0.2.0 — 2026-09-05

### Added
- **NukeX stacks LRGB mono batches.** A batch carrying more than one mono
  filter used to be refused, and before that it produced one populated
  channel and three black ones. Every mono frame has the same shape
  whatever filter took it, so all of them shared a single frame cache and no
  colour channel could read its own exposures back. Each filter now gets its
  own cache and every channel carries its own set of frames, so L, R, G and
  B stack together in one run — including the ordinary case where you shot
  twice as much luminance as colour.

- **Frames from the ends of a long session are no longer thrown away.**
  NukeX aligns every frame to one reference. Over a long night the mount
  drifts, and by the far end of the session the stars near the edges are not
  the same stars the reference saw, so matching fails even though the frame
  is perfectly good. A frame that cannot match the reference now matches a
  neighbour it *can* match, and the two transforms are combined. On a
  seven-hour M27 session that is the difference between using the middle of
  the night and using all of it. Frames are still resampled exactly once,
  and the Process Console says when a frame was aligned this way.

### Fixed
- **Narrowband colour no longer depends on brightness.** The colour of an
  emission-line pixel is meant to come from the ratio of the lines present,
  not from how bright the pixel is. NukeX was scaling colour by raw signal,
  so on a 12-frame M16 stack the entire green channel came out at zero: OIII
  rendered blue instead of teal, the background went magenta, and 24.5 of
  24.5 million pixels reported as clipped. This is the failure Lupton et al.
  (2004, PASP 116, 133) describe when they write that under a non-linear
  mapping "an object's color in the composite image depends upon its
  brightness". Hue is now the line ratio, brightness is carried by
  luminance alone, and out-of-gamut pixels are pulled back in a way that
  keeps their colour — the same approach used for the published Hubble
  images (Rector, Levay, Frattare et al. 2004, AJ).

  Faint pixels are still kept near-neutral so noise is not painted in bold
  colour, but that is now a deliberate threshold measured from the stack's
  own background rather than a side effect.

- **The edges of a stack are no longer dragged dark.** Where a frame was
  shifted to line up with the others, the pixels that fall outside it were
  left at zero, and the stacker could not tell those zeros from a genuine
  measurement of black — so it averaged them in. On a 53-frame session that
  put a visible black rim on the border and, less obviously, left a band up
  to 48 pixels deep noticeably too dark. NukeX now records which pixels each
  frame actually covers and only averages real measurements.

- **The auto-stretch now suits the image in front of it.** The stretch
  intensity was a single fixed number, and the backgrounds it had to cope
  with differ by nearly a factor of ten between targets — so the same
  setting left one stack sitting at 8% brightness and another at 43%.
  Turning it up globally would not have fixed it: a stronger stretch
  brightens but flattens, and on the brightest test set it cost more than
  half the separation between the background and the highlights. NukeX now
  solves the intensity against each image's own background, aiming at the
  same level PixInsight's own screen autostretch uses, so different targets
  come out looking comparable. The Process Console reports the value it
  picked.

- **Large stacks no longer drive the machine into swap.** The working batch
  size was chosen from the graphics card's memory, but the matching buffers
  are held in system memory, so a card with plenty of VRAM could ask for
  more system memory than the machine had. On a 24 MP colour stack that
  meant tens of gigabytes of swapping. The batch is now limited by both.
  Output is unchanged — verified bit-for-bit across four different batch
  sizes.


## v5.0.1.1 — 2026-09-04

### Added
- **NukeX now registers a frame's colour channels to each other.** Until now
  it aligned frames to one another but never aligned the colour planes
  *within* a frame, so lateral chromatic aberration and atmospheric dispersion
  smeared every star across colour no matter how good the frame-to-frame
  alignment was. Each channel is now centroided at the stars the aligner
  already found, fitted against green for uniform scale plus translation, and
  that correction is folded into the warp that was going to run anyway — one
  resample per channel, exactly as before, so it costs nothing extra.

  Measured on 53 Optolong L-Quad Enhance frames of M3, median star separation
  between the red and green planes of the finished stack:

  | | before | after |
  |---|---|---|
  | red vs green | 0.354 px | **0.042 px** |
  | blue vs green | 0.081 px | 0.046 px |

  Blue is the control rather than a second result: through a quad-band filter
  green and blue both image near 500 nm, so blue-green separation is mostly
  centroid noise and there is very little colour error in it to remove. Red
  images H-alpha at 656 nm and is where the error lives. On that rig red was
  scaled about +330 ppm relative to green — 1.2 px of displacement at the
  frame corner — and the correction is now measured and removed per frame.

  It is always on and needs no setting. A frame whose correction would move
  nothing measurable is left alone rather than resampled, so a well-corrected
  rig pays nothing for the feature. Mono frames are untouched.

### Changed
- **Stars are detected on green instead of channel 0.** On a debayered colour
  frame channel 0 is red — through a quad-band filter often both the weakest
  channel and the one carrying the most lateral colour error. Green has two of
  every four photosites and is the better reference for both star detection
  and channel registration. Because the frame-to-frame homography is fitted
  from those centroids, this sharpens the whole stack and not just the colour
  registration: median green star FWHM on the M3 set went from 4.24 px to
  3.77 px, and 33 of 33 and 12 of 12 frames still align on the regression
  corpora.

### Fixed
- **The last row and column of every aligned frame are no longer discarded.**
  The warp's bounds check rejected a source coordinate that landed exactly on
  the final row or column instead of interpolating it, so those output pixels
  were left at zero — and the stacker has no way to tell an absent sample from
  a measured black one, so the zeros were averaged in as though they were
  data. This is a small correction on its own (47 pixels of a 3840x2160 mono
  stack) and it is deliberately visible in the regression baselines.

## v5.0.1.0 — 2026-09-04

### Added
- **NukeX offers to learn a filter it does not recognise.** FITS `FILTER`
  values are whatever the capture software wrote, so no shipped table can
  list them all, and until now an unrecognised name on a colour camera just
  stopped the batch. It now asks which emission lines the filter passes —
  H-alpha, OIII, SII — records the answer in
  `<user-data>/nukex4/filter_aliases.json`, and re-stacks. Once.

  H-beta is not offered, and the dialog says why: at 486 nm it lands on the
  same photosites as OIII, so a solve carrying both has no unique answer. For
  the same reason Ha and SII without OIII is refused at the checkbox rather
  than accepted and failed later.

  The file is plain JSON you can read, edit or delete, keyed by the header
  name reduced to lowercase alphanumerics. It is consulted after the shipped
  table, so an entry can add a spelling but never redefine `Ha` or `HaO3`.

## v5.0.0.2 — 2026-09-04

All four found in the first real user session on v5.0.0.1, on 53 Optolong
L-Quad Enhance frames.

### Fixed
- **A quad-band filter such as L-Quad Enhance now stacks.** Its measured
  quantum efficiency shipped from the start, but no spelling reached it: the
  filter covers Ha, OIII and SII, and only two-line sets had canonical names,
  so the batch stopped at start with the data sitting in the database. There
  is now a three-line canonical, `HaO3S2`, derived from the same measurements
  the database was built from, and `Lqef`, `L-QEF`, `L-Quad`, `L-Quad-Enhance`
  and `L-Synergy` all resolve to it. Three lines on a colour sensor is exactly
  determined, so the decomposition is better conditioned than a two-line one,
  not worse. Ships as camera-database v2, which the in-module updater will
  offer.
- **The console no longer blames your installation for someone else's
  problem.** Every failed run appended "share/qe_database.json is missing from
  the plugin install", including runs one line after the console had announced
  updating that very database. The hint now appears only when the file is
  actually absent.
- **Text with punctuation in it renders correctly.** The interface and console
  passed UTF-8 characters to PixInsight, which reads them as ISO-8859-1, so
  every em dash and ellipsis arrived as garbage — the "Browse…" button read
  "Browseâ€¦". Eight strings affected.
- **The module reports its own version.** The console banner, the process
  description and the window title were hardcoded to "NukeX v4" on a v5 build.
  They now read the version header, so they cannot drift again.

## v5.0.0.1 — 2026-09-04

Two defects found by an adversarial review of the v5.0.0.0 engine changes.
Both are in batches that mix frame types; a batch of one kind is unaffected.

### Fixed
- **A batch mixing Bayer (CFA) frames with mono frames is refused.** The
  Bayer pattern is taken from the batch's first frame, so the two orderings
  were wrong in different directions. With a mono frame first the Bayer frame
  was never demosaiced and the colour routing read image channels that did
  not exist — an out-of-bounds read, and a crash on a real-sized frame. With
  a Bayer frame first every mono frame was demosaiced as though it were a
  mosaic, which produced no error at all and wrong pixels. Stack the two
  groups separately.
- **A frame the engine could not measure no longer terminates PixInsight.**
  If a frame failed to read during the measurement pass but read
  successfully afterwards, an internal consistency check called `abort()`,
  which takes the whole application down with no chance to save. It now
  fails the stack with an explanation.

## v5.0.0.0 — 2026-09-04

Colour-science overhaul. NukeX now knows what filter and camera produced
each frame, decomposes dual-narrowband OSC data into its emission lines
through the camera's measured quantum efficiency, and composes colour in
Lab/LCH with a calibrated emission-line palette.

**Read this before expecting different pixels.** The colour science is
delivered in a NEW window, `NukeX_composed`. On dual-narrowband data the
`NukeX_stacked` and `NukeX_stretched` windows are **bit-identical to
v4.0.1.0** — measured on 12 M16 HaO3 frames, same hash for both. Phase A
does route those frames into Ha/OIII slots rather than plain R/G/B, but the
stacked pixel values are the same debayered channels in the same order, so
the stack itself does not move. If you look only at the stretched window you
will see exactly what v4 gave you. The emission-line colour is in the
composed window, and that is the one to judge.

### Added
- Filter taxonomy: BROADBAND_L, BROADBAND_RGB, BROADBAND_OSC,
  NARROWBAND_SINGLE, DUAL_NB_OSC, resolved from FITS FILTER / BAYERPAT /
  INSTRUME with a tiered policy — an unknown dual-narrowband name on Bayer
  data stops the batch loudly; an unknown mono name warns and stacks as
  luminance.
- Quantum-efficiency database (`share/qe_database.json`): 55 cameras plus a
  generic Sony OSC fallback, and 96 filters including the canonical HaO3 /
  S2O3 / L-eXtreme / L-eNhance / L-Ultimate / ALP-T entries. Camera keys
  match INSTRUME case-insensitively and by model substring.
- Runtime camera-database updater: signed manifest, Ed25519 verification
  against a key embedded in the module, explicit consent before install, and
  an atomic replace. Declining a version is remembered.
- Phase B Q-matrix decomposition (Eigen QR) of dual-narrowband OSC stacks
  into Ha / OIII / SII slots, with multi-source OIII merge across HaO3 and
  S2O3 batches.
- ColorComposer: Lab/LCH composite of the derived slots against a calibrated
  emission-line palette with no green quadrant by construction. New
  `NukeX_composed` window; `NUKEX_GAMUT_CLIPPED` and `NUKEX_QE_CONFIDENCE`
  provenance keywords.
- OSC-as-LRGB: a rec709 luminance slot synthesised per OSC frame.
- "QE override file…" picker in the interface for cameras and filters the
  database does not carry (`docs/qe_overrides_format.md`).
- Broadband light-pollution filter names (L-Pro, LPS, UV-IR cut, CLS)
  recognised as plain OSC on Bayer cameras.

### Changed
- **The voxel record is sized to the stack's real channel count.** Every
  voxel used to carry seven per-channel arrays dimensioned at MAX_CHANNELS,
  so an L-only stack provisioned eight channels and used one; 1376 of 1436
  bytes per voxel were per-channel payload. Grouping those fields into one
  VoxelChannel record and storing exactly as many as the stack has takes a
  3-channel OSC voxel from 1436 bytes to 468. Two lossless packing changes
  ride along: 16-bit histogram bins (a bin cannot exceed the frame count,
  which was already uint16) and no interior padding in ZDistribution.

  | corpus | before | after |
  |---|---|---|
  | L-only 8.3 MP | 11.9 GB | 1.6 GB |
  | LRGB-mono 8.3 MP | 11.9 GB | 5.0 GB |
  | OSC 24.5 MP | 35.2 GB | 11.5 GB |
  | OSC 62 MP | 89.0 GB | 29.0 GB |

  The 24.5 MP corpora now fit in RAM instead of running on swap. Measured on
  the 33-frame M27 OSC set, Phase A went from 45 s/frame to 3.9 s/frame — the
  whole phase from around 25 minutes to 128 seconds. Pixel output is
  bit-identical; the frozen NGC7635 golden is unchanged by it.
- **The alignment reference is checked before it is used.** A measurement
  pass reads every frame before Phase A. The frame the aligner would have
  taken anyway — the first one — is kept whenever it reaches 75% of the best
  star count in the batch; only when it does not is it replaced, by the
  sharpest frame that does. On an LRGB-mono set the per-filter star yield
  varies enormously, so "whichever frame sorts first" was a coin flip: on
  M27 2025 it landed on a blue frame with 32 stars against 200 elsewhere, and
  71 of 72 frames aligned with zero inliers. That corpus now aligns 51 of 72.
  Batches whose first frame is already viable are unaffected, deliberately:
  moving the reference among equally good candidates was measured to cost
  alignments (65 of 65 down to 59 of 65 on NGC7635) for no gain.
- Rating DB `user_version` 1 → 2: stored filter classes migrate to the
  5-class encoding on first open; pre-v5 narrowband ratings become
  NARROWBAND_SINGLE.
- Rating popup shows the colour axis for RGB-mono and OSC stacks.
- E2E corpus: new OSC and dual-narrowband (M16 HaO3) baselines beside the
  preserved NGC7635 floor, which still verifies bit-identical — stacked
  `c2277834`, noise `b9ec9edd`, all three stretch sweeps — so nothing in this
  release moved the L-only path. The LRGB-mono corpus is present but skipped;
  see the mono-batch entry under Fixed. The harness now honours each case's
  declared `min_frames_ok_alignment` instead of demanding zero failures.
- Eigen is taken from the system (`find_package(Eigen3)`) rather than
  vendored.

### Fixed
- **Multi-filter mono batches are refused instead of returning empty
  channels.** A batch of separate L / R / G / B mono frames produced one
  populated channel and three that were exactly zero — a solid-coloured
  image. FrameCache is keyed on post-debayer geometry, so every mono frame
  shares one cache whatever its filter, and Phase B cannot read a given
  slot's own frames: one slot fits a mixture of all of them and the rest fit
  zeros. Phase A routing was never wrong. Giving each slot its own frame set
  reaches into the shadow buffers and weight kernels, which assume all
  channels share one, so until that lands the engine stops the batch and says
  to stack each mono filter separately. Single-filter mono batches are
  unaffected.
- **Heap corruption on a mixed-filter batch.** `ChannelConfig::merge` unions
  slot names, so a batch whose later frames carry filters the first frame did
  not needs more channels than the first frame implies. The cube was
  allocated from the first frame and the config grown underneath it, which
  only ever worked because the unused MAX_CHANNELS provisioning absorbed the
  overflow. The slot union is now settled before allocation and checked
  against it. A mono L frame followed by a Bayer HaO3 frame was enough to
  trigger it.
- Mono frames route by the slot the config registered rather than the raw
  FILTER string, so a filter-wheel slot number no longer aborts the stack.
- A voxel with fewer than three samples keeps the sample's robust location
  instead of being zeroed.

### Removed
- `StackingMode` enum, `ChannelConfig::from_mode`, `output_rgb_mapping`,
  `is_mono`.
- Module-local `filter_classifier` and `fits_metadata`, superseded by
  `lib/io`.

### Known limitations
- **A batch may carry only one mono filter.** Stack L, R, G and B separately
  and combine the results. The engine says so rather than guessing; see the
  mono-batch entry under Fixed.
- **Very long sessions may lose frames at the ends.** Alignment is against a
  single reference, so frames far from it in time can fail once tracking
  drift accumulates. Measured on a seven-hour M27 set: everything within
  about 90 minutes of the reference aligns, and 21 of 72 frames at the two
  ends do not. Failed frames are stacked unwarped at half weight.
- **A 24 MP colour stack needs more than 32 GB to stay out of swap.** The
  voxel record is 11.5 GB, and Phase B stages a batch sized from GPU memory
  in host RAM beside it. It completes on a 30 GB machine; it swaps while
  doing so.

## v4.0.1.0 — 2026-04-25

Phase 8: adaptive stretch tuning.  The first NukeX release where the
auto-stretch can *learn from your taste*: rate a stack 1-5 with optional
nudges on four axes (brightness, saturation, color, star bloat), and the
next stack on similar imagery uses your accumulated ratings to nudge its
own parameters.

The learner is a three-layer fallback: a closed-form ridge regression
trained from your own ratings (Layer 3) sits on top of an optional
community-bootstrap model (Layer 2, deferred to v4.0.1.x), which sits on
top of the Phase-5 hard-coded constants this project has shipped since
v4.0.0.4 (Layer 1).  At fresh install the user model is empty and Layer
2 is absent, so every stack falls through to Layer 1 — pixel output is
**bit-identical to v4.0.0.8** (verified end-to-end, 6/6 golden hashes).
The learning kicks in only after you actively click Save on a rating.

### Added
- **Rating popup after every Auto-stretch run.**  At the end of an
  `executeGlobal()` that produced a `NukeX_stretched` window, a modal
  asks "How did the stretch look?" with four signed sliders (-2..+2,
  centred at 0 = "fine") plus an Overall (1..5) slider.  The Color
  axis is hidden for mono / narrowband filter classes.  An "Rate last
  run" button on the NukeX Interface re-opens the dialog for the most
  recent run; a "Don't show after Execute" checkbox in the dialog
  persists the opt-out via PCL Settings under
  `NukeX/Phase8/RatingPopupSuppressed`.
- **Closed-form ridge regression (Layer 3).**  Saving a rating triggers
  an in-process retrain: the user's per-stretch run rows are loaded
  from SQLite, ridge-regressed via Eigen LDLT (no iterative solver, no
  external dependency), and the resulting per-parameter coefficients
  are atomically written to `~/.nukex4/phase8_user_model.json`.
  Atomic write = `tmp + fsync + rename` on POSIX; partial-write or
  power-loss leaves the previous good model in place.  Cross-validated
  R² is recorded per parameter so a future "Explain" UI can surface it.
- **Image-statistics feature extractor (29 columns).**  Each stack
  records per-channel mean / median / MAD / shadow-quartile / highlight
  / dynamic-range / saturated-fraction plus global SNR / star-density /
  noise-floor features as a single `runs` row keyed by a stable
  `run_id`.  Layer 3 trains a separate ridge regression per parameter
  per stretch, so a single user accumulates per-curve, per-axis taste
  data without one curve's behavior leaking into another's prediction.
- **SQLite ratings DB at `~/.nukex4/phase8_user.sqlite`.**  Schema v1
  with WAL journal mode + on-open integrity check.  CRUD lives in
  `src/lib/learning/rating_db.cpp`; an `ATTACH DATABASE` hook is in
  place to layer Phase 8.5's bootstrap rows on top of the user's own
  rows for the per-stretch query.
- **Layer-fallback wiring through `stretch_factory`.**  The factory
  consults `LayerLoader` (Layer 3 → Layer 2 → Layer 1 cascade) for the
  parameters it ships to each `StretchOp::set_param`, with hard
  per-parameter clamps from the new `StretchOp::param_bounds()` so the
  learner can never drive a stretch outside its safe range.  All seven
  Phase-5 curves participate (VeraLux, GHS, MTF, ArcSinh, Log, Lupton,
  CLAHE).

### Changed
- **NukeXInstance now carries a Phase 8 last-run context** (`run_id`,
  filter class, `lastRun` aggregate of the input stats + chosen curve
  + final params).  This is what the rating dialog reads back when
  the user clicks Save, and what the "Rate last run" button needs to
  re-open the dialog without re-stacking.  `run_id` is seeded from
  `std::random_device` so re-launches of PI never collide on a primary
  key.
- **RatingDialog UX polish.**  Sliders shrunk 180 → 120 px, axis
  labels compacted to `Axis  (low <-> high)` form, so the modal is
  narrow enough to sit beside the stretched image rather than
  covering it.

### Deferred — explicitly out of v4.0.1.0 scope
- **Phase 8.5 (community bootstrap):**  ~350 of Scott's labeled
  sessions exported to `share/phase8_bootstrap.sqlite` +
  `share/phase8_bootstrap_model.json`, plus a new E2E golden
  (`lrgb_mono_ngc7635_phase8_bootstrap`) covering the Layer 2
  activation path.  Until this ships, Layer 2 is absent and every
  Layer 3 fallback lands on Layer 1 — verified safe, see "Testing".
- **Phase 8 polish release:**  Reset-to-factory, Reset-to-bootstrap,
  and Explain UI escape hatches; non-modal rating dialog with live
  preview.  Punted to a v4.0.1.x polish release; the v4.0.1.0 modal
  is good enough to ship and the user has a "Don't show after
  Execute" opt-out for users who don't want to rate.

### Implementation notes
- The user-trained model is per-(stretch, parameter) — one ridge
  regression per cell of a 7-curves × ~2-params grid — so coefficients
  can't bleed across curves.  A bug here would corrupt only the cell
  being retrained, not the whole user model.
- `read_param_models_json` clears its output map on any parse failure
  to avoid silently mixing old + new data.  `SaveRatingFromLastRun`
  guards against the corrupt-file case (file exists but won't parse)
  so it doesn't atomically replace a bad-but-readable file with an
  empty one — see `e7709a1`.

### Testing
- ctest serial: 55/55 pass (was 53 at v4.0.0.8 ship; +1
  `test_atomic_write` for the JSON write path, +1
  `test_phase8_fallback` for the Layer 3 → 2 → 1 cascade including
  the missing-Layer-2-and-empty-user-model case that is in fact the
  fresh-install state).
- E2E goldens (6 hashes across primary `lrgb_mono_ngc7635` + GHS /
  MTF / ArcSinh sweep variants) byte-for-byte against v4.0.0.8.  No
  new goldens needed: existing goldens validate the no-rating path,
  Layer 3 activation lives in integration tests with seeded user DB.
- Two subagent-driven code-review catches landed during
  implementation: `7cf0ac3` (unseeded `std::rand()` collision risk on
  `run_id`) and `e7709a1` (corrupt `user_model_json` data-loss
  scenario on retrain).

### Build / plumbing
- **SQLite amalgamation vendored via FetchContent** (same pattern
  as cfitsio in v4.0.0.3) so the released `.so` doesn't depend on
  whatever sqlite the user's PI happens to load.  `ldd NukeX-pxm.so |
  grep -iE "sqlite|curl|ssl"` is empty by design.  The system path
  remains opt-in via `-DNUKEX_USE_SYSTEM_SQLITE=ON` for distro
  packagers.
- **nlohmann/json v3.11.3 vendored** for the user-model JSON
  round-trip.
- New library `src/lib/learning/` housing the rating DB, ridge
  regression, image stats, and per-stretch parameter model.

## v4.0.0.8 — 2026-04-21

Robustness + observability release driven by the first real-data stack on
shipped v4.0.0.7 (M27 Bayer-RGB, 2026-04-20).  Two concrete user-reported
gaps closed; pixel output on well-exposed frames is bit-identical to
v4.0.0.7 (E2E goldens unchanged).

### Added
- **Saturation guard in StarDetector.**  Before running
  `find_local_maxima`, the detector measures the fraction of pixels at
  or above `saturation_level` on a 4× decimated sample of the frame.
  If that fraction exceeds `saturation_reject_fraction` (default 0.5),
  the frame is rejected up front with an empty catalog.  Previously, a
  dawn-twilight frame where a majority of pixels were clipped sent the
  O(n²) exclusion-radius filter chasing a plateau of "local maxima" and
  hung `StarDetector::find_local_maxima` for 5+ minutes per frame.
  New unit tests exercise the hang on 1200×1200 (10.4 s → < 50 ms) and
  verify normal frames with a handful of saturated-core stars still
  detect correctly.
- **Distinctive log line for blown-out frames.**  In the stacking
  engine, a pre-flight saturation check replaces the generic
  `aligned: FAILED (stars=0)` line with
  `aligned: SKIPPED (blown out — X.X% pixels at saturation)` so a user
  watching the Process Console can tell a truly unusable frame from a
  detection misfire.
- **Fit-loop heartbeat in Phase B.**  The OpenMP-parallelised Ceres
  distribution-fitting loop (`gpu_executor.cpp:437`) now emits a
  `fitted K/N voxels (Ts)` line every 2 s from thread 0.  Previously
  the Process Console was silent for the entire 3–4 min single-batch
  fit between kernel 2 and kernel 3, which made long stacks look
  frozen.  The emission is rate-limited via an atomic compare-exchange
  on a shared "last report time" so the observer's mutex never lands
  on the hot compute path; unit tests (`test_fit_heartbeat`) lock in
  the thread-0 gate, the interval gate, and the concurrent-done count.

### Implementation notes
- New public `StarDetector::saturation_fraction(image, level)` helper
  backs both the detector's own guard and the stacking-engine log line
  so the fraction is computed from a single definition.
- New `FitHeartbeat` utility class in `src/lib/gpu` keeps the
  rate-limit / emission logic testable without a full Phase B harness.

## v4.0.0.7 — 2026-04-20

Polish pass on top of v4.0.0.6.  No compute-path changes — pixel output
is bit-identical to v4.0.0.6 (E2E golden hashes unchanged).  The
delta is entirely user-facing clarity, scripting ergonomics, and test
hygiene.

### Added
- **WARNING / CRITICAL** messages in the Process Console on low
  alignment success rate:  > 50 % failed fires `** CRITICAL **` with
  a remediation hint; 10–50 % fires `** WARNING **`; ≤ 10 % stays
  quiet (some drift / clouded frames is normal).
- **Auto-selector rationale** now names the FITS values that drove
  the classification:  `Auto: classified as LRGB-mono (FITS
  FILTER='L', BAYERPAT='', NAXIS3=1) -> VeraLux`.
- **Tooltips** on every NukeX interface control: Primary / Finishing
  Stretch dropdowns + labels, Enable GPU checkbox, frame-list tree
  boxes, and all Add / Remove / Clear / Toggle All buttons.
- **Validate()** now refuses to enable the Execute button when
  `cacheDirectory` is unset-but-non-default-and-unwritable, and warns
  at ExecuteGlobal time when fewer than 5 frames are enabled (Phase B's
  Ceres fitters need n ≥ 5 to converge).
- **FITS header provenance** on all output windows (`NukeX_stacked`,
  `NukeX_noise`, `NukeX_stretched`):  `CREATOR`, `NUKEXVER`,
  `NUKEXIMG`, `NFRAMES`, `NFAILALN`, `HISTORY` entries.  The stretched
  window additionally carries `PRIMSTR` / `FINSTR` with the applied
  curve names.

### Changed
- **Filter classifier** now case-folds the `FILTER` keyword before the
  narrowband-name lookup.  Previously depended on the FITS reader's
  uppercase normalisation; now robust to any casing upstream might
  produce.
- **ctest** runs all 45 tests in 94 s with no `-E` exclusion list.
  Heavy exploratory / integration / visual / sweep cases are re-tagged
  as opt-in Catch2 dot-prefixed tags (`[.integration]` etc.) so they
  skip by default but are still callable via the corresponding filter.

### Fixed
- **`tools/run_e2e.sh` and `tools/release.sh`** now use `set -o
  pipefail` so `PixInsight.sh … | tee file` surfaces a PI crash
  instead of silently reporting success.  `run_e2e.sh` also wraps PI
  in `timeout --kill-after=30s ${NUKEX_E2E_TIMEOUT:-3600}`.

### Testing
- Golden-hash E2E regression now covers the dropdown-sweep variants
  (`GHS`, `MTF`, `ArcSinh`) as well as the primary path, so a
  per-curve regression in (say) MTF while VeraLux stays stable is
  caught bitwise.
- New regression tests for the `isfinite` guard in `StudentTNLL::
  Evaluate` and `ContaminationNLL::Evaluate`:  extreme-outlier and
  all-identical-samples inputs that used to CHECK-abort the process
  at `ceres/line_search.cc:705` now exit the fitter cleanly.
- Thirteen mixed-case narrowband filter names (`Ha`, `ha`, `hA`,
  `h-alpha`, `Narrowband`, `nb`, …) lock in the defensive case-fold.

### Plumbing
- Module version macros centralised in `src/module/NukeXVersion.h`
  (previously only in `NukeXModule.cpp`, so any other translation unit
  that wanted them had to hardcode).  Version bumps now require
  editing one place.

## v4.0.0.6 — 2026-04-20

### Added
- **Alignment diagnostics exposed on PJSR.** Two new read-only process parameters — `nFramesProcessed` and `nFramesFailedAlignment` — are populated from the stacking result after `executeGlobal()`. Scripted callers can read them directly (`P.nFramesFailedAlignment`) instead of regex-parsing the Process Console log. The E2E harness (`tools/validate_e2e.js`) now fails a case if any frame failed alignment, rather than relying on wall-time and execute-ok alone.
- **Informative per-frame alignment log.** The Process Console used to emit `aligned (200 stars)` for every frame whether alignment succeeded or failed — a real UX trap that contributed to the NGC7635 alignment-failure regression going unnoticed before the v4.0.0.5 investigation. It now emits the actual outcome, inlier count, and sub-pixel RMS, e.g. `aligned: ok (stars=200, inliers=83, rms=0.088 px)` or `aligned: FAILED (stars=200, inliers=0, rms=0.000 px)`.

### Build / tooling
- **`make sign` and `make package`** now work out of the box: `tools/release.sh` drives the full mechanical post-build pipeline (sign `.so`, stage into `repository/bin/`, create date-stamped tarball, compute SHA-1, patch `updates.xri` with new `fileName`/`sha1`/`releaseDate`, re-sign the XRI). The previous `cmake/PackageAndSign.cmake` was never included and had three latent bugs (wrong target name, wrong tarball filename, no SHA-1 update) that would have surfaced the first time anyone typed `make package`.
- **`make e2e` / `make e2e-regen` hardened.** Invocation moved to `tools/run_e2e.sh` after the initial CMake inlining hit a `$$`-escape bug where `$NUKEX_E2E_REGEN` expanded to `$$` (shell PID) at configure time, corrupting the manifest path. The driver now also wipes the manifest's `output_root` before invoking PI as a belt-and-braces against any stale FITS triggering an overwrite prompt.
- **Silent-overwrite of harness FITS outputs.** `ImageWindow.saveAs` was being called with `verifyOverwrite=true`, which pops a modal "replace?" dialog in headless `--automation-mode`. Changed to `false`.

## v4.0.0.5 — 2026-04-20

### Fixed
- **Alignment: replaced the positional nearest-neighbour star matcher** with Groth (1986) / Valdes (1995) triangle similarity matching (K=100, canonical vertex ordering, sorted-r1 window index, vote-based correspondences). On real multi-hour imaging sessions with cumulative tracking drift >> 5 px, the old matcher reported "aligned (N stars)" but produced 0 usable matches in nearly all frames — the E2E baseline on NGC7635 L/Lights (65 frames, 3.5 hr) dropped from 61/65 failed alignments to **0/65**.
- **Phase B: Ceres solver crash on pathological voxels.** `StudentTNLL::Evaluate` and `ContaminationNLL::Evaluate` now return `false` instead of letting a non-finite cost/gradient reach `ceres/line_search.cc:705`'s CHECK-fail (which aborted the process after ~26 min on a full stack).

### Performance
- **Phase B per-voxel Ceres fitting now parallelised across CPU cores** (OpenMP `parallel for` with dynamic-256 scheduling). Each voxel's fit is independent: `ModelSelector::select` allocates its fitters as stack locals, so there is no shared mutable state to synchronise.
  - **Measured: 9.35× Phase B speedup** on NGC7635 L/Lights (32.86 min → 3.51 min). Total stack wall-time 34 min → 4.6 min. Ship bar was 1.5×.

### Observability
- **`NukeXProgress` emits three sideband progress channels** so headless harnesses (and curious users watching a long stack) can see liveness even during silent Ceres compute:
  1. `std::cerr` line per progress event (reaches shell via `2>&1 | tee`).
  2. Append log at `/tmp/nukex_progress.log` (`tail -f` from any terminal).
  3. 10-second heartbeat watchdog thread writes `/tmp/nukex_heartbeat.txt` with elapsed seconds + last phase/detail, proving liveness even when no callbacks fire.

### Testing
- **E2E validation harness** (`tools/validate_e2e.js` + `make e2e`). Runs the full pipeline against a manifest of test stacks, verifies execute-ok, wall-time budgets, 0-alignment-failure, and bitwise regression via FNV-1a hash of raw pixel samples (not file SHA — avoids the FITS write-time-stamp problem). Per-case golden hashes in `test/fixtures/golden/`. Dropdown sweep (`GHS`, `MTF`, `ArcSinh`) verified to produce distinct stretched outputs.
- **Baseline fixture:** `test/fixtures/phaseB_baseline_ms.txt` captures the authoritative Phase B floor (**1,971,756 ms** on NGC7635, pre-OpenMP) that B6 measures the post-optimisation run against.
- New `test_alignment_diag.cpp` (tag `[.diag]`, opt-in) loads real NGC7635 frames and sweeps matcher parameters for future debugging. Existing homography tests updated: 5×4 grid and collinear-point layouts replaced with deterministic-random positions (grids had too many congruent triangles for triangle-matching descriptors to discriminate).

### Build / plumbing
- `tools/capture_baseline.js` rewritten for PI `--automation-mode` quirks discovered while getting headless harnesses to work: `File.environmentVariable` does not see shell env (use `jsArguments`), `Console.writeln` does not reach shell stdout (wrap in `Console.beginLog()`/`endLog()`), enum values sharing an integer value collide on the prototype (use integer literals). See memory `reference_pjsr_automation_quirks.md`.
- `docs/superpowers/plans/2026-04-19-phase7-perf-findings.md` documents the flamegraph + selected optimisation + actual 9.35× speedup.
- `.gitignore` covers `Testing/`, `repository/bin/`, `build_profile/`, `.claude/`.

## v4.0.0.4 — 2026-04-20

### Added
- **Stretch pipeline wired to output.** New `NukeX_stretched` window opens alongside `NukeX_stacked` and `NukeX_noise`. Previously the module stacked but never applied the configured stretch.
- **Metadata-driven Auto selection.** `primaryStretch` defaults to `Auto`; the module reads FITS `FILTER`/`BAYERPAT`/`NAXIS3` from the first light, classifies into one of LRGB-mono / LRGB-color / Bayer-RGB / Narrowband, and picks the Phase-5 champion curve (VeraLux today across all classes). The classification is logged to the Console.
- **Finishing stretch slot.** New `finishingStretch` parameter; only `None` is enrolled this release. SAS / OTS / Photometric are deferred to later phases per their own dedicated brainstorms.

### Changed
- **Schema break:** the flat 10-entry `stretchType` enum and `autoStretch` bool are replaced by `primaryStretch` (Auto + 7 curves: VeraLux, GHS, MTF, ArcSinh, Log, Lupton, CLAHE) and `finishingStretch` (None only). Saved projects referencing the old IDs revert to defaults.

### Deferred to v4.0.0.5
- Phase B Ceres-fitting perf optimization (>=1.5x target on real workload).
- PJSR E2E validation harness (`make e2e`) with bitwise regression across a 4-stack corpus.

### Build
- Added optional `NUKEX_PROFILING=ON` CMake flag that adds `-fno-omit-frame-pointer` to support perf flamegraph capture for the v4.0.0.5 Phase B work.
- Added `#include <chrono>` Phase-B wall-time instrumentation in the stacker; logs `Phase B: <n> ms` via the progress observer.
