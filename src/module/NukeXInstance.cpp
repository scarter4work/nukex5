// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NukeXInstance.h"
#include "NukeXParameters.h"
#include "NukeXProcess.h"
#include "NukeXVersion.h"

#include "NukeXProgress.h"
#include "RatingDialog.h"
#include "FilterDialog.h"
#include "nukex/io/filter_alias.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/calibration/qe_update_state.hpp"
#include <filesystem>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/GlobalSettings.h>

// NukeX pipeline headers
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/compose/color_composer.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/stretch_pipeline.hpp"
#include "nukex/stretch/image_stats.hpp"
#include "nukex/stretch/layer_loader.hpp"
#include "nukex/stretch/param_model.hpp"
#include "nukex/learning/rating_db.hpp"
#include "nukex/learning/train_model.hpp"
#include "nukex/learning/atomic_write.hpp"
#include "nukex/io/fits_reader.hpp"
#include "stretch_factory.hpp"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>   // std::getenv
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Build a FITS keyword set that records which build of NukeX produced this
// image and what the stacking result looked like.  Callers layer
// image-type-specific keywords (e.g., primaryStretch label) on top.
pcl::FITSKeywordArray base_output_keywords(
   const char* nukex_version,
   const char* image_kind,
   int n_frames_processed,
   int n_frames_failed_alignment )
{
   pcl::FITSKeywordArray ka;
   ka.Append( pcl::FITSHeaderKeyword(
      "CREATOR", pcl::IsoString( "'NukeX v" ) + nukex_version + "'",
      "Software that produced this image" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NUKEXVER", pcl::IsoString( "'" ) + nukex_version + "'",
      "NukeX module version" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NUKEXIMG", pcl::IsoString( "'" ) + image_kind + "'",
      "NukeX output kind: stacked | noise | stretched" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NFRAMES", pcl::IsoString().Format( "%d", n_frames_processed ),
      "Number of light frames processed" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NFAILALN", pcl::IsoString().Format( "%d", n_frames_failed_alignment ),
      "Frames flagged as failed alignment (weight x 0.5)" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "HISTORY", pcl::IsoString(),
      pcl::IsoString( "NukeX v" ) + nukex_version
          + " - Distribution-Fitted Stacking" ) );
   return ka;
}

// Phase 8 rating-DB filter-class encoding (rating_db.hpp schema v2).
//
// RatingDialog shows the color-balance axis only for classes whose output
// carries broadband chrominance: BROADBAND_RGB (2) and BROADBAND_OSC (3).
// Luminance, single-line narrowband and dual-NB composites hide it.
int filter_class_to_rating_int( nukex::FilterClass fc )
{
   switch ( fc )
   {
   case nukex::FilterClass::BROADBAND_L:       return 1;
   case nukex::FilterClass::BROADBAND_RGB:     return 2;
   case nukex::FilterClass::BROADBAND_OSC:     return 3;
   case nukex::FilterClass::NARROWBAND_SINGLE: return 4;
   case nukex::FilterClass::DUAL_NB_OSC:       return 5;
   case nukex::FilterClass::UNKNOWN:           return 0;
   }
   return 0;
}

// Serialize the trainable params on `op` to a compact JSON object so Task
// 19 can store it in the rating DB verbatim. Keys come from
// op.param_bounds(); only params whose current value is readable via
// get_param() are emitted.
std::string op_trainable_params_json( const nukex::StretchOp& op )
{
   using json = nlohmann::json;
   json j = json::object();
   for ( const auto& [pname, _] : op.param_bounds() )
   {
      auto v = op.get_param( pname );
      if ( v.has_value() )
         j[pname] = static_cast<double>( *v );
   }
   return j.dump();
}

// <PixInsight base>/share — where the release tarball deposits
// qe_database.json (Task 16) and where the Phase 8 bootstrap will live.
static std::string PIShareRoot()
{
   const std::string base =
       pcl::PixInsightSettings::GlobalString( "Application/BaseDirectory" ).ToUTF8().c_str();
   return base + "/share";
}

// <user-data>/nukex4 -- the same 0700 directory resolve_user_data_paths()
// creates for the Phase 8 state. A downloaded database lands here because
// <PI>/share is root-owned in a normal install and a user must not need
// write access there to receive a camera-database update.
static std::string QEUserDataRoot()
{
   const char* home = std::getenv( "HOME" );
   return ( home ? std::string( home ) + "/.config" : std::string( "/tmp" ) )
          + "/nukex4";
}

static std::string QEUpdateStatePath()
{
   return QEUserDataRoot() + "/qe_update_state.json";
}

// Precedence: a downloaded database beats the shipped one. The shipped copy
// is never modified or removed, so deleting a single file always restores
// the as-installed behaviour. The user's qe_overrides.json still layers on
// top of whichever wins here -- the engine applies that separately.
static bool UsingDownloadedQEDatabase()
{
   std::error_code ec;
   const bool present = std::filesystem::exists(
       QEUserDataRoot() + "/qe_database.json", ec );
   return present && !ec;
}

static std::string ResolveQEDatabasePath()
{
   if ( UsingDownloadedQEDatabase() )
      return QEUserDataRoot() + "/qe_database.json";
   return PIShareRoot() + "/qe_database.json";
}

// The version of the database actually loaded, which is not the same thing
// as the version last installed: delete the downloaded file and the shipped
// one takes over. A provenance keyword that names a version we did not use
// is worse than none at all, since explaining a changed result is its only
// job. 0 means "the database that shipped with this module".
static int ActiveQEDatabaseVersion()
{
   if ( !UsingDownloadedQEDatabase() )
      return 0;
   return nukex::load_update_state( QEUpdateStatePath() ).installed_db_version;
}

} // anonymous namespace

namespace pcl
{

// Task 19: persist the user's rating row, retrain Layer 3 for this stretch,
// and atomically replace the user-model JSON. Called from ExecuteGlobal's
// rating popup *and* from NukeXInterface's "Rate last run" button (Task 18).
//
// Path discipline: we use the paths stashed on lastRun at Execute time, not
// a fresh resolve_user_data_paths() call. If HOME changed between Execute
// and Save (rare, but e.g. user switching env), the row goes to the same DB
// the predict path read from -- no drift between Layer 3 reads and writes.
void NukeXInstance::SaveRatingFromLastRun( const pcl::RatingResult& res )
{
   if ( !lastRun.valid )
      return;

   pcl::Console console;

   // Open (or create) the per-user rating DB at the path we resolved at
   // Execute time. If the DB can't be opened, the rating is discarded --
   // we never silently "kind of saved it".
   sqlite3* db = nukex::learning::open_rating_db( lastRun.user_db_path );
   if ( db == nullptr )
   {
      console.CriticalLn( String( "NukeX: couldn't open rating DB; rating discarded." ) );
      return;
   }

   // Build the RunRecord. ImageStats::to_feature_row() returns the same
   // 29-column vector the predict path consumed, so training reads exactly
   // what prediction saw (24 per-channel followed by 5 global stats).
   nukex::learning::RunRecord rec;
   rec.run_id           = lastRun.run_id;
   rec.created_at_unix  = lastRun.created_at_unix;
   rec.stretch_name     = lastRun.stretch_name;
   rec.target_class     = lastRun.target_class;
   rec.filter_class     = lastRun.filter_class;

   const auto feature_row = lastRun.stats.to_feature_row();
   for ( int i = 0; i < 24; ++i )
      rec.per_channel_stats[i] = feature_row[i];
   rec.bright_concentration = lastRun.stats.bright_concentration;
   rec.color_rg             = lastRun.stats.color_rg;
   rec.color_bg             = lastRun.stats.color_bg;
   rec.fwhm_median          = lastRun.stats.fwhm_median;
   rec.star_count           = lastRun.stats.star_count;

   rec.params_json          = lastRun.params_json_applied;
   rec.rating_brightness    = res.brightness;
   rec.rating_saturation    = res.saturation;
   rec.rating_color         = res.color;          // std::optional<int>
   rec.rating_star_bloat    = res.star_bloat;
   rec.rating_overall       = res.overall;

   if ( !nukex::learning::insert_run( db, rec ) )
   {
      console.CriticalLn( String( "NukeX: rating insert failed; rating discarded." ) );
      nukex::learning::close_rating_db( db );
      return;
   }

   // Attach the read-only bootstrap DB if it ships with this version.
   // At v4.0.1.0 ship the bootstrap is empty (Phase 8.5 populates), so
   // attach is a no-op and train_one_stretch sees just the user rows.
   // Re-derive share_root only for the bootstrap path -- symmetrical with
   // ExecuteGlobal and avoids stashing another field on LastRunState.
   {
      const char* home = std::getenv( "HOME" );
      const std::string user_data_root =
          home ? std::string( home ) + "/.config" : std::string( "/tmp" );
      const std::string share_root = PIShareRoot();
      auto paths = nukex::learning::resolve_user_data_paths( user_data_root, share_root );
      nukex::learning::attach_bootstrap( db, paths.bootstrap_db );
   }

   // Retrain Layer 3 for this stretch only. If not enough rows yet, the
   // per_param map comes back empty and we simply skip the model update.
   auto stretch_coeffs = nukex::learning::train_one_stretch(
       db, lastRun.stretch_name, /*lambda=*/ 1.0 );

   nukex::learning::close_rating_db( db );
   db = nullptr;

   if ( stretch_coeffs.per_param.empty() )
   {
      // Not enough data yet -- Layer 3 stays unchanged. Still counts as
      // a successful rating save; the row is in the DB for future fits.
      console.NoteLn( String( "NukeX: rating saved. Layer 3 will update once more rows are collected." ) );
      return;
   }

   // Load existing user models, replace the retrained stretch, serialise
   // to a JSON string, then atomic_write over the target file.
   //
   // Corrupt-file guard: read_param_models_json clears `out` on any parse
   // failure. If the user file exists but is unparseable, blindly merging
   // into the empty map and writing back would silently destroy the
   // other stretches' coefficients. Preserve the corrupt file for forensics
   // and keep the rating row -- Layer 3 for this stretch stays stale until
   // the user resolves the corruption (or a future Reset-to-bootstrap
   // escape hatch lands in the deferred polish release).
   nukex::ParamModelMap models;
   if ( std::filesystem::exists( lastRun.user_model_json_path ) )
   {
      if ( !nukex::read_param_models_json( lastRun.user_model_json_path, models ) )
      {
         console.CriticalLn( String(
             "NukeX: user model JSON is corrupt; rating row saved but Layer 3 "
             "not rewritten. Original file preserved for forensics." ) );
         return;
      }
   }
   // else: fresh install, no user model yet -- proceed with empty map.

   nukex::ParamModel updated( lastRun.stretch_name );
   for ( const auto& [pname, c] : stretch_coeffs.per_param )
   {
      nukex::ParamCoefficients out;
      out.feature_mean = c.feature_mean;
      out.feature_std  = c.feature_std;
      out.coefficients = c.coefficients;
      out.intercept    = c.intercept;
      out.lambda       = c.lambda;
      out.n_train_rows = c.n_train_rows;
      out.cv_r_squared = c.cv_r_squared;
      updated.add_param( pname, std::move( out ) );
   }

   models.erase( lastRun.stretch_name );
   if ( !updated.empty() )
      models.emplace( lastRun.stretch_name, std::move( updated ) );

   // Stage-serialise: write_param_models_json takes a path. Write to a
   // staging path, slurp it back into a string, then atomic_write_file
   // over user_model_json_path. A future refactor should add a to_string()
   // overload; keeping the dance inline for v4.0.1.0 avoids touching the
   // serialiser signature and its callers.
   const std::string stage_path = lastRun.user_model_json_path + ".stage";
   if ( !nukex::write_param_models_json( models, stage_path ) )
   {
      console.CriticalLn( String(
          "NukeX: couldn't stage retrained model; rating row saved but Layer 3 stale." ) );
      return;
   }

   std::string staged_contents;
   {
      std::ifstream stage( stage_path, std::ios::binary );
      std::stringstream buf;
      buf << stage.rdbuf();
      staged_contents = buf.str();
   }
   std::error_code ec_rm;
   std::filesystem::remove( stage_path, ec_rm );

   if ( !nukex::learning::atomic_write_file( lastRun.user_model_json_path, staged_contents ) )
   {
      console.CriticalLn( String(
          "NukeX: couldn't persist retrained model; rating row saved but Layer 3 stale." ) );
      return;
   }

   console.NoteLn( String(
       IsoString( "NukeX: Layer 3 coefficients updated for " )
       + lastRun.stretch_name.c_str() + "." ) );
}

NukeXInstance::NukeXInstance( const MetaProcess* m )
   : ProcessImplementation( m )
{
}

NukeXInstance::NukeXInstance( const NukeXInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

void NukeXInstance::Assign( const ProcessImplementation& p )
{
   const NukeXInstance* x = dynamic_cast<const NukeXInstance*>( &p );
   if ( x != nullptr )
   {
      lightFrames      = x->lightFrames;
      flatFrames       = x->flatFrames;
      primaryStretch   = x->primaryStretch;
      finishingStretch = x->finishingStretch;
      enableGPU        = x->enableGPU;
      cacheDirectory   = x->cacheDirectory;
      qeOverridePath   = x->qeOverridePath;
   }
}

bool NukeXInstance::IsHistoryUpdater( const View& ) const
{
   return false;  // Global process — doesn't modify existing views
}

UndoFlags NukeXInstance::UndoMode( const View& ) const
{
   return UndoFlag::DefaultMode;
}

bool NukeXInstance::CanExecuteOn( const View&, String& whyNot ) const
{
   whyNot = "NukeX is a global process. It cannot be executed on views.";
   return false;
}

bool NukeXInstance::Validate( String& whyNot )
{
   if ( lightFrames.IsEmpty() )
   {
      whyNot = "No light frames specified.";
      return false;
   }
   int n_enabled = 0;
   for ( const auto& f : lightFrames )
      if ( f.enabled ) ++n_enabled;
   if ( n_enabled == 0 )
   {
      whyNot = "No light frames are enabled.";
      return false;
   }

   // Cache directory writability check.  A non-writable cache is a
   // common misconfiguration (stale /tmp, wrong mount point, etc.)
   // and previously surfaced only as cryptic I/O errors deep inside
   // the ring-buffer later in Phase A.  Catch it at Validate time so
   // PI's Execute button stays disabled with a clear message.
   if ( !cacheDirectory.IsEmpty() )
   {
      String cache = cacheDirectory.Trimmed();
      if ( !cache.IsEmpty() )
      {
         IsoString cache_utf8 = cache.ToUTF8();
         const char* p = cache_utf8.c_str();
         struct stat st;
         if ( ::stat( p, &st ) != 0 || !S_ISDIR( st.st_mode ) )
         {
            whyNot = "Cache directory does not exist or is not a directory: " + cache;
            return false;
         }
         if ( ::access( p, W_OK ) != 0 )
         {
            whyNot = "Cache directory is not writable by the current user: " + cache;
            return false;
         }
      }
   }

   return true;
}

bool NukeXInstance::CanExecuteGlobal( String& whyNot ) const
{
   if ( lightFrames.IsEmpty() )
   {
      whyNot = "No light frames specified.";
      return false;
   }
   int n_enabled = 0;
   for ( const auto& f : lightFrames )
      if ( f.enabled ) ++n_enabled;
   if ( n_enabled == 0 )
   {
      whyNot = "No light frames are enabled.";
      return false;
   }
   return true;
}

bool NukeXInstance::ExecuteGlobal()
{
   NukeXProgress progress;
   progress.message( "NukeX " NUKEX_VERSION_STRING " -- Distribution-Fitted Stacking" );
   progress.message( String().Format( "Processing %zu light frame(s)", lightFrames.Length() ).ToUTF8().c_str() );

   // Collect enabled file paths
   std::vector<std::string> light_paths;
   for ( const auto& f : lightFrames )
   {
      if ( f.enabled )
         light_paths.push_back( f.path.ToUTF8().c_str() );
   }

   std::vector<std::string> flat_paths;
   for ( const auto& f : flatFrames )
   {
      if ( f.enabled )
         flat_paths.push_back( f.path.ToUTF8().c_str() );
   }

   if ( light_paths.empty() )
   {
      progress.message( "** No enabled light frames. Aborting." );
      return false;
   }

   // Advisory: Student-t / contamination fitters need n ≥ 5 samples per
   // voxel to converge; with fewer frames, Phase B falls back to robust
   // stats only and distributional modelling is skipped.  Surface this
   // so users understand the quality cliff they're on.
   if ( light_paths.size() < 5 )
   {
      progress.message( String().Format(
         "** WARNING ** Only %zu enabled light frame(s).  Distribution "
         "fitting needs n >= 5; stacking will fall back to robust "
         "statistics per voxel and skip Student-t / contamination "
         "modelling.  Add more frames for full Phase B output quality.",
         light_paths.size() ).ToUTF8().c_str() );
   }

   // Configure stacking engine
   nukex::StackingEngine::Config config;
   config.cache_dir = cacheDirectory.ToUTF8().c_str();
   config.gpu_config.force_cpu_fallback = !enableGPU;
   config.qe_override_path = qeOverridePath.ToUTF8().c_str();
   config.qe_database_path = ResolveQEDatabasePath();
   // Shipped QE database lives beside the module install: <base>/share/qe_database.json.

   // Filter names this user has taught NukeX, beside the Phase 8 state.
   const std::string alias_path = QEUserDataRoot() + "/filter_aliases.json";
   config.filter_alias_path = alias_path;

   // Execute pipeline with progress reporting
   nukex::StackingEngine engine( config );
   auto result = engine.execute( light_paths, flat_paths, &progress );

   // An unrecognised FILTER stops the batch rather than guessing, which is
   // right, but FITS FILTER values are whatever the capture software wrote and
   // no shipped table can list them all. Offer to learn this one, then run
   // again -- once. A second failure is shown as itself, not asked about
   // again.
   if ( !result.ok && !result.unknown_filter.empty()
        && std::getenv( "NUKEX_PHASE8_NO_POPUP" ) == nullptr )
   {
      FilterDialog dlg( result.unknown_filter );
      auto taught = dlg.Run();
      if ( taught.saved )
      {
         nukex::FilterAliasStore aliases;
         aliases.load( alias_path );          // a missing file is fine
         aliases.set( result.unknown_filter, taught.canonical );
         if ( aliases.save( alias_path ) )
         {
            progress.message( String().Format(
               "Filter '%s' recorded as %s in %s. Re-stacking.",
               result.unknown_filter.c_str(), taught.canonical.c_str(),
               alias_path.c_str() ).ToUTF8().c_str() );
            nukex::StackingEngine retry( config );
            result = retry.execute( light_paths, flat_paths, &progress );
         }
         else
         {
            progress.message( String().Format(
               "** Could not write %s, so the filter was not remembered.",
               alias_path.c_str() ).ToUTF8().c_str() );
         }
      }
   }

   // Check cancellation
   if ( progress.is_cancelled() )
   {
      progress.message( "** Cancelled by user." );
      return false;
   }

   // The engine refused the batch. It says why, and the message is written to
   // be acted on, so the job here is only to add the install hint when the
   // install is genuinely the problem.
   //
   // This used to append "share/qe_database.json is missing from the plugin
   // install" to EVERY failure. A user whose filter simply was not in the
   // database -- one line after the console announced updating that same
   // database -- was told to go and check their installation. Decide it from
   // the file rather than guessing.
   if ( !result.ok )
   {
      const std::string db_path = ResolveQEDatabasePath();
      const bool db_present = std::filesystem::exists( db_path );

      String msg = String().Format( "** %s", result.error.c_str() );
      if ( !db_present )
         msg += String().Format(
            "\n** The quantum-efficiency database is not where NukeX expects it "
            "(%s). The release package was not fully installed -- re-install "
            "NukeX from its repository, or point the QE override file at a copy.",
            db_path.c_str() );
      progress.message( msg.ToUTF8().c_str() );
      return false;
   }

   if ( result.n_frames_processed == 0 )
   {
      progress.message( "** No frames were processed. Check input files." );
      return false;
   }

   // Publish result counts to PJSR-readable output parameters before logging.
   nFramesProcessed        = result.n_frames_processed;
   nFramesFailedAlignment  = result.n_frames_failed_alignment;
   nFramesRejectedFilter   = result.n_frames_rejected_filter;

   progress.message( String().Format(
      "Stacking complete: %d frame(s) processed, %d failed alignment, %d rejected (unknown filter)",
      result.n_frames_processed, result.n_frames_failed_alignment,
      result.n_frames_rejected_filter ).ToUTF8().c_str() );

   // Warn if any frames were dropped due to unknown filter coverage.
   if ( result.n_frames_rejected_filter > 0 )
   {
      progress.message( String().Format(
         "** WARNING ** %d frame(s) rejected: unknown FILTER value on Bayer frame. "
         "Add the filter to qe_overrides.json to recover those frames in a future run.",
         result.n_frames_rejected_filter ).ToUTF8().c_str() );
   }

   // Loud warnings on low alignment success rate.  Before v4.0.0.5,
   // a broken matcher silently weight-penalised 61/65 frames and the
   // only indicator was a buried summary line — the whole class of
   // "it ran, but most of my signal was at half weight" regression
   // can't be allowed to be quiet again.
   //
   // Rate is computed over frames that actually reached the aligner
   // (n_frames_processed). Filter-rejected frames (n_frames_rejected_filter)
   // are excluded — they never touched the aligner, so counting them
   // here would falsely inflate the alignment-failure percentage.
   //
   // Thresholds:
   //   > 50% failed  → ** CRITICAL ** with remediation hint
   //   10–50% failed → ** WARNING  ** so users notice but can decide
   //   ≤ 10% failed  → no warning (some drift / clouded frames is normal)
   if ( result.n_frames_processed > 0 )
   {
      double pct = 100.0 * double( result.n_frames_failed_alignment )
                        / double( result.n_frames_processed );
      if ( pct > 50.0 )
      {
         progress.message( String().Format(
            "** CRITICAL ** %.1f%% of frames (%d of %d) failed alignment. "
            "Stacked result uses weight-penalised frames; SNR is significantly "
            "degraded.  Check that the first light frame is a reasonable "
            "reference (no clouds, well-tracked, enough stars), and that all "
            "lights cover a similar field.",
            pct, result.n_frames_failed_alignment,
            result.n_frames_processed ).ToUTF8().c_str() );
      }
      else if ( pct > 10.0 )
      {
         progress.message( String().Format(
            "** WARNING ** %.1f%% of frames (%d of %d) failed alignment and "
            "are stacked at 0.5x weight penalty.  Review the per-frame 'aligned'"
            " log lines above for which frames are affected.",
            pct, result.n_frames_failed_alignment,
            result.n_frames_processed ).ToUTF8().c_str() );
      }
   }

   // Create output ImageWindow with the stacked result
   if ( !result.stacked.empty() )
   {
      int w = result.stacked.width();
      int h = result.stacked.height();
      int nc = result.stacked.n_channels();

      ImageWindow window( w, h, nc,
                          32,    // bits per sample (float32)
                          true,  // float sample
                          nc >= 3, // color if 3+ channels
                          true,  // initialProcessing
                          "NukeX_stacked" );

      View view = window.MainView();
      ImageVariant v = view.Image();

      if ( v.IsFloatSample() && v.BitsPerSample() == 32 )
      {
         pcl::Image& img = static_cast<pcl::Image&>( *v );
         for ( int ch = 0; ch < nc; ch++ )
         {
            const float* src = result.stacked.channel_data( ch );
            float* dst = img.PixelData( ch );
            ::memcpy( dst, src, w * h * sizeof( float ) );
         }
      }

      // Provenance: stamp the FITS header so this window round-trips
      // through Save/Load with NukeX identity, version, and the run's
      // alignment-result counters intact.
      window.SetKeywords( base_output_keywords(
          NUKEX_VERSION_STRING, "stacked",
          result.n_frames_processed, result.n_frames_failed_alignment ) );

      window.Show();
      progress.message( "Stacked image opened." );
   }

   // ── ColorComposer-driven 3-channel sRGB output (Task 12) ─────
   //
   // Compose a 3-channel sRGB ImageWindow from the Phase B derived semantic
   // slots (result.derived). ColorComposer turns each pixel's slot tuple
   // into an sRGB triplet via Lab/LCH composition. This is the primary
   // color-science display for v5; it coexists with the raw stacked window
   // and the stretch pipeline output below (which still reads result.stacked
   // for broadband/mono luminance work — kept for backwards compatibility).
   if ( !result.derived.slots.empty() )
   {
      const int w = result.derived.width;
      const int h = result.derived.height;
      const int N = w * h;

      nukex::ColorComposer composer;

      // Pre-resolve slot data pointers — looking up by name once per pixel
      // would re-hash the unordered_map 7 times × 24M pixels at typical sizes.
      auto slot_ptr = [&]( const std::string& name ) -> const float* {
         auto it = result.derived.slots.find( name );
         return it == result.derived.slots.end() ? nullptr : it->second.data();
      };
      const float* L_p    = slot_ptr( "L"    );
      const float* R_p    = slot_ptr( "R"    );
      const float* G_p    = slot_ptr( "G"    );
      const float* B_p    = slot_ptr( "B"    );
      const float* Ha_p   = slot_ptr( "Ha"   );
      const float* OIII_p = slot_ptr( "OIII" );
      const float* SII_p  = slot_ptr( "SII"  );

      // Chroma gate, driven by the stack's own statistics rather than a
      // constant. Normalising the emission chrominance by its total makes
      // hue a line ratio (Lupton et al. 2004), but it also means blank sky
      // would normalise to a bold colour. So chrominance vanishes at the
      // measured sky level and reaches full strength three robust sigma
      // above it. Sampled on a stride -- a median over every pixel of a
      // 24 MP frame would copy ~200 MB to produce one number.
      if ( Ha_p || OIII_p || SII_p )
      {
         const int stride = std::max( 1, N / 200000 );
         std::vector<double> samples;
         samples.reserve( static_cast<std::size_t>( N / stride ) + 1 );
         for ( int p = 0; p < N; p += stride )
         {
            double tw = 0.0;
            if ( Ha_p   ) tw += std::max( 0.0, static_cast<double>( Ha_p[p]   ) );
            if ( OIII_p ) tw += std::max( 0.0, static_cast<double>( OIII_p[p] ) );
            if ( SII_p  ) tw += std::max( 0.0, static_cast<double>( SII_p[p]  ) );
            samples.push_back( tw );
         }
         if ( !samples.empty() )
         {
            const std::size_t mid = samples.size() / 2;
            std::nth_element( samples.begin(), samples.begin() + mid, samples.end() );
            const double median = samples[mid];
            for ( double& v : samples )
               v = std::fabs( v - median );
            std::nth_element( samples.begin(), samples.begin() + mid, samples.end() );
            const double mad = samples[mid];
            // 1.4826 converts MAD to a Gaussian-equivalent sigma.
            const double sigma = 1.4826 * mad;
            if ( sigma > 0.0 )
            {
               composer.set_chroma_gate( median, median + 3.0*sigma );
               Console().WriteLn( String().Format(
                  "Chroma gate: sky %.6f, full colour at %.6f (3 sigma).",
                  median, median + 3.0*sigma ) );
            }
            else
            {
               // A constant emission field has no noise scale to gate on.
               // Say so rather than silently leaving colour ungated.
               Console().WarningLn(
                  "Chroma gate disabled: emission slots have zero spread, "
                  "so no sky level could be measured." );
            }
         }
      }

      ImageWindow cw( w, h, /*nc*/ 3,
                      32, true, true, true,
                      "NukeX_composed" );
      View cv = cw.MainView();
      ImageVariant cvi = cv.Image();

      if ( cvi.IsFloatSample() && cvi.BitsPerSample() == 32 )
      {
         pcl::Image& ci = static_cast<pcl::Image&>( *cvi );
         float* dst_r = ci.PixelData( 0 );
         float* dst_g = ci.PixelData( 1 );
         float* dst_b = ci.PixelData( 2 );
         for ( int p = 0; p < N; ++p )
         {
            nukex::DerivedSlots ds;
            if ( L_p    ) ds.L    = static_cast<double>( L_p[p]    );
            if ( R_p    ) ds.R    = static_cast<double>( R_p[p]    );
            if ( G_p    ) ds.G    = static_cast<double>( G_p[p]    );
            if ( B_p    ) ds.B    = static_cast<double>( B_p[p]    );
            if ( Ha_p   ) ds.Ha   = static_cast<double>( Ha_p[p]   );
            if ( OIII_p ) ds.OIII = static_cast<double>( OIII_p[p] );
            if ( SII_p  ) ds.SII  = static_cast<double>( SII_p[p]  );
            nukex::sRGBPixel out = composer.compose_pixel( ds );
            dst_r[p] = static_cast<float>( out.r );
            dst_g[p] = static_cast<float>( out.g );
            dst_b[p] = static_cast<float>( out.b );
         }
      }

      // Provenance keywords: include gamut-clip diagnostic so users can
      // compare palette choices and see how many pixels needed clipping.
      pcl::FITSKeywordArray cw_ka = base_output_keywords(
          NUKEX_VERSION_STRING, "composed",
          result.n_frames_processed, result.n_frames_failed_alignment );
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_GAMUT_CLIPPED",
          pcl::IsoString().Format( "%lld",
              static_cast<long long>( composer.gamut_clipped_count() ) ),
          "Pixels clipped to sRGB gamut by ColorComposer" ) );
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_QE_CONFIDENCE",
          result.qe_generic_camera_fallback ? "generic-fallback" : "database",
          "QE source for Phase B: camera entry or generic Sony OSC fallback" ) );
      // A camera database that can update itself would otherwise make a
      // changed result unexplainable: same frames, different pixels, no
      // record of why. This keyword is that record.
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_QE_DB_VERSION",
          pcl::IsoString().Format( "%d", ActiveQEDatabaseVersion() ),
          "QE camera database version used for the Phase B solve" ) );
      cw.SetKeywords( cw_ka );

      cw.Show();
      progress.message( String().Format(
         "Composed image opened (%lld pixel(s) gamut-clipped).",
         static_cast<long long>( composer.gamut_clipped_count() ) ).ToUTF8().c_str() );
   }

   // ── Stretch pipeline (Phase 7 wiring + Phase 8 context) ──────
   //
   // Phase 8 Layer 3 → Layer 2 → Layer 1 fallback runs inside build_primary.
   // At v4.0.1.0 ship, Layer 2 ships empty (bootstrap deferred to Phase 8.5)
   // and Layer 3 is empty until the user saves a rating, so this reduces to
   // factory defaults — preserving bit-identical output vs v4.0.0.8.
   if ( !result.stacked.empty() && !light_paths.empty() )
   {
      nukex::FrameMetadata meta = nukex::FITSReader::read_headers( light_paths.front() );

      // Resolve Phase 8 file paths. user_data_root is where per-user rating
      // DB + trained-model JSON live; share_root is where the read-only
      // bootstrap ships (absent today — LayerLoader falls back cleanly).
      const char* home = std::getenv( "HOME" );
      const std::string user_data_root =
          home ? std::string( home ) + "/.config" : std::string( "/tmp" );
      const std::string share_root = PIShareRoot();

      auto paths = nukex::learning::resolve_user_data_paths( user_data_root, share_root );

      nukex::LayerLoader layer_loader( paths.bootstrap_model_json,
                                       paths.user_model_json );
      nukex::ImageStats  stats = nukex::compute_image_stats( result.stacked );
      nukex::Phase8Context p8{ &layer_loader, &stats };

      std::string auto_log;
      auto primary_op   = nukex::build_primary(
          static_cast<nukex::PrimaryStretch>( primaryStretch ), meta, auto_log, &p8 );
      auto finishing_op = nukex::build_finishing(
          static_cast<nukex::FinishingStretch>( finishingStretch ) );

      if ( !auto_log.empty() )
         progress.message( auto_log.c_str() );

      // Solve the stretch intensity against THIS image rather than shipping
      // one number for every target. Measured across the regression corpora
      // the linear background spans 0.0166 to 0.1479 -- nearly an order of
      // magnitude -- so a single log_D put one stack's median at 0.08 and
      // another's at 0.43. Raising it globally is not the answer either: it
      // brightens but flattens, and on the mono corpus the spread between the
      // median and the 99.9th percentile FALLS from 0.191 to 0.073 as log_D
      // goes 2 to 5.
      if ( primary_op != nullptr && primary_op->name == "VeraLux" )
      {
         if ( auto* vl = dynamic_cast<nukex::VeraLuxStretch*>( primary_op.get() ) )
         {
            const float solved = vl->auto_tune( result.stacked );
            progress.message( pcl::String().Format(
               "Stretch intensity solved for this image: log_D = %.2f "
               "(background target 0.25).", solved ).ToUTF8().c_str() );
         }
      }

      // Capture last-run state BEFORE the unique_ptr is moved into the
      // pipeline, so we can read op.name / op.get_param() cheaply. The
      // rating dialog (below) and Task 18's "Rate last run" button both
      // depend on lastRun being populated for every successful Execute.
      if ( primary_op )
      {
         lastRun.valid               = true;
         lastRun.stats               = stats;
         lastRun.stretch_name        = primary_op->name;
         {
            nukex::FilterClassifier classifier;
            lastRun.filter_class = filter_class_to_rating_int( classifier.classify( meta ).cls );
         }
         lastRun.target_class        = 0; // TODO(Phase 8.5): FITS OBJECT -> class
         lastRun.params_json_applied = op_trainable_params_json( *primary_op );
         // Fresh 128-bit run id. std::rand is not seeded anywhere in NukeX
         // and would repeat across PI launches, so Task 19's DB would get
         // silent primary-key collisions; use random_device-seeded mt19937
         // instead. ExecuteGlobal is UI-thread only, so the static RNG is safe.
         {
            static std::mt19937_64 rng{ std::random_device{}() };
            std::uniform_int_distribution<int> byte_dist( 0, 255 );
            for ( auto& byte : lastRun.run_id )
               byte = static_cast<std::uint8_t>( byte_dist( rng ) );
         }
         lastRun.created_at_unix     =
             std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch() ).count();
         lastRun.user_db_path         = paths.user_db;
         lastRun.user_model_json_path = paths.user_model_json;
      }
      else
      {
         lastRun.valid = false;
      }

      // Deep copy — stretch is in-place; must not mutate result.stacked.
      // Safe: nukex::Image stores pixels in std::vector<float>, so operator=
      // performs a full element-wise deep copy (no shared buffer).
      nukex::Image stretched = result.stacked;

      nukex::StretchPipeline pipeline;
      if ( primary_op )
      {
         primary_op->enabled  = true;
         primary_op->position = 0;
         pipeline.ops.push_back( std::move( primary_op ) );
      }
      if ( finishing_op )
      {
         finishing_op->enabled  = true;
         finishing_op->position = 1;
         pipeline.ops.push_back( std::move( finishing_op ) );
      }
      pipeline.execute( stretched );

      int sw  = stretched.width();
      int sh  = stretched.height();
      int snc = stretched.n_channels();

      ImageWindow sw_win( sw, sh, snc, 32, true, snc >= 3, true, "NukeX_stretched" );
      View sv = sw_win.MainView();
      ImageVariant svi = sv.Image();
      if ( svi.IsFloatSample() && svi.BitsPerSample() == 32 )
      {
         pcl::Image& si = static_cast<pcl::Image&>( *svi );
         for ( int ch = 0; ch < snc; ch++ )
         {
            const float* src = stretched.channel_data( ch );
            float* dst = si.PixelData( ch );
            ::memcpy( dst, src, sw * sh * sizeof( float ) );
         }
      }
      // Provenance on the stretched window: include the primary +
      // finishing stretch enum labels so a user inspecting the saved
      // FITS later can tell which curve produced the image.
      pcl::FITSKeywordArray sw_ka = base_output_keywords(
          NUKEX_VERSION_STRING, "stretched",
          result.n_frames_processed, result.n_frames_failed_alignment );
      static const char* kPrimaryNames[] = {
          "Auto", "VeraLux", "GHS", "MTF", "ArcSinh", "Log", "Lupton", "CLAHE"
      };
      const int pe = static_cast<int>( primaryStretch );
      const int ne = static_cast<int>(
          sizeof(kPrimaryNames) / sizeof(kPrimaryNames[0]) );
      const char* primary_name =
          ( pe >= 0 && pe < ne ) ? kPrimaryNames[pe] : "Unknown";
      sw_ka.Append( pcl::FITSHeaderKeyword(
          "PRIMSTR", pcl::IsoString( "'" ) + primary_name + "'",
          "Primary stretch curve applied" ) );
      sw_ka.Append( pcl::FITSHeaderKeyword(
          "FINSTR", pcl::IsoString( "'None'" ),
          "Finishing stretch (only None enrolled today)" ) );
      sw_win.SetKeywords( sw_ka );

      sw_win.Show();
      progress.message( "Stretched image opened." );

      // ── Phase 8 rating popup ─────────────────────────────────
      // Skip unconditionally when:
      //   (a) NUKEX_PHASE8_NO_POPUP is set — headless / E2E harness
      //   (b) the user has opted out via the "don't show again" checkbox
      //       (Task 18 persists this through PCL Settings)
      // Otherwise, open the dialog and let Task 19 persist anything the
      // user clicks Save on.  save_rating_from_last_run is currently a
      // no-op stub so the E2E regression is unaffected.
      const bool headless = ( std::getenv( "NUKEX_PHASE8_NO_POPUP" ) != nullptr );
      if ( lastRun.valid && !headless
           && TheNukeXProcess != nullptr
           && !TheNukeXProcess->rating_popup_suppressed() )
      {
         pcl::RatingDialog dlg( lastRun.filter_class );
         pcl::RatingResult res = dlg.Run();
         if ( res.dont_show_again )
            TheNukeXProcess->set_rating_popup_suppressed( true );
         if ( res.saved )
            SaveRatingFromLastRun( res );
      }
   }

   // Create noise map window
   if ( !result.noise_map.empty() )
   {
      int w = result.noise_map.width();
      int h = result.noise_map.height();
      int nc = result.noise_map.n_channels();

      ImageWindow nw( w, h, nc, 32, true, nc >= 3, true, "NukeX_noise" );
      View nv = nw.MainView();
      ImageVariant nvi = nv.Image();

      if ( nvi.IsFloatSample() && nvi.BitsPerSample() == 32 )
      {
         pcl::Image& ni = static_cast<pcl::Image&>( *nvi );
         for ( int ch = 0; ch < nc; ch++ )
         {
            const float* src = result.noise_map.channel_data( ch );
            float* dst = ni.PixelData( ch );
            ::memcpy( dst, src, w * h * sizeof( float ) );
         }
      }

      nw.SetKeywords( base_output_keywords(
          NUKEX_VERSION_STRING, "noise",
          result.n_frames_processed, result.n_frames_failed_alignment ) );

      nw.Show();
      progress.message( "Noise map opened." );
   }

   progress.message( "NukeX done." );
   return true;
}

// ── Parameter serialization ──────────────────────────────────────

void* NukeXInstance::LockParameter( const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXLightFramePathParameter )    return lightFrames[tableRow].path.Begin();
   if ( p == TheNXLightFrameEnabledParameter ) return &lightFrames[tableRow].enabled;
   if ( p == TheNXFlatFramePathParameter )     return flatFrames[tableRow].path.Begin();
   if ( p == TheNXFlatFrameEnabledParameter )  return &flatFrames[tableRow].enabled;
   if ( p == TheNXPrimaryStretchParameter )    return &primaryStretch;
   if ( p == TheNXFinishingStretchParameter )  return &finishingStretch;
   if ( p == TheNXEnableGPUParameter )         return &enableGPU;
   if ( p == TheNXCacheDirectoryParameter )         return cacheDirectory.Begin();
   if ( p == TheNXQEOverridePathParameter )         return qeOverridePath.Begin();
   if ( p == TheNXNFramesProcessedParameter )       return &nFramesProcessed;
   if ( p == TheNXNFramesFailedAlignmentParameter ) return &nFramesFailedAlignment;
   if ( p == TheNXNFramesRejectedFilterParameter )  return &nFramesRejectedFilter;
   return nullptr;
}

bool NukeXInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXLightFramesParameter )
   {
      lightFrames.Clear();
      if ( sizeOrLength > 0 )
         lightFrames.Add( FrameItem(), sizeOrLength );
   }
   else if ( p == TheNXLightFramePathParameter )
   {
      lightFrames[tableRow].path.Clear();
      if ( sizeOrLength > 0 )
         lightFrames[tableRow].path.SetLength( sizeOrLength );
   }
   else if ( p == TheNXFlatFramesParameter )
   {
      flatFrames.Clear();
      if ( sizeOrLength > 0 )
         flatFrames.Add( FrameItem(), sizeOrLength );
   }
   else if ( p == TheNXFlatFramePathParameter )
   {
      flatFrames[tableRow].path.Clear();
      if ( sizeOrLength > 0 )
         flatFrames[tableRow].path.SetLength( sizeOrLength );
   }
   else if ( p == TheNXCacheDirectoryParameter )
   {
      cacheDirectory.Clear();
      if ( sizeOrLength > 0 )
         cacheDirectory.SetLength( sizeOrLength );
   }
   else if ( p == TheNXQEOverridePathParameter )
   {
      qeOverridePath.Clear();
      if ( sizeOrLength > 0 )
         qeOverridePath.SetLength( sizeOrLength );
   }
   else
      return false;

   return true;
}

size_type NukeXInstance::ParameterLength( const MetaParameter* p, size_type tableRow ) const
{
   if ( p == TheNXLightFramesParameter )       return lightFrames.Length();
   if ( p == TheNXLightFramePathParameter )    return lightFrames[tableRow].path.Length();
   if ( p == TheNXFlatFramesParameter )        return flatFrames.Length();
   if ( p == TheNXFlatFramePathParameter )     return flatFrames[tableRow].path.Length();
   if ( p == TheNXCacheDirectoryParameter )    return cacheDirectory.Length();
   if ( p == TheNXQEOverridePathParameter )    return qeOverridePath.Length();
   return 0;
}

} // namespace pcl
