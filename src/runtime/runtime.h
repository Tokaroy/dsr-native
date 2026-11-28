#pragma once
// Process-wide state for the native runtime: loaded modules, configured paths, guest allocator hooks.

#include <cstdint>
#include <memory>
#include <string>

#include "loader/module.h"
#include "loader/nso.h"

namespace dsr {

struct RuntimeConfig {
    std::string game_dir;     // directory holding exefs/ and the romfs contents (dvdbnd*.bdt, ...)
    std::string save_dir;     // writable directory for save data
    std::string cache_dir;    // writable directory for caches (shaders etc.)
    std::string cheats_dir;   // cheat files (<build id>.txt, runtime/cheats.h); empty = no cheats
    bool dry_run = false;     // map + relocate + bind only, never execute guest code
    bool abort_on_unimplemented = false;
    bool docked = false;      // nn::oe::GetOperationMode: false = handheld (720p), true = console
    std::string language = "en-US";
    // User settings (settings.txt next to the game folder on Android).
    int target_fps = 60;          // presentation pace: 30 (the Switch rate) .. 60; with dynamic_fps the highest rate
    // fps = auto: the cap follows what the device sustains (60, 30, 20 fps, not below min_fps), the game's time step
    // follows the frame time, so the game keeps its speed (dynamic fps, after PR #13).
    bool dynamic_fps = false;
    int min_fps = 20;             // lowest rate fps = auto may use (15..30); also the longest game time step
    // fps = unlocked (experimental): paced at the screen's highest refresh rate (display_hz, 60..144) instead of at
    // most 60. The game's time step follows the frame time down to that period; above 60 its physics may misbehave.
    bool unlocked_fps = false;
    int display_hz = 60;          // the screen's highest refresh rate at its resolution (from the activity)
    bool async_pipelines = true;  // compile new GPU pipelines off the render thread (no hitch; the draw waits a frame)
    bool auto_capture = false;    // diagnostics: save a few frame captures automatically during play
    // detailed_logs (settings menu "Detailed diagnostic files", off by default): on = also the shader dump,
    // auto_capture (and the L3 + R3 capture), and every file kept; off = the run log (Info level) and the logcat
    // mirror, and only the 10 newest starts' log files kept in Download/DSRNative/logs.
    bool detailed_logs = false;
    // mali_tile_turbo (experimental, Mali GPUs only): image layout transitions scoped to the stages and accesses their
    // layouts name, instead of waiting for / blocking all GPU work (vk_backend.cpp, LayoutScope).
    bool mali_tile_turbo = false;
    // late_depth (experimental, tile-based GPUs): when shaders read the depth buffer of the pass being drawn (soft
    // particles, fog: about once a frame), they get the copy made when that pass last ended (a frame old at first)
    // instead of the pass being split for a copy. May show slight edge errors on soft particles in fast motion.
    bool late_depth = false;
    bool profile = true;          // diagnostics: sampling CPU profile of the busy threads, logged every 10 s
    float gamma = 1.0f;           // extra display gamma applied through the game's brightness ramp (> 1 lifts shadows)
    // mailbox_pacing: with present_mode mailbox, each frame is held back to finish half a refresh before the refresh
    // it is meant for (vk_backend.cpp, MailboxHold): no dropped or repeated frames, still no queue. 0 = plain mailbox.
    bool mailbox_pacing = true;
    // mailbox_margin: how long before its refresh a paced frame is finished, in ms (0 = half a refresh). The
    // compositor takes the newest frame for a refresh some time before it: frames should finish in the middle
    // between two such moments. 10 was the best of 4 / 8 / 10 / 12 / 14 on a Thor at 60 Hz (4 and 14 stuttered).
    double mailbox_margin_ms = 10;
    std::string present_mode = "fifo";  // swapchain present mode: fifo (vsync, default), mailbox, fifo_relaxed, immediate (FIFO if unsupported)
    bool lrz_workaround = false;
    bool shader_depth_test = true;  // depth tests against gl_FragDepth-written depth done in the shader (square holes)  // refresh depth buffers written by gl_FragDepth before reuse (square holes)
    int hole_test = 0;  // square-hole diagnostics (bits): 1 = effects without depth test, 2 = uncompressed depth, 4 = no write-masked colour targets, 8 = one render pass per draw into reduced-size targets, 16 = depth textures read from copies, 64 = D24S8 depth, 128 = CPU waits for GPU idle at each slot, 256 = uncompressed colour targets, 512 = no null colour attachments, 1024 = effect shaders output debug values, 2048 = no depth test in reduced-size effect passes
    // Test of the renderer's recovery from a lost GPU (VK_ERROR_DEVICE_LOST): every this many seconds after the first
    // frame, the renderer acts as if the GPU had been lost and restarts it (0 = off).
    int test_gpu_lost = 0;
    bool audio_smooth = false;  // release audio buffers at a smoothed pace (experiment; movies were not timed by it)
    // Older Adreno GPUs: storage usage and mutable format only where needed, so images keep their bandwidth
    // compression. Off by default: purple streaks on the OnePlus 6T (Adreno 630) for ~5% less GPU time.
    bool gpu_compression = false;
    // Aspect ratio of the game's handheld (720p) output: "16:9" (the game's own), "auto" (the screen's, from
    // screen_aspect) or "W:H". Experimental: widens the display resolution the game sets up (main+0x716170).
    std::string aspect_ratio = "16:9";
    float screen_aspect = 0;  // width / height of the screen (Android: the window), 0 = unknown
    bool hud_box = true;      // other aspect ratios: keep the HUD and menus in a centred 16:9 box
    int screen_height = 0;    // the screen's shorter side in pixels (Android: the window), 0 = unknown
    // sharp_hud: with a render scale, the HUD, menus and text drawn at the screen's resolution (up to
    // 1080 lines) instead of the render size (vk_backend.cpp, SharpStart).
    bool sharp_hud = false;
    // Post-processing of the enlarged 3D picture (from pull request #47; turns the sharp-HUD image on by itself):
    // fxaa 0 = off, 1 = FXAA 3.11 fast (for Mali), 2 = quality; fsr = FSR 1 upscaling (EASU + RCAS sharpening) instead
    // of the bilinear enlargement.
    int fxaa = 0;
    bool fsr = false;
    // fsr_half: FSR 1 in half precision (colours and weights at 16 bits, positions at 32: up to twice as fast on
    // Mali and Adreno); 0 = the full-precision shaders.
    bool fsr_half = true;
    // shadow_quality: 0 = no shadows (the shadow map pass is not drawn, lookups say "lit"), 1 = 512, 2 = 1024,
    // 3 = the game's 2048 (1024 with performance_mode).
    int shadow_quality = 3;
    // skip_prepass (experimental, for Mali): the game's depth-only world pass is left out and the colour pass writes
    // the depth instead (vk_backend.cpp, PrepassStat). 1 = all of it; 2 = only its alpha-tested draws (foliage,
    // fences: in the depth-only pass they still run the texture fetch and discard, the expensive part of it), the
    // plain opaque ones stay and keep hiding what is behind them.
    int skip_prepass = 0;
    // skip_hidden (experimental): world draws that covered no pixel in recent frames (occlusion queries) are left
    // out, retested every few frames (vk_backend.cpp, OccDecide). 1 = gentle (nothing left out while the camera moves),
    // 2 = aggressive.
    int skip_hidden = 0;
    int render_width = 1280;  // the game's render size once aspect_ratio and render_scale are applied
    int render_height = 720;
    float render_scale = 1.0f;  // settings render_scale: 0.5 - 1.0 of the game's 720-line resolution
    // GPU driver: settings.txt `gpu_driver` (a file in DSRNative/drivers, or "system"). The Android front end
    // copies (or unzips) it into the app's private folder: gpu_driver_dir (ends in '/') holds gpu_driver_lib.
    // Empty gpu_driver_lib: the system driver.
    std::string gpu_driver = "system";
    std::string gpu_driver_dir, gpu_driver_lib;
    std::string native_lib_dir;  // the app's native library folder (libadrenotools loads its hooks from there)
    bool bc_enabler = true;      // Adreno 6xx system drivers: patch in the BC texture formats (BCeNabler)
    // The swapchain has the game's frame size and the system compositor scales it to the screen, instead of the
    // GPU filling a screen-sized image each frame.
    bool compositor_scaling = true;
    // GPUs without BC textures (Mali; Adreno 6xx without BCeNabler): BC1 textures are re-encoded as ETC2 (same size
    // as BC1) instead of decoded to RGBA8 (4x larger), when the GPU has ETC2 (bc_etc2); BC3 too with bc3_etc2
    // (4x smaller than RGBA8, but lossier: off by default).
    bool bc_etc2 = true;
    bool bc3_etc2 = false;
    // tex_cache_mb (settings menu "Texture disk cache", from pull request #59): size limit (MB) of the on-disk
    // cache of those conversions, ETC2 and ASTC (cache folder, tex_cache.dat/.idx): the next run reads converted
    // textures back instead of encoding them again. 0 = off. Once full, new levels are not stored.
    int tex_cache_mb = 512;
    // bc_astc (experimental; encoder from pull request #47 by fernanfez00): on GPUs with ASTC (Mali) BC1 / BC3 textures
    // are re-encoded as ASTC and BC5 normal maps as ASTC luminance + alpha, instead of ETC2 / decoded RGBA8.
    // astc_block: 4, 6 or 8 (world textures; the HUD, text and small textures stay 4x4).
    bool bc_astc = false;
    int astc_block = 4;
    // mali_bc (experimental, Mali GPUs only; the BC texture support of the malisx2 driver, which samples BC1-7 on
    // Mali-G52 / G57 / G610): 0 = BC textures are used as they are when the driver reports them (else converted as
    // above), 1 = the same after a test of every BC format (formats the driver does not report are then used as
    // well), 2 = never: BC always converted (ASTC / ETC2 / decoded). Other GPUs ignore it.
    int mali_bc = 0;
    // Diagnostics. shadow_compare: how the shadow map lookups compare (0 = the game's function as Vulkan's;
    // 1 = operands swapped; 2 = always 1 (shadowed); 3 = always 0 (lit)).
    int shadow_compare = 0;
    // game_hooks = 0: none of the engine patches (frame rate, resolution, log hooks) are installed.
    bool game_hooks = true;
    // render_pass_objects = 1: VkRenderPass / VkFramebuffer objects even when the driver has dynamic rendering
    // (diagnostics for drivers whose dynamic rendering misbehaves, e.g. Mali flicker reports).
    bool render_pass_objects = false;
    // mali_barriers = 1 (PR #13): narrow layout-transition barriers (destination = the stages that use the image) and
    // render targets made readable when their pass ends, so a tile-based GPU (Mali) can overlap passes. Off by default.
    bool mali_barriers = false;
    // performance_mode = 1: the engine's own lower-quality options: low-quality effects, 1024x1024 shadow maps instead
    // of 2048x2048 (read at start-up). The menu also sets render_scale 0.75 when it is turned on.
    bool performance_mode = false;
    // texture_mip_skip = 1: big BC textures (512x512 and up, with mip levels) are loaded without their most detailed
    // level: half the width and height, ~75% less data to convert, upload and sample; softer up close.
    int texture_mip_skip = 0;
    // pass_load_skip = 0: render passes always load their targets (by default a pass learned to overwrite a target
    // starts without loading it: vk_backend.cpp, PassCoverage).
    bool pass_load_skip = true;
    // staging_memory: the memory the CPU copies each frame's vertex / uniform data into for the GPU. auto = CPU-cached
    // (and GPU-coherent) memory where the driver offers it on Adreno (Turnip's default staging memory is uncached:
    // the render thread's copies cost more than with Qualcomm's driver), else the first host-visible memory;
    // cached / uncached force it either way.
    std::string staging_memory = "auto";
    // recorder_copies = 1: a copy thread copies the draws' uniform and vertex data (vk_backend.cpp, CopyOut). Off by
    // default: on the OnePlus 6T (4 fast cores, throttled) the extra busy thread cost the render thread more than
    // it saved; the render thread copies.
    bool recorder_copies = false;
    // lighter_post_fx = 1: no ambient occlusion, depth of field, camera motion blur or light shafts (the engine's own
    // switches; read at start-up), and no bloom (its passes are skipped by vk_backend.cpp: the engine's bloom switch
    // is never read). The menu turns it on with performance mode on GPUs without BC support.
    bool lighter_post_fx = false;
    // skip_intro = 1: the company logos at the start are skipped (straight to the title movie).
    bool skip_intro = false;
    // geo_cache_mb (settings menu "Geometry cache", from pull request #47): size of the geometry cache (vertex / index
    // data kept on the GPU across frames), 32-512 MB. Smaller uses less RAM but fills sooner (emptied and refilled: a
    // short stall). Performance mode sets 64.
    int geo_cache_mb = 192;
    // geo_watch: geometry ranges that stayed the same are watched for writes (page protection, hle/os/write_watch.h)
    // instead of being hashed again every frame; 0 = hash them every frame as before.
    bool geo_watch = true;
    int max_anisotropy = 0;  // limit on the game's anisotropic texture filtering (0 = the game's own level)
    // anisotropy (settings menu "Anisotropic filtering"): -1 = as the game asks, 0 = off, 2 / 4 / 8 / 16 = that level on
    // every mipmapped, linearly filtered texture (capped at the GPU's limit). Sharper textures at grazing angles
    // (floors, walls, roads) for some GPU time.
    int anisotropy = -1;
    bool fast_cores = true;  // keep the game's frame-critical threads off the efficiency cores
    bool record_thread = true;  // record Vulkan commands on a second thread (draw preparation stays on the render thread)
    bool pin_render_thread = false;  // keep the render thread on the fastest CPU core(s) (slower on 8 Gen 2: off)
    float exposure = 1.37f;       // scales the tone-mapping exposure (1.0 = as the game draws it)
};

struct Runtime {
    RuntimeConfig config;
    loader::NsoImage main_nso;
    loader::NsoImage sdk_nso;          // parsed (not mapped) to source data constants such as nn::util tables
    bool have_sdk = false;
    loader::Module main;

    // Guest allocator entry points exported by main (the game replaces malloc & co).
    uintptr_t guest_malloc = 0, guest_free = 0, guest_calloc = 0, guest_realloc = 0, guest_aligned_alloc = 0;
};

Runtime& GetRuntime();

// Loads main (and optionally sdk for data), binds imports. Returns false with *err on failure.
bool LoadGame(const RuntimeConfig& cfg, std::string* err);

// Runs the guest startup sequence (nninitStartup, static constructors, nnMain) on a dedicated
// thread sized like the Switch main thread. Blocks until nnMain returns.
int RunGame();

// Patches engine functions of this game build (log/panic output, frame rate). Called by LoadGame.
void InstallGameHooks();
// The game version the hooks recognised ("1.0.0", "1.0.3"), or null for an unknown build.
const char* GameVersion();
// The game's current per-frame time step in seconds (1/30 unless the 60 fps patches are active).
float GameFrameTime();
// The game's movie player shows one video frame per game frame (30 fps on the Switch): while a video
// plays, frames are paced at 30 instead of the configured rate (at 60 the video ran at double speed and
// stopped every few frames to wait for its sound).
void SetMoviePlaying(bool playing);
int FrameRateNow();  // the rate the frame limiter paces to now (the cap, or the game version's video rate)
// Smoothed GPU time of one submission in ms (0 = not known yet), from the Vulkan backend's GPU timestamps: what the
// dynamic frame rate (fps = auto) weighs against the frame period.
void ReportGpuFrameTime(double ms);
// present_mode mailbox pacing (vk_backend.cpp, MailboxHold), once per frame: how much earlier than its aim (half a
// refresh before the nearest refresh) the frame will finish, in ms, within half a refresh (negative: later). The
// frame limiter moves the phase of its refresh-grid deadlines by it.
void ReportPresentSlack(double ms);
// The display's last refresh (steady_clock / CLOCK_MONOTONIC ns) and refresh period, from the Vulkan backend's
// AChoreographer thread (present_mode mailbox): the frame limiter then starts frames on the refresh grid.
void ReportDisplayRefresh(int64_t last_ns, int64_t period_ns);
// Diagnostics of the paced frame limiter since the last call (frames on the refresh grid, how many slept and for how
// long, those that came after their deadline, the phase and the slack it follows), for the 10 s report.
std::string TakeLimiterReport();
// The frame limiter is pacing to the display's refreshes (present_mode mailbox pacing reports came lately).
bool DisplayPacingActive();
// Whether the GPU samples BC textures itself (natively or through BCeNabler): 1 yes, 0 no (converted on the CPU),
// -1 not known yet (no Vulkan device). The menu turns texture_mip_skip on with performance mode only without BC.
void SetGpuHasBc(bool has);
int GpuHasBc();
bool FrameRateAdjustable();  // the frame rate patches are in: config.target_fps may change while the game runs
// The rate a fixed cap paces at: target_fps (30..60), or with fps = unlocked the screen's refresh rate (60..144).
inline int FixedFps(const RuntimeConfig& c) {
    if (c.unlocked_fps) return c.display_hz < 60 ? 60 : (c.display_hz > 144 ? 144 : c.display_hz);
    return c.target_fps < 30 ? 30 : (c.target_fps > 60 ? 60 : c.target_fps);
}

// Prints an import binding summary (implemented / unimplemented / data) to the log.
void LogImportReport(bool list_unimplemented);

// Guest heap helpers used by HLE code that must hand memory to the game (strdup, operator new, ...).
void* GuestMalloc(size_t size);
void GuestFree(void* p);
void* GuestAlignedAlloc(size_t align, size_t size);

}  // namespace dsr
