// NukeX v4 — Phase 7 E2E validation harness.
//
// Invocation:
//   PixInsight.sh --automation-mode --force-exit \
//     -r=tools/validate_e2e.js,manifest=<path>[,regen=1][,out=<dir>][,log=<path>]
//
// Runs the stacking pipeline once per case in the manifest and:
//   1. Verifies executeGlobal() succeeded.
//   2. Verifies wall-time is within the case's budget.
//   3. Verifies all frames aligned (≥ min_frames_ok_alignment).
//   4. Saves stacked + noise + stretched + composed FITS outputs to
//      <output_root>/<case>/primary/
//      (shell side computes SHA-256 and compares against goldens).
//   5. Runs each dropdown_sweep variant and saves its stretched output to
//      <output_root>/<case>/sweep_<label>/.  Shell side checks the stretched
//      outputs are distinct from the primary and from each other.
//
// Why key=value args: PI --automation-mode does not expose shell env to
// PJSR (File.environmentVariable always returns "").  Use jsArguments.
// Why the captured log file: Console.writeln does not reach shell stdout;
// we wrap everything in Console.beginLog()/endLog() and persist.
// See memory: reference_pjsr_automation_quirks.md.

function parseArgs() {
   var out = {
      manifest: "",
      regen: false,
      out_override: "",
      log: "/tmp/nukex_e2e_console.log",
      meta: "/tmp/nukex_e2e_meta.txt",
      // Run a single case by name. The full corpus takes hours; being able
      // to regenerate or re-verify one case keeps a long run from being
      // all-or-nothing, and makes an interrupted session cheap to resume.
      only: ""
   };
   if (typeof jsArguments === "undefined") return out;
   for (var i = 0; i < jsArguments.length; i++) {
      var kv = String(jsArguments[i]);
      var eq = kv.indexOf("=");
      if (eq < 0) continue;
      var k = kv.substring(0, eq);
      var v = kv.substring(eq + 1);
      if      (k === "manifest") out.manifest = v;
      else if (k === "regen")    out.regen = (v === "1" || v === "true");
      else if (k === "out")      out.out_override = v;
      else if (k === "log")      out.log = v;
      else if (k === "meta")     out.meta = v;
      else if (k === "only")     out.only = v;
   }
   return out;
}

function readJson(path) {
   if (!File.exists(path)) throw new Error("file not found: " + path);
   var text = File.readTextFile(path);
   // JSON.parseString is the PJSR-specific name; fall back to JSON.parse
   // on PI builds where JSON follows standard ECMAScript.
   if (typeof JSON.parseString === "function") return JSON.parseString(text);
   return JSON.parse(text);
}

function writeText(path, text) {
   File.writeTextFile(path, text);
}

function ensureDir(path) {
   if (!File.directoryExists(path)) {
      // createDirectory creates one level; walk components.
      var parts = path.split("/");
      var cur = "";
      for (var i = 0; i < parts.length; i++) {
         if (parts[i].length === 0) { cur += "/"; continue; }
         cur += parts[i];
         if (!File.directoryExists(cur)) File.createDirectory(cur, false);
         cur += "/";
      }
   }
}

// NukeX's frame cache exists to keep frame data OUT of RAM. This used to be
// hardcoded to "/tmp", which on Fedora is tmpfs -- so the "disk" cache was
// RAM, competing with the process it was meant to relieve. A 24 MP OSC
// corpus then OOM-killed the run at frame 17/33. Manifests set cache_dir to
// real disk; the default below is real disk too.
function cacheDir(manifest) {
   var d = (manifest && manifest.cache_dir) ? manifest.cache_dir
                                            : File.homeDirectory + "/.cache/nukex_e2e_frames";
   ensureDir(d);
   return d;
}

function collectLights(dir, glob, max_frames) {
   var pats = glob ? [glob] : ["*.fit", "*.fits", "*.FIT", "*.FITS"];
   var all = [];
   for (var i = 0; i < pats.length; i++) {
      var found = searchDirectory(dir + "/" + pats[i], false);
      for (var j = 0; j < found.length; j++) all.push(found[j]);
   }
   // Sort ONLY when a subset is requested.  "The first N of M" has no
   // meaning without a defined order, and searchDirectory's order is
   // filesystem-dependent.  Cases that consume the whole directory keep
   // the legacy unsorted order deliberately: sorting them would reshuffle
   // the frame sequence, move the alignment reference, and so break the
   // bit-identical v4.0.1.0 golden that lrgb_mono_ngc7635 exists to defend.
   if (max_frames && all.length > max_frames) {
      all.sort();
      all = all.slice(0, max_frames);
   }
   return all;
}

function findWindow(id_substr) {
   // The module registers output windows with names like "NukeX_stacked",
   // "NukeX_noise", "NukeX_stretched".  Iterate all open windows and return
   // the first whose mainView id contains id_substr.
   var wins = ImageWindow.windows;
   for (var i = 0; i < wins.length; i++) {
      var w = wins[i];
      if (String(w.mainView.id).indexOf(id_substr) >= 0) return w;
   }
   return null;
}

function fnvPixelHash(win) {
   // Deterministic 32-bit FNV-1a over raw pixel floats, sufficient for bitwise
   // regression detection.  Not cryptographic — purpose is to detect any
   // change in pixel content (which is what a correctness regression would
   // produce).  FITS headers are timestamped on save, so we hash the pixel
   // samples directly via PJSR rather than rely on whole-file SHA.
   var img = win.mainView.image;
   var w = img.width, h = img.height, nc = img.numberOfChannels;
   var samples = new Float32Array(w * h);
   var hash = 0x811c9dc5 | 0;
   for (var ch = 0; ch < nc; ch++) {
      img.getSamples(samples, new Rect(0, 0, w, h), ch);
      var i32 = new Uint32Array(samples.buffer);
      for (var i = 0; i < i32.length; i++) {
         hash = (hash ^ i32[i]) | 0;
         hash = Math.imul(hash, 16777619);
      }
   }
   var hex = (hash >>> 0).toString(16);
   while (hex.length < 8) hex = "0" + hex;
   return { w: w, h: h, nc: nc, fnv1a_hex: hex };
}

function saveAsFits(win, path) {
   // ImageWindow.saveAs( filePath, queryOptions, allowMessages, strict, verifyOverwrite )
   //
   // verifyOverwrite=true DOES prompt the user when the file already exists,
   // which in headless --automation-mode still pops a modal dialog that
   // blocks the harness until the user clicks.  We want silent overwrite:
   // pass false.  (The run_e2e.sh driver also rms the output dir before
   // invoking PI as a belt-and-braces check.)
   return win.saveAs(path, false /*queryOptions*/,
                     false /*allowMessages*/,
                     false /*strict*/,
                     false /*verifyOverwrite — silently overwrite*/);
}

function closeAllNukexWindows() {
   // Between cases / sweep variants, close any NukeX_* windows to prevent
   // name collisions in subsequent runs.
   var wins = ImageWindow.windows;
   for (var i = 0; i < wins.length; i++) {
      var w = wins[i];
      if (String(w.mainView.id).indexOf("NukeX") >= 0) {
         try { w.forceClose(); } catch (e) {}
      }
   }
}

function runPrimary(tc, out_dir, manifest) {
   var lights = collectLights(tc.light_dir, tc.light_glob, tc.max_frames);
   if (lights.length === 0)
      return { status: "fail", reason: "no FITS in " + tc.light_dir };
   Console.writeln("[" + tc.name + "] " + lights.length + " light frames");

   var P = new NukeX;
   var arr = [];
   for (var i = 0; i < lights.length; i++) arr.push([lights[i], true]);
   P.lightFrames = arr;
   P.flatFrames = [];
   P.primaryStretch = tc.primary_stretch;
   P.finishingStretch = tc.finishing_stretch;
   P.enableGPU = true;
   P.cacheDirectory = cacheDir(manifest);

   closeAllNukexWindows();

   var t0 = new Date().getTime();
   var ok = false;
   var execErr = "";
   try { ok = P.executeGlobal(); }
   catch (e) { execErr = String(e); }
   var elapsed = (new Date().getTime() - t0) / 1000.0;

   // Read the module's read-only output parameters. Older modules
   // that don't expose them come back undefined; treat as -1.
   var nProcessed = -1, nFailed = -1;
   try {
      if (typeof P.nFramesProcessed        !== "undefined") nProcessed = P.nFramesProcessed;
      if (typeof P.nFramesFailedAlignment  !== "undefined") nFailed    = P.nFramesFailedAlignment;
   } catch (e) {}

   if (!ok) return { status: "fail", reason: "executeGlobal false: " + execErr,
                     elapsed_s: elapsed,
                     n_frames_processed: nProcessed,
                     n_frames_failed_alignment: nFailed };

   ensureDir(out_dir);
   var saved = {};
   var hashes = {};
   var tags = ["stacked", "noise", "stretched", "composed"];
   for (var t = 0; t < tags.length; t++) {
      var w = findWindow("NukeX_" + tags[t]);
      if (w) {
         var p = out_dir + "/" + tags[t] + ".fit";
         try { if (saveAsFits(w, p)) saved[tags[t]] = p; }
         catch (e) { saved[tags[t] + "_save_error"] = String(e); }
         try { hashes[tags[t]] = fnvPixelHash(w); }
         catch (e) { hashes[tags[t] + "_hash_error"] = String(e); }
      }
   }

   return {
      status:                    "ok",
      elapsed_s:                 elapsed,
      lights:                    lights.length,
      n_frames_processed:        nProcessed,
      n_frames_failed_alignment: nFailed,
      saved_paths:               saved,
      pixel_hashes:              hashes
   };
}

function runSweepVariant(tc, variant, out_dir, manifest) {
   var lights = collectLights(tc.light_dir, tc.light_glob, tc.max_frames);
   if (lights.length === 0)
      return { status: "fail", reason: "no FITS" };

   var P = new NukeX;
   var arr = [];
   for (var i = 0; i < lights.length; i++) arr.push([lights[i], true]);
   P.lightFrames = arr;
   P.flatFrames = [];
   P.primaryStretch = variant.primary_stretch;
   P.finishingStretch = tc.finishing_stretch;
   P.enableGPU = true;
   P.cacheDirectory = cacheDir(manifest);

   closeAllNukexWindows();

   var t0 = new Date().getTime();
   var ok = false;
   try { ok = P.executeGlobal(); }
   catch (e) { return { status: "fail", reason: "throw: " + e }; }
   var elapsed = (new Date().getTime() - t0) / 1000.0;

   if (!ok) return { status: "fail", reason: "executeGlobal false",
                     elapsed_s: elapsed };

   ensureDir(out_dir);
   var saved = {};
   var hashes = {};
   var w = findWindow("NukeX_stretched");
   if (w) {
      var p = out_dir + "/stretched.fit";
      try { if (saveAsFits(w, p)) saved.stretched = p; }
      catch (e) { saved.stretched_save_error = String(e); }
      try { hashes.stretched = fnvPixelHash(w); }
      catch (e) { hashes.stretched_hash_error = String(e); }
   }
   return { status: "ok", elapsed_s: elapsed, saved_paths: saved, pixel_hashes: hashes };
}

function collectPrimaryHashes(primary) {
   var out = {};
   if (primary.pixel_hashes) {
      if (primary.pixel_hashes.stacked)   out.stacked   = primary.pixel_hashes.stacked.fnv1a_hex;
      if (primary.pixel_hashes.noise)     out.noise     = primary.pixel_hashes.noise.fnv1a_hex;
      if (primary.pixel_hashes.stretched) out.stretched = primary.pixel_hashes.stretched.fnv1a_hex;
      if (primary.pixel_hashes.composed)  out.composed  = primary.pixel_hashes.composed.fnv1a_hex;
   }
   return out;
}

function runCase(tc, manifest, out_root, regen, golden_dir) {
   var out_dir = out_root + "/" + tc.name;
   ensureDir(out_dir);

   // Check 1-4: primary run
   var primary = runPrimary(tc, out_dir + "/primary", manifest);
   var checks = { execute_ok: primary.status === "ok" };

   if (primary.status !== "ok") {
      return { name: tc.name, status: "fail", primary: primary, checks: checks };
   }

   // Check 5: wall-time budget
   checks.wall_time_s = primary.elapsed_s;
   checks.wall_time_budget_s = tc.wall_time_budget_s;
   checks.wall_time_within_budget = primary.elapsed_s <= tc.wall_time_budget_s;

   // Check 5b: alignment outcome via read-only module output parameter.
   // Surfaces regressions like the "61/65 failed" state that predated the
   // Groth triangle-matcher fix.
   //
   // The bar is the case's own `min_frames_ok_alignment`. Every case has
   // declared one since the manifest was written and this check ignored it,
   // demanding zero failures instead -- stricter than the manifest asked for
   // (bayer_rgb_m27_2023 declares 30 of 33) and impossible to express for a
   // corpus with a known, measured limitation. A per-case floor still
   // ratchets: it fails the moment alignment gets worse than what the case
   // was recorded at, which is what the check is for.
   //
   // A return of -1 means the module is older than this parameter; the check
   // is skipped rather than failed (graceful back-compat).
   checks.n_frames_processed         = primary.n_frames_processed;
   checks.n_frames_failed_alignment  = primary.n_frames_failed_alignment;
   if (primary.n_frames_failed_alignment === -1) {
      checks.alignment_all_ok = null;  // module pre-dates the param
   } else {
      var n_ok = primary.n_frames_processed - primary.n_frames_failed_alignment;
      // No declared floor means the historical bar: every frame must align.
      var floor = (tc.min_frames_ok_alignment === undefined)
                ? primary.n_frames_processed
                : tc.min_frames_ok_alignment;
      checks.n_frames_ok_alignment      = n_ok;
      checks.min_frames_ok_alignment    = floor;
      checks.alignment_all_ok           = (n_ok >= floor);
   }

   // Check 6: dropdown sweep
   var sweep_results = [];
   if (tc.dropdown_sweep && tc.dropdown_sweep.length > 0) {
      for (var i = 0; i < tc.dropdown_sweep.length; i++) {
         var v = tc.dropdown_sweep[i];
         var sdir = out_dir + "/sweep_" + v.label;
         var r = runSweepVariant(tc, v, sdir, manifest);
         r.label = v.label;
         sweep_results.push(r);
      }
   }
   checks.sweep_count          = sweep_results.length;
   checks.sweep_all_ok         = sweep_results.every(function(r){ return r.status === "ok"; });

   // Dropdown sweep sanity: stretched hashes must all differ from each other
   // and from the primary (different primary_stretch enums => different
   // pixels).  If any collide, either the module is ignoring primary_stretch
   // or two curves are accidentally producing the same output.
   var primary_stretched_hash =
      (primary.pixel_hashes && primary.pixel_hashes.stretched
          ? primary.pixel_hashes.stretched.fnv1a_hex : null);
   var seen = {};
   if (primary_stretched_hash) seen[primary_stretched_hash] = "primary";
   var sweep_distinct = true;
   for (var si = 0; si < sweep_results.length; si++) {
      var sh = (sweep_results[si].pixel_hashes
                  && sweep_results[si].pixel_hashes.stretched
                  ? sweep_results[si].pixel_hashes.stretched.fnv1a_hex
                  : null);
      if (!sh) { sweep_distinct = false; break; }
      if (seen[sh]) { sweep_distinct = false; break; }
      seen[sh] = sweep_results[si].label;
   }
   checks.sweep_distinct = sweep_distinct;

   // Check 7: bitwise regression against golden pixel hashes for BOTH
   // primary outputs AND each sweep variant.  Primary catches regressions
   // in stacking (alignment, Phase B fitting, the Auto stretch path);
   // sweep catches regressions in specific named curves (GHS / MTF /
   // ArcSinh).  On regen=true, rewrite the full golden; on regen=false,
   // compare and fail on any mismatch.  Missing sweep entries in the
   // golden are tolerated for back-compat with older one-level goldens.
   var golden_path = golden_dir + "/" + tc.name + ".json";
   var current_primary = collectPrimaryHashes(primary);
   var current_sweep = {};
   for (var si = 0; si < sweep_results.length; si++) {
      var sr = sweep_results[si];
      if (sr.status === "ok" && sr.pixel_hashes && sr.pixel_hashes.stretched)
         current_sweep[sr.label] = { stretched: sr.pixel_hashes.stretched.fnv1a_hex };
   }

   // A frozen golden is the regression floor.  regen must never rewrite it:
   // its entire value is being a fixed reference that later work is measured
   // against, so "the L-only path did not move" stays a provable claim
   // rather than one resting on remembering to `git checkout` after a regen.
   // It still VERIFIES under regen -- refusing to rewrite is not a reason to
   // stop checking, and this way every regen re-proves the floor for free.
   var frozen = tc.golden_frozen === true;
   var golden_check = { checked: false };
   if (regen && !frozen) {
      ensureDir(golden_dir);
      File.writeTextFile(golden_path,
         JSON.stringify({ primary: current_primary, sweep: current_sweep }) + "\n");
      golden_check = { checked: false, wrote: golden_path };
   } else if (File.exists(golden_path)) {
      var g = readJson(golden_path);
      var match = true;
      var diffs = {};
      // Primary hashes — iterate the GOLDEN's keys, not the run's, for
      // the same reason the sweep loop below does: a hash the run newly
      // produces (e.g. "composed", added in v5) is new coverage, not a
      // regression, and must not fail a golden recorded before it existed.
      // A key in the golden but missing from the run still fails, because
      // `got` comes back undefined — that direction IS a regression.
      if (g.primary) {
         for (var k in g.primary) {
            var want = g.primary[k];
            var got  = current_primary[k];
            if (got !== want) { match = false; diffs["primary." + k] = { got: got, want: want }; }
         }
      }
      // Sweep hashes — only check labels that are PRESENT in the golden.
      // Labels in the run but not in the golden don't count as regressions
      // (graceful back-compat); labels in the golden but not in the run
      // DO count as regressions (something disappeared).
      if (g.sweep) {
         for (var sk in g.sweep) {
            var want_s = g.sweep[sk].stretched;
            var got_s  = current_sweep[sk] ? current_sweep[sk].stretched : undefined;
            if (got_s !== want_s) {
               match = false;
               diffs["sweep." + sk + ".stretched"] = { got: got_s, want: want_s };
            }
         }
      }
      golden_check = { checked: true, match: match, diffs: diffs,
                       frozen: frozen, path: golden_path };
      checks.golden_match = match;
   } else {
      // No golden present — degrade gracefully, flag but don't fail
      // (CI should have run regen at least once).
      golden_check = { checked: false, reason: "no golden at " + golden_path };
      checks.golden_match = null;
   }

   var case_pass = checks.execute_ok
                   && checks.wall_time_within_budget
                   && checks.sweep_all_ok
                   && (sweep_results.length === 0 || checks.sweep_distinct)
                   && (checks.golden_match !== false)
                   && (checks.alignment_all_ok !== false);

   return {
      name:         tc.name,
      status:       case_pass ? "pass" : "fail",
      primary:      primary,
      sweep:        sweep_results,
      checks:       checks,
      golden_check: golden_check
   };
}

function runMain(args) {
   if (!args.manifest) {
      File.writeTextFile(args.meta, "STATUS fail\nREASON manifest= not provided\n");
      throw new Error("missing manifest= argument");
   }

   Console.beginLog();
   Console.writeln("=== NukeX v4 E2E validation (regen=" + args.regen + ") ===");
   var manifest = readJson(args.manifest);
   var out_root = args.out_override || manifest.output_root || "/tmp/nukex_e2e";
   // golden_dir in manifest is repo-relative; resolve against REPO root
   // (caller can override via out= but goldens live with the source tree).
   var golden_dir = manifest.golden_dir || "test/fixtures/golden";
   if (golden_dir.charAt(0) !== "/") {
      // repo-relative
      var repo = args.manifest.replace(/\/test\/fixtures\/[^/]+$/, "");
      golden_dir = repo + "/" + golden_dir;
   }
   ensureDir(out_root);

   var results = [];
   var overall_pass = true;
   for (var i = 0; i < manifest.cases.length; i++) {
      var tc = manifest.cases[i];
      if (args.only && tc.name !== args.only) {
         results.push({ name: tc.name, status: "skip", reason: "not selected by only=" });
         continue;
      }
      if (tc.skip) {
         Console.writeln("SKIP: " + tc.name + " — " + (tc.skip_reason || ""));
         results.push({ name: tc.name, status: "skip", reason: tc.skip_reason });
         continue;
      }
      Console.writeln("--- case: " + tc.name + " ---");
      var r = runCase(tc, manifest, out_root, args.regen, golden_dir);
      Console.writeln("  → " + r.status);
      if (r.status !== "pass") overall_pass = false;
      results.push(r);
   }

   var bytes = Console.endLog();
   File.writeTextFile(args.log, bytes.toString());

   var report = {
      status:     overall_pass ? "PASS" : "FAIL",
      manifest:   args.manifest,
      output_root: out_root,
      regen:      args.regen,
      cases:      results
   };
   var report_path = out_root + "/e2e_report.json";
   File.writeTextFile(report_path, JSON.stringify(report) + "\n");

   File.writeTextFile(args.meta,
      "STATUS " + (overall_pass ? "ok" : "fail") + "\n" +
      "REPORT " + report_path + "\n" +
      "LOG_PATH " + args.log + "\n" +
      "CASES " + results.length + "\n");

   if (!overall_pass) throw new Error("E2E validation failed; see " + report_path);
}

(function main() {
   var args = parseArgs();
   try {
      runMain(args);
   } catch (e) {
      var msg = String(e && e.message ? e.message : e);
      // Try to flush any captured log before propagating.
      try {
         var bytes = Console.endLog();
         if (bytes) File.writeTextFile(args.log, bytes.toString());
      } catch (e2) {}
      try {
         File.writeTextFile(args.meta,
            "STATUS fail\nREASON " + msg + "\nLOG_PATH " + args.log + "\n");
      } catch (e3) {}
      throw e;
   }
})();
