// Hooks on specific functions inside the game's main module, for the game builds listed in kBuilds (matched by
// build id): 1.0.0 and the 1.0.3 update (two editions of it: title 010088F00820E000's and 01004AB00A260000's, the same
// version with different programs). They replace engine functions whose output is useful to us but goes
// nowhere in a retail build, and patch the frame rate and resolution. The places were found in 1.0.0 and matched
// in 1.0.3 by their code (the same instructions around them, with addresses masked).
#include <cerrno>
#include <ucontext.h>
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif
#include <sys/mman.h>
#include <signal.h>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <mutex>
#include <map>

#include "hle/input.h"
#include "common/log.h"
#include "hle/os/heap.h"
#include "hle/os/os.h"
#include <atomic>

#include "runtime/runtime.h"

namespace dsr {
namespace {
float* g_game_step = nullptr;  // the game's per-frame time step (scratch page), when patched

// Where things are in one game build (offsets in main).
struct GameBuild {
    const char* build_id;  // first 16 bytes, hex
    const char* version;
    const char* data_version;  // DataVersion.txt of the RomFS that goes with it
    // Per-frame update: adrp x8 / ldr s8, [x8, #...] loading the constant 1/30 s time step.
    uint64_t step_adrp;
    uint32_t step_adrp_insn, step_ldr_insn;
    // Render thread frame limiter (this, fps) and its state in `this`.
    uint64_t limiter;
    uint64_t limiter_global;  // 1.0.0: skipped while this int (main+) is non-zero; 0 = see limiter_enabled
    uint32_t limiter_enabled;  // 1.0.3: skipped while this byte of `this` is zero
    uint32_t last_tick, skip_active, skip_toggle, skip_time;
    // Resolution defaults (mov w2/w1, #1280 / #720).
    uint64_t width_cfg, width_get, display_width, height_cfg, height_get, display_height;
    // Engine log, log with va_list, DL_PANIC.
    uint64_t log, vlog, panic;
    // Game frame rate while a video plays. The movie player is frame-based: 1.0.0 shows one video frame per game
    // frame (30 fps videos: 30), 1.0.3 one per two game frames (60; at 30 its videos ran at half speed).
    int movie_fps;
    // Performance mode (settings performance_mode): the engine's own quality variables, from its debug menu. (The
    // LOD level bias, 1 on the Switch, is left alone: at 2 nearby walls switched to low-detail models with gaps.)
    //   lowvfx_read:   ldrb w10, [x20, #0x599] reading "Enable Low Quality Vfx" (0) into the effect setup: becomes
    //                  mov w10, #1 (effects through the engine's low-quality resolve).
    //   shadow_size:   the shadow map size global (debug menu "ShadwoMap Size", 512..2048, 2048): set to 1024
    //                  before the game starts; the engine sizes the shadow target and its texel offsets from it.
    uint64_t lowvfx_read, shadow_size;
    // The per-frame dynamic buffer (DLDrawDevice_NXNvn: three, one per frame in flight): its size is
    // `add w8, w8, #0x200, lsl #12` (2 MiB, plus a config value that is 0). A frame that needs more aborts the game
    // ("DL_PANIC: dynamic buffer not big enough"), as in a busy boss fight on a wide screen: made 8 MiB.
    uint64_t dyn_buffer;
    // Lighter post-processing (settings lighter_post_fx): the engine's own switches, from its debug menu, read by
    // the per-frame setup of the renderer (`ldrb` from the graphics settings object, x19):
    //   ao[2]:       "Enable AO" (+0x2ca): the ambient-occlusion switch and the render parameter it sets -> 0
    //   dof:         "Enable DOF" (+0x2e1): passed (inverted) to the image-process manager -> 0 (no depth of field)
    //   camblur[2]:  "Disable Camera Blur" (+0x295), on both paths to the camera motion blur -> 1
    //   lightshaft:  the "LightShaft" draw-group switch (a global, 1) where the pass mask is built -> 0
    uint64_t ao[2], dof, camblur[2], lightshaft;
};
const GameBuild kBuilds[] = {
    {"5a797a3a5da6c4f3468cb7c84e4dde63", "1.0.0", "20180818_462",
     0x66c9c8, 0xB00080A8, 0xBD497108,
     0x70bd90, 0x251fb4c, 0, 0x310, 0x31d, 0x31e, 0x320,
     0x6a170c, 0x6a18f8, 0x716170, 0x6a1758, 0x6a1968, 0x716178,
     0x7fea18, 0x7feb30, 0x7892d8, 30,
     0x3620f4, 0x1aeb890, 0x70b498,
     {0x370014, 0x370088}, 0x370584, {0x36ea4c, 0x36ea5c}, 0x3623e0},
    {"6569410c91c398e78ec76b418ee4cca8", "1.0.3", "20181026_101",
     0x667930, 0xB0008068, 0xBD455908,
     0x706d28, 0, 0x2f7, 0x2e8, 0x2f5, 0x2f6, 0x2f8,
     0x69c638, 0x69c824, 0x711ff0, 0x69c684, 0x69c894, 0x711ff8,
     0x7f71d4, 0x7f72ec, 0x782dac, 60,
     0x356790, 0x1adf890, 0x7065f4,
     {0x364494, 0x364508}, 0x364a04, {0x362f3c, 0x362f4c}, 0x356a7c},
    // 1.0.3 of title 01004AB00A260000 (update v196608, buildinfo region DV): the same game and data as the other
    // 1.0.3 with its code moved; matched from it, and the limiter's fields are the same.
    {"df3766a2bb651a3e822828abf10b6f7b", "1.0.3", "20181026_101",
     0x667900, 0xF0008088, 0xBD46ED08,
     0x707090, 0, 0x2f7, 0x2e8, 0x2f5, 0x2f6, 0x2f8,
     0x69c608, 0x69c7f4, 0x711ec8, 0x69c654, 0x69c864, 0x711ed0,
     0x7f9ec8, 0x7f9fe0, 0x784660, 60,
     0x356790, 0x1ae3890, 0x7067ac,
     {0x364494, 0x364508}, 0x364a04, {0x362f3c, 0x362f4c}, 0x356a7c},
};
const GameBuild* g_build = nullptr;

// Skip intro logos (settings skip_intro). The title screen's step machine (TitleStep / TitleStep_forNS, step
// number at this+0xc) opens the company logo dialog (menu dialog 60, FrpgMenuDlgLogo) by setting its request word
// to 1 (`bl` to set(60, 1)) and then waits until the dialog has cleared it again after its last logo. Both classes'
// open calls become `nop`, so the wait passes at once, as if the logos had finished. TitleStep_forNS also counts
// down a timer (this+0xe0) before closing the dialog: that step branches straight to its close path. Everything
// else (loading, the title movie, press start) runs as before.
struct LogoSkip {
    const char* build_id;  // first hex digits of the build (GameBuild::build_id)
    uint64_t open[2];
    uint32_t open_insn[2];  // bl set_dialog_request (60, 1)
    uint64_t timer;         // ldr s1, [x19, #0xe0]
    uint64_t timer_done;    // the timer-expired path
};
const LogoSkip kLogoSkips[] = {
    {"5a797a3a", {0x6f1c30, 0x6f73e8}, {0x97fa3d6c, 0x97fa277e}, 0x6f7450, 0x6f746c},
    {"6569410c", {0x6ed0e4, 0x6f27fc}, {0x97fa3510, 0x97fa1f4a}, 0x6f2864, 0x6f2880},
    {"df3766a2", {0x6ecf7c, 0x6f268c}, {0x97fa355e, 0x97fa1f9a}, 0x6f26f4, 0x6f2710},
};
constexpr uint32_t kNop = 0xD503201F, kLdrS1Timer = 0xBD40E261;

// Sprint "graze" check (frame-rate fix). Every update the game measures how far the player's character moved since
// the previous update and, if it was less than 1/30 of a unit (`fmov #30`, multiply, compare with 1: below
// 1 unit per second at the game's own 30 updates a second), takes it to be running into a wall and slows the
// movement down: a speed factor (move control +0x1a8, 0.5..1) is multiplied by 0.8 per update, and by 1.2 per update
// while moving again. The test is per update, so at a higher frame rate an update covers less ground: at 60 fps the
// threshold is 2 units per second, at 120 fps 4, which a sprint (once slowed down: for good) no longer reaches -
// the character then sprints at half speed, slower than walking. (Found from the high frame rate fix of
// DS1 FPS Unlock for the PC version, MIT licence, github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered: "FixGraze".)
// The three constants now come from the scratch page and are set every frame from the game's time step (by
// FrameLimiter): 1/step, 0.8^(30 step), 1.2^(30 step): the same test and the same slow-down / recovery per second
// at every frame rate (at 30 fps exactly the game's).
struct GrazeFix {
    const char* build_id;
    // 1.0.3 (optimised code): fmov s0,#30 / fmul s0,s0,v1.s[0] / fmov s1,wzr / tst w8,#1 / add x8,x19,#0x1a8 at
    // `mul`, the result stored by str s0,[x8] at `store`. Rewritten as adrp x9,scratch / ldr s0,[x9,#4] / fmul /
    // fmov / tst, the store as str s0,[x19,#0x1a8] (x8 is only its address, x9 is loaded again before it is used).
    // 1.0.0 (unoptimised): fmov s1,#30 / fmul s0,s0,s1 / stur s0,[x29,#-0x18] / ldur s0,[x29,#-0x18] at `mul`:
    // adrp x9,scratch / ldr s1,[x9,#4] / fmul / stur (the reload of the value just stored is dropped).
    bool optimised;
    uint64_t mul, store;
    uint64_t decay, recover;  // adrp x9,page / ldr s1,[x9,#lo] loading 0.8 and 1.2: now the scratch page's
    uint32_t decay_insn[2], recover_insn[2];
};
const GrazeFix kGrazeFixes[] = {
    {"5a797a3a", false, 0x2a3be8, 0, 0x2a3c48, 0x2a3c90, {0xD0009EE9, 0xBD48CD21}, {0xD0009EE9, 0xBD495521}},
    {"6569410c", true, 0x29c874, 0x29c8d4, 0x29c898, 0x29c8bc, {0x90009EC9, 0xBD44B521}, {0x90009EC9, 0xBD453D21}},
    {"df3766a2", true, 0x29c874, 0x29c8d4, 0x29c898, 0x29c8bc, {0xD0009EE9, 0xBD464921}, {0xD0009EE9, 0xBD46D121}},
};
float* g_graze = nullptr;  // scratch page: [0] 1/step, [1] slow-down, [2] recovery per update

void SetGrazeFactors(float step) {
    if (!g_graze || !(step > 0.0f)) return;
    g_graze[0] = 1.0f / step;
    g_graze[1] = std::pow(0.8f, 30.0f * step);
    g_graze[2] = std::pow(1.2f, 30.0f * step);
}

void ApplyGrazeFix(Runtime& rt, float* graze) {
    const GrazeFix* f = nullptr;
    for (const GrazeFix& g : kGrazeFixes)
        if (rt.main_nso.build_id_hex.rfind(g.build_id, 0) == 0) f = &g;
    if (!f) return;
    const uintptr_t page = reinterpret_cast<uintptr_t>(graze);
    const uint32_t off = uint32_t(page & 0xfff);  // 4 (after the time step)
    auto ldr_s = [&](int rt_, uint32_t byte_off) { return 0xBD400000u | ((byte_off / 4) << 10) | (9u << 5) | uint32_t(rt_); };
    struct P { uint64_t at; uint32_t from, to; };
    std::vector<P> p;
    if (f->optimised) {
        p = {{f->mul, 0x1E27D000, rt.main.EncodeAdrp(f->mul, 9, page)},
             {f->mul + 4, 0x5F819000, ldr_s(0, off)},
             {f->mul + 8, 0x1E2703E1, 0x5F819000},
             {f->mul + 12, 0x7200011F, 0x1E2703E1},
             {f->mul + 16, 0x9106A268, 0x7200011F},
             {f->store, 0xBD000100, 0xBD01AA60}};  // str s0,[x8] -> str s0,[x19,#0x1a8]
    } else {
        p = {{f->mul, 0x1E27D001, rt.main.EncodeAdrp(f->mul, 9, page)},
             {f->mul + 4, 0x1E210800, ldr_s(1, off)},
             {f->mul + 8, 0xBC1E83A0, 0x1E210800},
             {f->mul + 12, 0xBC5E83A0, 0xBC1E83A0}};
    }
    p.push_back({f->decay, f->decay_insn[0], rt.main.EncodeAdrp(f->decay, 9, page)});
    p.push_back({f->decay + 4, f->decay_insn[1], ldr_s(1, off + 4)});
    p.push_back({f->recover, f->recover_insn[0], rt.main.EncodeAdrp(f->recover, 9, page)});
    p.push_back({f->recover + 4, f->recover_insn[1], ldr_s(1, off + 8)});
    // All checked before anything changes.
    for (const P& x : p) {
        const uint32_t* at = reinterpret_cast<const uint32_t*>(rt.main.base() + x.at);
        if (*at != x.from) {
            LOGW("hooks", "sprint frame-rate fix not applied: main+0x%lx is %08x, expected %08x", (unsigned long)x.at,
                 *at, x.from);
            return;
        }
    }
    graze[0] = 30.0f;
    graze[1] = 0.8f;
    graze[2] = 1.2f;
    std::string err;
    size_t done = 0;
    for (const P& x : p) {
        if (!rt.main.PatchInstruction(x.at, x.from, x.to, &err)) break;
        ++done;
    }
    if (done != p.size()) {
        for (size_t i = done; i-- > 0;) rt.main.PatchInstruction(p[i].at, p[i].to, p[i].from, &err);
        LOGW("hooks", "sprint frame-rate fix not applied (%s)", err.c_str());
        return;
    }
    g_graze = graze;
    LOGI("hooks", "sprint frame-rate fix: the wall check and its slow-down follow the time step");
}

// Performance mode: low-quality effects, 1024x1024 shadow maps (see GameBuild); the menu also sets render scale 0.75.
// Each part is checked before it is changed and is skipped (logged) if the game's code or data is not as expected.
void ApplyPerformanceMode(Runtime& rt, const GameBuild& b) {
    constexpr uint32_t kLdrbLowVfx = 0x3956668A;    // ldrb w10, [x20, #0x599]
    constexpr uint32_t kMovLowVfxOn = 0x5280002A;   // mov w10, #1
    std::string err;
    std::string done;
    if (rt.main.PatchInstruction(b.lowvfx_read, kLdrbLowVfx, kMovLowVfxOn, &err))
        done += done.empty() ? "low-quality effects" : ", low-quality effects";
    else
        LOGW("hooks", "performance mode: effects quality not changed (%s)", err.c_str());
    if (rt.config.shadow_quality == 3) {  // shadow_quality 0-2 set their own size (ApplyShadowQuality)
        int32_t* shadow = reinterpret_cast<int32_t*>(rt.main.base() + b.shadow_size);
        if (rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) && *shadow == 2048) {
            *shadow = 1024;
            done += done.empty() ? "shadow maps 1024" : ", shadow maps 1024";
        } else {
            LOGW("hooks", "performance mode: shadow map size not changed (found %d)",
                 rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) ? *shadow : -1);
        }
    }
    LOGI("hooks", "performance mode: %s", done.empty() ? "nothing applied" : done.c_str());
}

// Diagnostics (swipe camera, direct camera control): which game code reads the right stick. The pad class's analog
// getter (1.0.0 main+0x81a5dc: index 3..8 -> float at this+0x468 + 4 * (index - 3); 3/4 the right stick X/Y, 5/6
// the left stick) is replaced by the same lookup that also logs each new caller (and its caller) of the stick
// channels, the first 48. The camera code that turns the stick into a turn is among them.
struct PadProbe {
    const char* build_id;
    uint64_t getter;
};
const PadProbe kPadProbes[] = {
    {"5a797a3a", 0x81a5dc},
    {"6569410c", 0x8130cc},
    {"df3766a2", 0x815aa0},
};
uint32_t g_pad_analog_base = 0x468;
uintptr_t g_main_base = 0, g_main_end = 0;
std::mutex g_probe_mutex;
std::unordered_set<uint64_t> g_probe_seen;

void CamNote(int i, float v);
// The swipe camera's diagnostics (pad probe, action probe and read trap, action / turn getter probes, per-second
// camera line): off now that the camera's caps are found; they cost a fault or a lock per read.
constexpr bool kCameraDiagnostics = false;

float PadAnalogProbe(const uint8_t* self, int index) {
    const unsigned i = unsigned(index - 3);
    const float v = i <= 5 ? *reinterpret_cast<const float*>(self + g_pad_analog_base + 4 * i) : 0.0f;
    if ((index == 3 || index == 4) && v != 0.0f) CamNote(5, v);
    if (index == 3 || index == 4) {
        // Every reader goes through the device set's "first device with a non-zero value" (main+0x816968 in df37),
        // so the reader is its caller: followed through the frame records (x29), only while each one lies close
        // above the previous on this stack (x29 may hold anything otherwise).
        uintptr_t ret[4] = {reinterpret_cast<uintptr_t>(__builtin_return_address(0)), 0, 0, 0};
        uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        for (int k = 1; k < 4 && fp; k++) {
            const uintptr_t next = *reinterpret_cast<const uintptr_t*>(fp);
            if (!(next > fp && next - fp < (1u << 20) && next % 16 == 0)) break;
            fp = next;
            ret[k] = *reinterpret_cast<const uintptr_t*>(fp + 8);
        }
        const uint64_t key = (uint64_t(ret[0]) * 31 + ret[1]) * 31 + ret[2] + uint64_t(index);
        std::lock_guard<std::mutex> lk(g_probe_mutex);
        if (g_probe_seen.size() < 64 && g_probe_seen.insert(key).second) {
            auto off = [](uintptr_t a) -> long {
                return a >= g_main_base && a < g_main_end ? long(a - g_main_base) : -1;
            };
            LOGI("hooks", "pad probe: right stick %s read (now %.3f) from main+0x%lx <- 0x%lx <- 0x%lx <- 0x%lx",
                 index == 3 ? "X" : "Y", v, off(ret[0]), off(ret[1]), off(ret[2]), off(ret[3]));
        }
    }
    return v;
}

// The game's input mapping (df37 main+0x830400): every frame each pad channel is read, passed through a dead zone,
// scaled and clamped to +-max, and written back; the parameters of entry i come from this getter (28-byte entries:
// +0 channel, +4 paired channel or -1, +8 dead zone, +12 threshold, +16 scale, +20 max, +24 flag). The right stick
// (channels 3 and 4) is clamped to max (1), which is why touch values past full tilt changed nothing. The swipe camera
// test raises max to 4 for those two channels: only touch input goes past 1, so controllers are not affected. The
// entries are logged once.
struct MapEntryHook {
    const char* build_id;
    uint64_t getter;
};
const MapEntryHook kMapEntryHooks[] = {
    {"5a797a3a", 0x821690},
    {"6569410c", 0x819d28},
    {"df3766a2", 0x81ccc0},
};
void InputMapEntry(const uint8_t* self, int i, int32_t* channel, float* dead_zone, int32_t* pair, float* threshold,
                   float* scale, float* max, uint8_t* flag) {
    const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(self + 8) + int64_t(i) * 0x1c;
    memcpy(channel, e + 0, 4);
    memcpy(pair, e + 4, 4);
    memcpy(dead_zone, e + 8, 4);
    memcpy(threshold, e + 12, 4);
    memcpy(scale, e + 16, 4);
    memcpy(max, e + 20, 4);
    *flag = e[24];
    static std::atomic<uint64_t> logged{0};
    if (i >= 0 && i < 64 && !(logged.fetch_or(1ull << i) & (1ull << i)))
        LOGI("hooks", "input mapping %d: channel %d (pair %d), dead zone %.3f, threshold %.3f, scale %.3f, max %.3f, "
             "flag %u", i, *channel, *pair, *dead_zone, *threshold, *scale, *max, *flag);
    // Every channel capped at full tilt is allowed to 4: only the touch swipe camera sends more than 1 (controllers
    // and the touch sticks stop at 1). The camera's actions (18 / 19) come from entries without a dead zone, clamped
    // per axis, so raising only the right stick's dead-zone entries (channels 3 / 4) left a cap of 1 per axis.
    if (*max >= 0.99f && *max <= 1.01f) *max = 8.0f;
}

// The action mapping (df37 main+0x81c848, used by main+0x82ffa8): after the channels, each action's frame value gets
// a dead zone and a scale and is clamped to +-max per action (28-byte entries: +0 action, +4 paired action, +8, +12,
// +16 scale, +20 max, +24 dead zone; max goes out through the stack argument). That per-axis clamp to 1 is what held
// the camera's actions (both axes at 1, length 1.41, with the touch at 2.69). Replaced by the same code that lets a max
// of 1 go to 4 while the touch swipe camera is past full tilt, so nothing else changes.
struct ActionMapHook {
    const char* build_id;
    uint64_t fn;
};
const ActionMapHook kActionMapHooks[] = {
    {"5a797a3a", 0x821218},
    {"6569410c", 0x8199d0},
    {"df3766a2", 0x81c848},
};
void ActionMapEntry(const uint8_t* self, int i, uint32_t* action, uint32_t* pair, uint32_t* f8, uint32_t* f12,
                    uint32_t* f24, float* scale, float* max) {
    const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(self + 8) + int64_t(i) * 0x1c;
    memcpy(action, e + 0, 4);
    memcpy(pair, e + 4, 4);
    memcpy(f8, e + 8, 4);
    memcpy(f12, e + 12, 4);
    memcpy(scale, e + 16, 4);
    memcpy(max, e + 20, 4);
    memcpy(f24, e + 24, 4);
    if (*max >= 0.99f && *max <= 1.01f && dsr::input::TouchSwipeActive()) *max = 8.0f;
}

// The input layer's action setter (df37 main+0x82c900): each device channel that drives a user action passes its value
// here; the action keeps its largest positive and its most negative value of the frame (+208, +248) and their sum
// (+328), which is later copied to the array the game reads (+32). Replaced by the same code that logs, once per
// action, its id and the first non-zero value: the right stick's action ids lead to the camera code that reads them.
struct ActionSetHook {
    const char* build_id;
    uint64_t setter;
};
const ActionSetHook kActionSetHooks[] = {
    {"5a797a3a", 0x8312d0},
    {"df3766a2", 0x82c900},
};
// Read trap (diagnostics): the camera reads the right stick's actions (6 / 7 while walking) through code no hook sees.
// A trap on the arrays' own pages caught nothing (any access to the page opened it, and those pages are busy), so the
// input object's two action arrays (the frame sums, std::vector at +328, and the published copy at +32) are moved to
// pages of their own: copied, and the vectors pointed at the copies (their sizes do not change after start-up). For
// 10 s after the camera first turns, those pages are made inaccessible every 2 ms; an access faults, the handler notes
// the instruction, its return address and the action index, opens the page and lets the instruction run. Each new
// reader is logged (48 at most), then the counts.
#if defined(__aarch64__)
struct TrapRange {
    std::atomic<uintptr_t> lo{0}, hi{0};
};
TrapRange g_trap[2];
std::atomic<bool> g_trap_on{false}, g_trap_armed{false};
struct sigaction g_trap_old;
size_t g_trap_page_size = 4096;
std::atomic<int> g_trap_count{0};
std::atomic<uint32_t> g_trap_faults{0}, g_trap_camera{0};
uint64_t g_trap_seen[48];
std::atomic<uint64_t> g_trap_pending[48];  // array << 63 | index << 56 | pc offset << 28 | lr offset

// Camera trap: the page holding the camera input the player's pad manipulator stores (its object +64: turn x, y;
// stored at df37 main+0x2b223c from the camera getters main+0x68b704 / 0x68b764), to find the code that turns the
// camera by it. Same scheme; only accesses to those 8 bytes are recorded.
std::atomic<uintptr_t> g_cam_trap_lo{0}, g_cam_trap_at{0};
std::atomic<bool> g_cam_trap_on{false}, g_trap_handler{false};
std::atomic<int> g_cam_trap_count{0};
std::atomic<uint32_t> g_cam_trap_faults{0}, g_cam_trap_hits{0};
uint64_t g_cam_trap_seen[32];
std::atomic<uint64_t> g_cam_trap_pending[32];  // offset << 56 | pc offset << 28 | lr offset

// The camera input's page is busy (the rest of the object), so waiting for the first access after re-arming rarely
// caught the reader. Instead the faulting load or store is carried out here (the common addressing forms) and the
// page stays closed: every access is seen. Other forms open the page until the next re-arm.
__uint128_t* TrapFpRegs(ucontext_t* uc) {
    uint8_t* p = reinterpret_cast<uint8_t*>(uc->uc_mcontext.__reserved);
    for (size_t off = 0; off + sizeof(_aarch64_ctx) <= sizeof(uc->uc_mcontext.__reserved);) {
        auto* h = reinterpret_cast<_aarch64_ctx*>(p + off);
        if (h->magic == 0 || h->size == 0) break;
        if (h->magic == FPSIMD_MAGIC) return reinterpret_cast<fpsimd_context*>(h)->vregs;
        off += h->size;
    }
    return nullptr;
}
struct TrapAccess {
    uintptr_t addr = 0;
    unsigned bytes = 0, count = 1;  // count 2: a pair
    bool load = false, fp = false;
    int sext = 0;  // 32 / 64: sign-extending load
    int rt = 0, rt2 = 0;
    int wb_reg = -1;  // writeback base
    uintptr_t wb_value = 0;
};
bool TrapDecode(const ucontext_t* uc, uint32_t w, TrapAccess* t) {
    const auto& mc = uc->uc_mcontext;
    auto base = [&](int r) -> uint64_t { return r == 31 ? mc.sp : mc.regs[r]; };
    auto reg = [&](int r) -> uint64_t { return r == 31 ? 0 : mc.regs[r]; };
    const int rn = (w >> 5) & 31;
    t->rt = w & 31;
    const unsigned size = w >> 30, v = (w >> 26) & 1;
    auto single = [&](unsigned opc) -> bool {
        if (v) {
            if (opc >= 2) {
                if (size) return false;
                t->bytes = 16;
                t->load = opc == 3;
            } else {
                t->bytes = 1u << size;
                t->load = opc == 1;
            }
            t->fp = true;
        } else {
            t->bytes = 1u << size;
            t->load = opc != 0;
            if (opc == 2) {
                if (size == 3) return false;  // prefetch
                t->sext = 64;
            } else if (opc == 3) {
                if (size >= 2) return false;
                t->sext = 32;
            }
        }
        return true;
    };
    if ((w & 0x3B000000) == 0x39000000) {  // unsigned offset
        if (!single((w >> 22) & 3)) return false;
        t->addr = base(rn) + uint64_t((w >> 10) & 0xFFF) * t->bytes;
        return true;
    }
    if ((w & 0x3B200000) == 0x38000000) {  // unscaled / post / pre index
        if (!single((w >> 22) & 3)) return false;
        const int64_t imm = int64_t(int32_t(w << 11) >> 23);
        const unsigned mode = (w >> 10) & 3;
        t->addr = base(rn) + (mode == 1 ? 0 : imm);
        if (mode == 1 || mode == 3) {
            t->wb_reg = rn;
            t->wb_value = base(rn) + imm;
        }
        return true;
    }
    if ((w & 0x3B200C00) == 0x38200800) {  // register offset
        if (!single((w >> 22) & 3)) return false;
        const uint64_t m = reg((w >> 16) & 31);
        const unsigned option = (w >> 13) & 7;
        int64_t off;
        if (option == 2) off = int64_t(uint32_t(m));
        else if (option == 6) off = int64_t(int32_t(uint32_t(m)));
        else if (option == 3 || option == 7) off = int64_t(m);
        else return false;
        if ((w >> 12) & 1) off <<= __builtin_ctz(t->bytes);
        t->addr = base(rn) + uint64_t(off);
        return true;
    }
    if ((w & 0x3A000000) == 0x28000000) {  // pair
        const unsigned opc = w >> 30, idx = (w >> 23) & 3;
        t->load = (w >> 22) & 1;
        t->fp = v;
        t->count = 2;
        t->rt2 = (w >> 10) & 31;
        if (v) {
            if (opc == 3) return false;
            t->bytes = 4u << opc;
        } else if (opc == 0) {
            t->bytes = 4;
        } else if (opc == 2) {
            t->bytes = 8;
        } else if (opc == 1 && t->load) {
            t->bytes = 4;
            t->sext = 64;
        } else {
            return false;
        }
        const int64_t imm = int64_t(int32_t(w << 10) >> 25) * int64_t(t->bytes);
        t->addr = base(rn) + (idx == 1 ? 0 : imm);
        if (idx == 1 || idx == 3) {
            t->wb_reg = rn;
            t->wb_value = base(rn) + imm;
        }
        return true;
    }
    return false;
}
bool TrapExecute(ucontext_t* uc, const TrapAccess& t) {
    __uint128_t* vr = t.fp ? TrapFpRegs(uc) : nullptr;
    if (t.fp && !vr) return false;
    auto& mc = uc->uc_mcontext;
    for (unsigned k = 0; k < t.count; k++) {
        const int r = k ? t.rt2 : t.rt;
        uint8_t* p = reinterpret_cast<uint8_t*>(t.addr + k * t.bytes);
        if (t.load) {
            if (t.fp) {
                __uint128_t v = 0;
                memcpy(&v, p, t.bytes);
                vr[r] = v;
            } else {
                uint64_t v = 0;
                memcpy(&v, p, t.bytes);
                if (t.sext && t.bytes < 8) {
                    const int sh = 64 - 8 * int(t.bytes);
                    v = uint64_t(int64_t(v << sh) >> sh);
                    if (t.sext == 32) v &= 0xFFFFFFFFull;
                }
                if (r != 31) mc.regs[r] = v;
            }
        } else {
            if (t.fp) {
                memcpy(p, &vr[r], t.bytes);
            } else {
                const uint64_t v = r == 31 ? 0 : mc.regs[r];
                memcpy(p, &v, t.bytes);
            }
        }
    }
    if (t.wb_reg >= 0) {
        if (t.wb_reg == 31) mc.sp = t.wb_value;
        else mc.regs[t.wb_reg] = t.wb_value;
    }
    mc.pc += 4;
    return true;
}

void TrapHandler(int sig, siginfo_t* si, void* ctx) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
    if (const uintptr_t lo = g_cam_trap_lo.load(std::memory_order_relaxed);
        lo && a >= lo && a < lo + g_trap_page_size) {
        mprotect(reinterpret_cast<void*>(lo), g_trap_page_size, PROT_READ | PROT_WRITE);
        g_cam_trap_faults.fetch_add(1, std::memory_order_relaxed);
        const uintptr_t at = g_cam_trap_at.load(std::memory_order_relaxed);
        auto* uc = static_cast<ucontext_t*>(ctx);
        const bool guest = uc->uc_mcontext.pc >= g_main_base && uc->uc_mcontext.pc < g_main_end;
        TrapAccess t;
        bool done = false;
        if (guest && TrapDecode(uc, *reinterpret_cast<const uint32_t*>(uc->uc_mcontext.pc), &t)) {
            const uint64_t pc0 = uc->uc_mcontext.pc;
            const uintptr_t end = t.addr + uintptr_t(t.bytes) * t.count;
            if (t.addr < at + 8 && end > at) {
                g_cam_trap_hits.fetch_add(1, std::memory_order_relaxed);
                const uint64_t pc = (pc0 - g_main_base) & 0x07FFFFFF;
                const uint64_t lr = (uc->uc_mcontext.regs[30] - g_main_base) & 0x0FFFFFFF;
                const uint64_t off = t.addr >= at ? t.addr - at : 0;
                const uint64_t key = ((off & 0x7F) << 56) | (uint64_t(t.load) << 55) | (pc << 28) | lr;
                const int n = g_cam_trap_count.load(std::memory_order_relaxed);
                bool seen = false;
                for (int i = 0; i < n && i < 32; i++) seen |= g_cam_trap_seen[i] == key;
                if (!seen && n < 32) {
                    g_cam_trap_seen[n] = key;
                    g_cam_trap_pending[n].store(key, std::memory_order_relaxed);
                    g_cam_trap_count.store(n + 1, std::memory_order_release);
                }
            }
            (void)TrapExecute;  // carrying the access out froze the game: the page opens instead (re-armed often)
        }
        if (done && g_cam_trap_lo.load(std::memory_order_relaxed))
            mprotect(reinterpret_cast<void*>(lo), g_trap_page_size, PROT_NONE);
        return;
    }
    for (int w = 0; w < 2; w++) {
        const uintptr_t lo = g_trap[w].lo.load(std::memory_order_relaxed);
        if (!lo || a < lo || a >= lo + g_trap_page_size) continue;
        mprotect(reinterpret_cast<void*>(lo), g_trap_page_size, PROT_READ | PROT_WRITE);
        g_trap_faults.fetch_add(1, std::memory_order_relaxed);
        const uint64_t index = (a - lo) / 4;
        if (index == 6 || index == 7) g_trap_camera.fetch_add(1, std::memory_order_relaxed);
        auto* uc = static_cast<ucontext_t*>(ctx);
        if (uc->uc_mcontext.pc < g_main_base || uc->uc_mcontext.pc >= g_main_end) return;  // our own code
        const uint64_t pc = (uc->uc_mcontext.pc - g_main_base) & 0x0FFFFFFF;
        const uint64_t lr = (uc->uc_mcontext.regs[30] - g_main_base) & 0x0FFFFFFF;
        const uint64_t key = (uint64_t(w) << 63) | ((index & 0x7F) << 56) | (pc << 28) | lr;
        const int n = g_trap_count.load(std::memory_order_relaxed);
        bool seen = false;
        for (int i = 0; i < n && i < 48; i++) seen |= g_trap_seen[i] == key;
        if (!seen && n < 48) {
            g_trap_seen[n] = key;
            g_trap_pending[n].store(key, std::memory_order_relaxed);
            g_trap_count.store(n + 1, std::memory_order_release);
        }
        return;
    }
    // Not ours: the previous handler (the crash handler).
    if (g_trap_old.sa_flags & SA_SIGINFO) {
        if (g_trap_old.sa_sigaction) g_trap_old.sa_sigaction(sig, si, ctx);
    } else if (g_trap_old.sa_handler != SIG_DFL && g_trap_old.sa_handler != SIG_IGN) {
        g_trap_old.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

void StartCamTrap(uintptr_t at) {
    bool expected = false;
    if (!g_cam_trap_on.compare_exchange_strong(expected, true)) return;
    g_trap_page_size = size_t(sysconf(_SC_PAGESIZE));
    const uintptr_t lo = at & ~uintptr_t(g_trap_page_size - 1);
    g_cam_trap_at = at;
    LOGI("hooks", "camera trap: watching the camera's angles at %p (page %p)", reinterpret_cast<void*>(at),
         reinterpret_cast<void*>(lo));
    struct sigaction sa {};
    sa.sa_sigaction = TrapHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (!g_trap_handler.exchange(true)) sigaction(SIGSEGV, &sa, &g_trap_old);
    std::thread([lo] {
        int logged = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        g_cam_trap_lo = lo;
        while (std::chrono::steady_clock::now() < end) {
            mprotect(reinterpret_cast<void*>(lo), g_trap_page_size, PROT_NONE);  // after an access not carried out
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            const int n = g_cam_trap_count.load(std::memory_order_acquire);
            for (; logged < n; logged++) {
                const uint64_t v = g_cam_trap_pending[logged].load(std::memory_order_relaxed);
                LOGI("hooks", "camera trap: camera angle +%d %s at main+0x%llx (return main+0x%llx)",
                     320 + int((v >> 56) & 0x7F), (v >> 55) & 1 ? "read" : "written",
                     (unsigned long long)((v >> 28) & 0x07FFFFFF),
                     (unsigned long long)(v & 0x0FFFFFFF));
            }
        }
        g_cam_trap_lo = 0;
        mprotect(reinterpret_cast<void*>(lo), g_trap_page_size, PROT_READ | PROT_WRITE);
        LOGI("hooks", "camera trap: done, %d accessors (%u faults, %u on the camera input)", g_cam_trap_count.load(),
             g_cam_trap_faults.load(), g_cam_trap_hits.load());
    }).detach();
}

// Our own hooks touch the trapped pages first every frame (the action setter, the stick vector), which opened them
// before any game reader got there: they close them again right after.
void TrapRearm() {
    if (!g_trap_on.load(std::memory_order_relaxed)) return;
    for (auto& r : g_trap) {
        const uintptr_t lo = r.lo.load(std::memory_order_relaxed);
        if (lo) mprotect(reinterpret_cast<void*>(lo), g_trap_page_size, PROT_NONE);
    }
}

// Moves a std::vector<float> (begin, end, capacity at `vec`) to a page of its own; its address, or 0.
uintptr_t MoveToOwnPage(uint8_t* vec) {
    auto* p = reinterpret_cast<uintptr_t*>(vec);
    const uintptr_t begin = p[0], endp = p[1];
    const size_t bytes = endp - begin;
    if (!begin || endp < begin || bytes == 0 || bytes > g_trap_page_size) return 0;
    void* page = mmap(nullptr, g_trap_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 0;
    memcpy(page, reinterpret_cast<const void*>(begin), bytes);
    const uintptr_t np = reinterpret_cast<uintptr_t>(page);
    p[0] = np;
    p[1] = np + bytes;
    p[2] = np + bytes;
    return np;
}

void StartReadTrap(uint8_t* input) {
    bool expected = false;
    if (!g_trap_on.compare_exchange_strong(expected, true)) return;
    g_trap_page_size = size_t(sysconf(_SC_PAGESIZE));
    const size_t n_sums = (*reinterpret_cast<uintptr_t*>(input + 336) - *reinterpret_cast<uintptr_t*>(input + 328)) / 4;
    const size_t n_copy = (*reinterpret_cast<uintptr_t*>(input + 40) - *reinterpret_cast<uintptr_t*>(input + 32)) / 4;
    const uintptr_t sums = MoveToOwnPage(input + 328), copy = MoveToOwnPage(input + 32);
    LOGI("hooks", "read trap: action arrays moved to their own pages: sums (%zu) %s, copy (%zu) %s", n_sums,
         sums ? "ok" : "not moved", n_copy, copy ? "ok" : "not moved");
    g_trap[0].lo = sums;
    g_trap[1].lo = copy;
    struct sigaction sa {};
    sa.sa_sigaction = TrapHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (!g_trap_handler.exchange(true)) sigaction(SIGSEGV, &sa, &g_trap_old);
    std::thread([] {
        int logged = 0, fails = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end) {
            for (auto& r : g_trap)
                if (r.lo && mprotect(reinterpret_cast<void*>(r.lo.load()), g_trap_page_size, PROT_NONE) != 0 &&
                    fails++ == 0)
                    LOGW("hooks", "read trap: mprotect failed (%s)", strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            for (auto& r : g_trap)
                if (r.lo) mprotect(reinterpret_cast<void*>(r.lo.load()), g_trap_page_size, PROT_READ | PROT_WRITE);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            const int n = g_trap_count.load(std::memory_order_acquire);
            for (; logged < n; logged++) {
                const uint64_t v = g_trap_pending[logged].load(std::memory_order_relaxed);
                LOGI("hooks", "read trap: action %d of the %s read at main+0x%llx (return main+0x%llx)",
                     int((v >> 56) & 0x7F), (v >> 63) ? "published copy" : "frame sums",
                     (unsigned long long)((v >> 28) & 0x0FFFFFFF), (unsigned long long)(v & 0x0FFFFFFF));
            }
        }
        for (auto& r : g_trap) {  // the moved arrays stay where they are
            const uintptr_t lo = r.lo.exchange(0);
            if (lo) mprotect(reinterpret_cast<void*>(lo), g_trap_page_size, PROT_READ | PROT_WRITE);
        }
        g_trap_on = false;
        LOGI("hooks", "read trap: done, %d readers (%u faults, %u on actions 6 / 7)", g_trap_count.load(),
             g_trap_faults.load(), g_trap_camera.load());
    }).detach();
}
#endif

void ActionSet(uint8_t* self, int id, float v) {
    float* pos = *reinterpret_cast<float**>(self + 208);
    float* neg = *reinterpret_cast<float**>(self + 248);
    float* sum = *reinterpret_cast<float**>(self + 328);
    float p = pos[id], n = neg[id];
    if (v >= 0.0f) {
        if (p < v) pos[id] = p = v;
    } else if (n > v) {
        neg[id] = n = v;
    }
    sum[id] = p + n;
    static std::atomic<uint64_t> logged[2];
    if (v != 0.0f && id >= 0 && id < 128 && !(logged[id >> 6].fetch_or(1ull << (id & 63)) & (1ull << (id & 63))))
        LOGI("hooks", "input action %d: first value %.3f (input object @%p, read array @%p)", id, v,
             static_cast<void*>(self), *reinterpret_cast<void**>(self + 32));
#if defined(__aarch64__)
    if (id == 6 && v != 0.0f) StartReadTrap(self);
    TrapRearm();
#endif
}

// The game's stick vector (df37 main+0x694d20, found with the read trap): actions (x, y) from the published array,
// a second radial dead zone of 0.37, then length = clamp((len - 0.37) * 1.724, 0, 1) along the stick's direction,
// returned as two floats in x0. That clamp to 1 is the camera's speed cap. Replaced by the same code without the
// clamp while the touch swipe camera sends more than full tilt (its length past 1; a stick never gets there, the
// touch stick is clamped to 1), so controllers, the left stick and the touch stick behave as before.
struct StickVecHook {
    const char* build_id;
    uint64_t fn;
};
const StickVecHook kStickVecHooks[] = {
    {"5a797a3a", 0x69a058},
    {"6569410c", 0x694d50},
    {"df3766a2", 0x694d20},
};
// Camera diagnostics: once a second while the touch camera is past full tilt, the most it sent, the most the camera's
// stick vector (actions 18 / 19) saw and gave, and the most the turn getters (below) read and returned.
std::mutex g_cam_mutex;
float g_cam_max[10];  // touch, vector in, vector out, action 26 in, turn out, pad raw, 18 x / 19 y, getter 6/7, 26/27
std::chrono::steady_clock::time_point g_cam_next;
void CamNote(int i, float v) {
    if (!kCameraDiagnostics) return;
    std::lock_guard<std::mutex> lk(g_cam_mutex);
    g_cam_max[i] = std::max(g_cam_max[i], std::fabs(v));
    g_cam_max[0] = std::max(g_cam_max[0], dsr::input::TouchCameraTilt());
    const auto now = std::chrono::steady_clock::now();
    if (now < g_cam_next) return;
    g_cam_next = now + std::chrono::seconds(1);
    if (g_cam_max[0] > 1.0f)
        LOGI("hooks", "camera: last second most: touch %.2f, pad %.2f, stick vector (18, 19) x %.2f y %.2f len %.2f -> "
             "%.2f, action getter 6/7 %.2f 26/27 %.2f, turn getter %.2f -> %.4f", g_cam_max[0], g_cam_max[5],
             g_cam_max[6], g_cam_max[7], g_cam_max[1], g_cam_max[2], g_cam_max[8], g_cam_max[9], g_cam_max[3],
             g_cam_max[4]);
    for (float& m : g_cam_max) m = 0.0f;
}

// The camera's turn getters (df37 main+0x6887ac / 0x6887d4): action 26 (27) times 1.5 degrees (minus), read straight
// from the published array. Same code that logs each caller once and feeds the camera diagnostics.
struct TurnHook {
    const char* build_id;
    uint64_t fn[2];
};
const TurnHook kTurnHooks[] = {{"df3766a2", {0x6887ac, 0x6887d4}}};
float g_turn_scale[2] = {0.02617994f, -0.02617994f};
template <int K>
float TurnGetter(const uint8_t* self) {
    const uint8_t* input = *reinterpret_cast<const uint8_t* const*>(self + 8);
    const float* a = *reinterpret_cast<const float* const*>(*reinterpret_cast<const uint8_t* const*>(input + 8) + 32);
    const float v = a[26 + K];
    const float r = v * g_turn_scale[K];
#if defined(__aarch64__)
    TrapRearm();
#endif
    const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    {
        std::lock_guard<std::mutex> lk(g_probe_mutex);
        if (v != 0.0f && g_probe_seen.size() < 128 && g_probe_seen.insert((uint64_t(ret) << 1) | 1ull << 62 | K).second)
            LOGI("hooks", "camera turn getter %d: called from main+0x%lx (action %.3f -> %.4f)", K,
                 ret >= g_main_base && ret < g_main_end ? long(ret - g_main_base) : -1L, v, r);
    }
    CamNote(3, v);
    CamNote(4, r);
    return r;
}

// The single action getter (df37 main+0x694d10, 4 instructions: the published array's value): same code that logs
// each caller of the right stick's actions once and feeds the camera diagnostics.
const StickVecHook kActionGetHooks[] = {{"df3766a2", 0x694d10}};
float ActionGet(const uint8_t* self, int id) {
    const float v = (*reinterpret_cast<const float* const*>(*reinterpret_cast<const uint8_t* const*>(self + 8) + 32))[id];
#if defined(__aarch64__)
    TrapRearm();
#endif
    if (id == 6 || id == 7 || id == 18 || id == 19 || (id >= 26 && id <= 31)) {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        {
            std::lock_guard<std::mutex> lk(g_probe_mutex);
            if (v != 0.0f && g_probe_seen.size() < 160 &&
                g_probe_seen.insert((uint64_t(ret) << 8) | 1ull << 61 | uint64_t(id)).second)
                LOGI("hooks", "action getter: action %d read by main+0x%lx (%.3f)", id,
                     ret >= g_main_base && ret < g_main_end ? long(ret - g_main_base) : -1L, v);
        }
        if (id == 6 || id == 7) CamNote(8, v);
        if (id == 26 || id == 27) CamNote(9, v);
    }
    return v;
}

// The camera's yaw (df37 main+0xab8c4, in the camera update): while the horizontal input is past a small threshold,
// the camera turns at a speed that ramps from its slowest to its fastest over a set time (+312 counts it up, +480 the
// time), in the input's direction only - how far the stick is pushed does not count. Below the threshold it turns by
// input times the slowest speed. That ramp is the swipe camera's cap and its lag (a flick ends before the ramp is up).
// While the touch swipe camera is used, the branch into the ramp is replaced by a jump to the proportional turn, so
// the camera turns by exactly what the swipe sends; restored when the thumb lifts (sticks keep the game's feel).
// The camera's follow (df37 main+0xab780): when the character has moved this frame (w19), the camera's angles are
// worked out again from where the camera now is to the character - so the camera turns after a walking character,
// against the swipe (and with a stick too: that is the game's own behaviour). While swiping that block is jumped
// over (cbz w19 -> b), so the camera keeps the angle the thumb gave it and only moves along with the character.
struct RampPatch {
    const char* build_id;
    uint64_t at;
    uint32_t orig, repl;  // b.le <proportional> / b <proportional>
    uint64_t follow_at;
    uint32_t follow_orig, follow_repl;  // cbz w19, <past> / b <past>
    uint64_t follow2_at;  // ldr q1, [x20, #208] -> b <past the pitch and yaw stores> (+44)
};
// The follow that the trap caught (df37 main+0xab3f8, pitch written at main+0xab410): in the normal camera the
// angles are worked out from the camera's position to its target every frame, so the camera turns after the
// character. Jumped over while swiping, like the first one.
constexpr uint32_t kFollow2Orig = 0x3DC03681, kFollow2Repl = 0x1400000B;
const RampPatch kRampPatches[] = {
    {"5a797a3a", 0xae554, 0x5400020D, 0x14000010, 0xae41c, 0x340001B3, 0x1400000D, 0xae094},
    {"6569410c", 0xab8c8, 0x5400022D, 0x14000011, 0xab780, 0x340001D3, 0x1400000E, 0xab3f8},
    {"df3766a2", 0xab8c8, 0x5400022D, 0x14000011, 0xab780, 0x340001D3, 0x1400000E, 0xab3f8},
};
bool g_follow_ok = false, g_follow2_ok = false;
loader::Module* g_ramp_module = nullptr;
const RampPatch* g_ramp = nullptr;
std::atomic<bool> g_ramp_off{false};
void SetCameraProportional(bool on) {
    if (!g_ramp || g_ramp_off.load(std::memory_order_relaxed) == on) return;
    g_ramp_off = on;
    const uint32_t w = on ? g_ramp->repl : g_ramp->orig;
    if (!g_ramp_module->WriteReadOnly(g_ramp->at, &w, 4)) LOGW("hooks", "swipe camera: camera ramp patch failed");
    const uint32_t f = on ? g_ramp->follow_repl : g_ramp->follow_orig;
    if (g_follow_ok && !g_ramp_module->WriteReadOnly(g_ramp->follow_at, &f, 4))
        LOGW("hooks", "swipe camera: camera follow patch failed");
    const uint32_t f2 = on ? kFollow2Repl : kFollow2Orig;
    if (g_follow2_ok && !g_ramp_module->WriteReadOnly(g_ramp->follow2_at, &f2, 4))
        LOGW("hooks", "swipe camera: camera follow patch 2 failed");
}

// Diagnostics (df37): the camera update gets the pad manipulator through main+0x2c5900 (called at main+0xaab50, the
// camera object in x20). Replaced by the same code through a stub that also passes x20 and the return address; the
// first time it is called from the camera while swiping, the camera trap watches the camera's angles (+320 pitch,
// +324 yaw) for 10 s to find the code that turns the camera after the character.
extern "C" uint64_t DsrManipGetterImpl(const uint8_t* self, uintptr_t cam, uintptr_t lr) {
    const uint8_t* o = *reinterpret_cast<const uint8_t* const*>(self + 72);
    uint64_t r = *reinterpret_cast<const uint64_t*>(o + 648);
    if (!r) r = *reinterpret_cast<const uint64_t*>(o + 120);
#if defined(__aarch64__)
    if (lr == g_main_base + 0xaab54 && cam && !g_cam_trap_on.load(std::memory_order_relaxed) &&
        dsr::input::TouchSwipeActive())
        StartCamTrap(cam + 320);
#else
    (void)cam;
    (void)lr;
#endif
    return r;
}
extern "C" void DsrManipGetterStub();
#if defined(__aarch64__)
asm(".text\n.p2align 2\n.global DsrManipGetterStub\n.type DsrManipGetterStub, %function\nDsrManipGetterStub:\n"
    "    mov x1, x20\n    mov x2, x30\n    b DsrManipGetterImpl\n");
#else
extern "C" void DsrManipGetterStub() {}
#endif

uint64_t StickVector(const uint8_t* self, int ix, int iy) {
    const float* a = *reinterpret_cast<const float* const*>(*reinterpret_cast<const uint8_t* const*>(self + 8) + 32);
    const float x = a[ix], y = a[iy];
    const float len = std::sqrt(x * x + y * y);
    float rx = 0.0f, ry = 0.0f;
    if (len > 0.37f && std::isfinite(len)) {
        float m = std::max(0.0f, (len - 0.37f) * 1.7241380214691162f);
        if (m > 1.0f && !dsr::input::TouchSwipeActive()) m = 1.0f;
        rx = x / len * m;
        ry = y / len * m;
    }
#if defined(__aarch64__)
    TrapRearm();
#endif
    if (kCameraDiagnostics) {  // each caller (with its two actions) once, then the first time it gets past full tilt
        const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        const uint64_t key = (uint64_t(ret) << 16) | (uint64_t(ix & 0xFF) << 8) | uint64_t(iy & 0xFF);
        const bool past = len > 0.37f + 1.0f / 1.7241380214691162f + 0.05f;
        std::lock_guard<std::mutex> lk(g_probe_mutex);
        const bool first = g_probe_seen.size() < 128 && g_probe_seen.insert(key).second;
        const bool first_past = past && g_probe_seen.size() < 128 && g_probe_seen.insert(key ^ 1ull << 63).second;
        if ((first && len > 0.0f) || first_past)
            LOGI("hooks", "stick vector: actions (%d, %d) for main+0x%lx: length %.3f -> %.3f (touch camera %.2f)",
                 ix, iy, ret >= g_main_base && ret < g_main_end ? long(ret - g_main_base) : -1L, len,
                 std::sqrt(rx * rx + ry * ry), dsr::input::TouchCameraTilt());
        if (first && len == 0.0f) g_probe_seen.erase(key);  // log it once it has a value
    }
    if (ix == 18) SetCameraProportional(dsr::input::TouchSwipeActive());
    if (ix == 18) {
        CamNote(6, x);
        CamNote(7, y);
        CamNote(1, len);
        CamNote(2, std::sqrt(rx * rx + ry * ry));
    }
    uint32_t bx, by;
    memcpy(&bx, &rx, 4);
    memcpy(&by, &ry, 4);
    return uint64_t(bx) | (uint64_t(by) << 32);
}

static void InstallCameraInputProbe(Runtime& rt, const GameBuild& b) {
    if (kCameraDiagnostics && !strncmp(b.build_id, "df3766a2", 8)) {  // camera angle trap (diagnostics)
        // ldr x8,[x0,#72] / ldr x0,[x8,#648] / cbnz x0 / ldr x0,[x8,#120] / ret
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + 0x2c5900);
        std::string err;
        g_main_base = rt.main.base();
        g_main_end = g_main_base + 0x8000000;
        if (code[0] == 0xF9402408 && code[4] == 0xD65F03C0 &&
            rt.main.HookFunction(0x2c5900, reinterpret_cast<void*>(&DsrManipGetterStub), &err))
            LOGI("hooks", "camera trap: armed (starts on the first swipe)");
        else
            LOGW("hooks", "camera trap: manipulator getter not hooked %s", err.c_str());
    }
    for (const RampPatch& r : kRampPatches) {
        if (strncmp(r.build_id, b.build_id, strlen(r.build_id))) continue;
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + r.at);
        // fcmp s11, s0 / b.le / ldr s0, [x20, #312]
        if (code[-1] != 0x1E202160 || code[0] != r.orig || code[1] != 0xBD413A80) {
            LOGW("hooks", "swipe camera: camera ramp not found at main+0x%lx", (unsigned long)r.at);
            continue;
        }
        g_ramp_module = &rt.main;
        g_ramp = &r;
        // cbz w19 / ldr q0, [x20, #208] / ldr q1, [x20, #256]
        const uint32_t* fc = reinterpret_cast<const uint32_t*>(rt.main.base() + r.follow_at);
        g_follow_ok = fc[0] == r.follow_orig && fc[1] == 0x3DC03680 && fc[2] == 0x3DC04281;
        if (!g_follow_ok) LOGW("hooks", "swipe camera: camera follow not found at main+0x%lx", (unsigned long)r.follow_at);
        // ldr q1, [x20, #208] / fsub v0.4s, v1.4s, v0.4s / stur q0, [x29, #-192] / ldr s0, [x20, #320] ... str s0, [x20, #324]
        const uint32_t* f2 = reinterpret_cast<const uint32_t*>(rt.main.base() + r.follow2_at);
        g_follow2_ok = f2[0] == kFollow2Orig && f2[1] == 0x4EA0D420 && f2[3] == 0xBD414280 && f2[10] == 0xBD014680;
        if (!g_follow2_ok)
            LOGW("hooks", "swipe camera: camera follow 2 not found at main+0x%lx", (unsigned long)r.follow2_at);
        else
            LOGI("hooks", "swipe camera: the camera does not follow the character while swiping");
        LOGI("hooks", "swipe camera: the camera's turn ramp is skipped while swiping (main+0x%lx)", (unsigned long)r.at);
    }

    for (const StickVecHook& h : kStickVecHooks) {
        if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
        // ldr x8,[x0,#8] / ldr x8,[x8,#0x20] / ldr s10,[x8,w1,sxtw #2] within the first 8 instructions
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.fn);
        bool ok = false;
        for (int k = 0; k < 8; k++) ok |= code[k] == 0xF9400408 && code[k + 1] == 0xF9401108;
        std::string err;
        if (!ok)
            LOGW("hooks", "stick vector not found at main+0x%lx", (unsigned long)h.fn);
        else if (rt.main.HookFunction(h.fn, reinterpret_cast<void*>(&StickVector), &err))
            LOGI("hooks", "swipe camera: the camera turns past the stick's top speed for fast swipes");
        else
            LOGW("hooks", "stick vector hook failed: %s", err.c_str());
    }

    if (kCameraDiagnostics) {
        for (const StickVecHook& h : kActionGetHooks) {
            if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
            // ldr x8,[x0,#8] / ldr x8,[x8,#32] / ldr s0,[x8,w1,sxtw #2] / ret
            const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.fn);
            std::string err;
            if (code[0] != 0xF9400408 || code[1] != 0xF9401108 || code[3] != 0xD65F03C0)
                LOGW("hooks", "action getter not found at main+0x%lx", (unsigned long)h.fn);
            else if (rt.main.HookFunction(h.fn, reinterpret_cast<void*>(&ActionGet), &err))
                LOGI("hooks", "action getter: probe");
            else
                LOGW("hooks", "action getter hook failed: %s", err.c_str());
        }

        for (const TurnHook& h : kTurnHooks) {
            if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
            for (int k = 0; k < 2; k++) {
                // ldr x0,[x0,#8] / mov w1,#26+k / bl / adrp / ldr s1 / fmul s0,s0,s1
                const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.fn[k]);
                if (code[2] != 0xF9400400 || code[3] != 0x52800341u + (uint32_t(k) << 5) || code[7] != 0x1E210800) {
                    LOGW("hooks", "camera turn getter %d not found at main+0x%lx", k, (unsigned long)h.fn[k]);
                    continue;
                }
                // the scale: adrp (page) + ldr s1, [x8, #imm]
                const uint64_t at = h.fn[k] + 20;
                const uint32_t adrp = code[5], ldr = code[6];
                const int64_t imm = int64_t((((adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 3)) << 44 >> 32;
                const uint64_t page = (at & ~0xFFFull) + uint64_t(imm);
                memcpy(&g_turn_scale[k], reinterpret_cast<const void*>(rt.main.base() + page + ((ldr >> 10) & 0xFFF) * 4), 4);
                std::string err;
                if (rt.main.HookFunction(h.fn[k], reinterpret_cast<void*>(k ? &TurnGetter<1> : &TurnGetter<0>), &err))
                    LOGI("hooks", "camera turn getter %d: probe (scale %.5f)", k, g_turn_scale[k]);
                else
                    LOGW("hooks", "camera turn getter %d hook failed: %s", k, err.c_str());
            }
        }

        for (const ActionSetHook& h : kActionSetHooks) {
            if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
            // ldr x9,[x0,#208] / ldr x10,[x0,#248] / sbfiz x11,x1,#2,#32
            const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.setter);
            std::string err;
            if (code[0] != 0xF9406809 || code[1] != 0xF9407C0A)
                LOGW("hooks", "input action setter not found at main+0x%lx", (unsigned long)h.setter);
            else if (rt.main.HookFunction(h.setter, reinterpret_cast<void*>(&ActionSet), &err))
                LOGI("hooks", "input action probe: logging the actions the sticks drive");
            else
                LOGW("hooks", "input action hook failed: %s", err.c_str());
        }

    }

    for (const MapEntryHook& h : kMapEntryHooks) {
        if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
        // ldr x9,[x0,#8] / orr w10,wzr,#0x1c / smull x11,w1,w10 / ldr w9,[x9,x11]
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.getter);
        std::string err;
        if (code[0] != 0xF9400409 || code[1] != 0x321E0BEA || code[2] != 0x9B2A7C2B)
            LOGW("hooks", "input mapping getter not found at main+0x%lx", (unsigned long)h.getter);
        else if (rt.main.HookFunction(h.getter, reinterpret_cast<void*>(&InputMapEntry), &err))
            LOGI("hooks", "input mapping: channels allowed past full tilt (only the touch swipe camera sends more)");
        else
            LOGW("hooks", "input mapping hook failed: %s", err.c_str());
    }

    for (const ActionMapHook& h : kActionMapHooks) {
        if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
        // ldr x9,[x0,#8] / orr w10,wzr,#0x1c / smull x11,w1,w10 / ldr w9,[x9,x11] / str w9,[x2] ... ldr x8,[sp] (+0x5c)
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.fn);
        std::string err;
        if (code[0] != 0xF9400409 || code[1] != 0x321E0BEA || code[2] != 0x9B2A7C2B || code[23] != 0xF94003E8 ||
            code[30] != 0xD65F03C0)
            LOGW("hooks", "action mapping getter not found at main+0x%lx", (unsigned long)h.fn);
        else if (rt.main.HookFunction(h.fn, reinterpret_cast<void*>(&ActionMapEntry), &err))
            LOGI("hooks", "action mapping: actions allowed past full tilt while the swipe camera is");
        else
            LOGW("hooks", "action mapping hook failed: %s", err.c_str());
    }

    if (!kCameraDiagnostics) return;
    const PadProbe* p = nullptr;
    for (const PadProbe& c : kPadProbes)
        if (!strncmp(c.build_id, b.build_id, strlen(c.build_id))) p = &c;
    if (!p) return;
    // The getter, as in every build: sub w8,w1,#3 / cmp w8,#5 / b.hi / add x8,x0,w8,sxtw #2 / ldr s0,[x8,#base]
    const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + p->getter);
    if (code[0] != 0x51000C28 || code[1] != 0x7100151F || (code[4] & 0xFFC003FF) != 0xBD400100) {
        LOGW("hooks", "pad probe: getter not found at main+0x%lx", (unsigned long)p->getter);
        return;
    }
    g_pad_analog_base = ((code[4] >> 10) & 0xFFF) * 4;
    g_main_base = rt.main.base();
    g_main_end = g_main_base + 0x8000000;
    std::string err;
    if (rt.main.HookFunction(p->getter, reinterpret_cast<void*>(&PadAnalogProbe), &err))
        LOGI("hooks", "pad probe: logging which code reads the sticks (getter main+0x%lx, values at +0x%x)",
             (unsigned long)p->getter, g_pad_analog_base);
    else
        LOGW("hooks", "pad probe not installed: %s", err.c_str());
}

// The engine's screen size block (1.0.0 main+0x1aeba90, 0x200 past GameBuild::shadow_size in every build):
// int width, height; float width, height; int half width, half height; float half width, half height, with
// the defaults 1280x720 / 640x360. The renderer fills it from the resolution getters when it starts
// (main+0x101d80), but a static initializer (main+0x3751c4) copies the half size into two globals before that,
// and the projected-decal pass sizes its colour target (main+0x3611c8) and its 1/size constants (main+0x36e9b4)
// from those copies. Below a 1280x720 render (render scale, performance mode) the target stayed 640x360 beside
// the game's own half-size depth (e.g. 484x272): decals were drawn into its top-left part only and showed in a
// rectangle at the top left of the screen. The defaults are set to the render size before the game starts.
static void ApplyScreenSizeDefaults(Runtime& rt, const GameBuild& b, int width, int height) {
    struct ScreenSize {
        int32_t w, h;
        float fw, fh;
        int32_t hw, hh;
        float fhw, fhh;
    };
    auto* s = reinterpret_cast<ScreenSize*>(rt.main.base() + b.shadow_size + 0x200);
    if (!rt.main.Contains(reinterpret_cast<uintptr_t>(s)) ||
        !rt.main.Contains(reinterpret_cast<uintptr_t>(s) + sizeof(ScreenSize) - 1) || s->w != 1280 || s->h != 720 ||
        s->fw != 1280.0f || s->fh != 720.0f || s->hw != 640 || s->hh != 360 || s->fhw != 640.0f || s->fhh != 360.0f) {
        LOGW("hooks", "screen size defaults not found: decals may show in part of the screen below 1280x720");
        return;
    }
    const int hw = width / 2, hh = height / 2;  // as the engine computes them (float * 0.5, truncated)
    *s = {width, height, float(width), float(height), hw, hh, float(hw), float(hh)};
    LOGI("hooks", "screen size defaults: %dx%d, half %dx%d (decal target)", width, height, hw, hh);
}

// Shadow quality (settings shadow_quality 0-2; from pull request #47): the shadow map size global the engine sizes its
// shadow target from (see GameBuild::shadow_size) at 512 (0 and 1) or 1024 (2). With 0 the renderer also leaves the
// shadow pass out and answers every lookup "lit" (vk_backend.cpp), so the small map only saves memory.
void ApplyShadowQuality(Runtime& rt, const GameBuild& b) {
    const int q = rt.config.shadow_quality;
    if (q >= 3) return;
    const int size = q == 2 ? 1024 : 512;
    int32_t* shadow = reinterpret_cast<int32_t*>(rt.main.base() + b.shadow_size);
    if (rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) && *shadow == 2048) {
        *shadow = size;
        LOGI("hooks", "shadow quality %d: shadow maps %dx%d%s", q, size, size, q == 0 ? ", shadows off" : "");
    } else {
        LOGW("hooks", "shadow quality: shadow map size not changed (found %d)%s",
             rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) ? *shadow : -1, q == 0 ? "; shadows still off" : "");
    }
}

// Lighter post-processing: no ambient occlusion, depth of field, camera motion blur or light shafts (see GameBuild).
// All or nothing: every instruction is checked first.
void ApplyLighterPostFx(Runtime& rt, const GameBuild& b) {
    struct P {
        uint64_t off;
        uint32_t from, to;
    };
    const P patches[] = {
        {b.ao[0], 0x394B2A60, 0x52800000},       // ldrb w0, [x19, #0x2ca]  -> mov w0, #0
        {b.ao[1], 0x394B2A77, 0x52800017},       // ldrb w23, [x19, #0x2ca] -> mov w23, #0
        {b.dof, 0x394B8668, 0x52800008},         // ldrb w8, [x19, #0x2e1]  -> mov w8, #0
        {b.camblur[0], 0x394A5668, 0x52800028},  // ldrb w8, [x19, #0x295]  -> mov w8, #1
        {b.camblur[1], 0x394A5668, 0x52800028},
        {b.lightshaft, 0x39400108, 0x52800008},  // ldrb w8, [x8]           -> mov w8, #0
    };
    for (const P& p : patches) {
        const uint32_t* at = reinterpret_cast<const uint32_t*>(rt.main.base() + p.off);
        if (!rt.main.Contains(reinterpret_cast<uintptr_t>(at)) || *at != p.from) {
            LOGW("hooks", "lighter post-processing not applied: main+0x%llx is not as expected",
                 (unsigned long long)p.off);
            return;
        }
    }
    std::string err;
    for (const P& p : patches)
        if (!rt.main.PatchInstruction(p.off, p.from, p.to, &err)) {
            LOGW("hooks", "lighter post-processing: %s", err.c_str());
            return;
        }
    LOGI("hooks", "lighter post-processing: no ambient occlusion, depth of field, camera motion blur or light shafts "
                  "(bloom: not drawn by the renderer)");
}

// Effect cluster overflow (a crash in the game itself). The render tasks draw a particle effect cluster
// (FrpgFxClusterAppearance_Model.cpp) by writing each of its pieces into per-thread arrays with room for 384 (16, 4,
// 4 and 64 bytes a piece at buffer+0x210, +0x1a10, +0x2010, +0x2610). The lock guarding the buffer lies right after
// the last 64-byte slot (buffer+0x8618; 1.0.0 +0x8608). With more pieces the copy goes on past the end: piece 385's
// matrix overwrites the lock, and unlocking it at the end of the function calls through that data (SIGSEGV on MTR_n,
// pc main+0x624868 in DV 1.0.3; seen during a boss attack, with 390 pieces). The game does check the count, but only
// to report "error !" (nothing in a release build), and 1.0.3 checks it against a limit at buffer+0x200 that is
// larger than 384 (it let 390 through). The count is now clamped before the loop to 384 (1.0.3: and that limit),
// in the register and in the stack copy the loop and the final draw read back: an oversized cluster shows its first
// 384 pieces for that frame.
struct FxClusterFix {
    const char* build_id;
    uint64_t at;
    int n;
    uint32_t from[9], to[9];
};
// 1.0.3: ldr w8,[x19,#0x200] / cmp w23,w8 / b.ls ok / adrp x0 / adrp x2 / add x0 / add x2 / mov w1,#444 / bl report
//   ->   ldr w8,[x19,#0x200] / mov w9,#384 / cmp w8,w9 / csel w8,w8,w9,lo / cmp w23,w8 / csel w23,w23,w8,ls /
//        str x23,[sp,#0x1e0] / nop / nop   (w9 is written again before it is next read)
// 1.0.0 checks against 384 itself (cmp w23,#0x181); its report call (adrp x0 / adrp x2 / add x0, reached only when
// the count is too large) -> mov w23,#384 / str x23,[sp,#0x308] / b past the report.
#define FX103_TO {0xB9420268, 0x52803009, 0x6B09011F, 0x1A893108, 0x6B0802FF, 0x1A8892F7, 0xF900F3F7, kNop, kNop}
const FxClusterFix kFxClusterFixes[] = {
    {"5a797a3a", 0x628c5c, 3, {0xB0007C20, 0xB0007C22, 0x912BCC00}, {0x52803017, 0xF90187F7, 0x14000004}},
    {"6569410c", 0x62398c, 9,
     {0xB9420268, 0x6B0802FF, 0x540000E9, 0x90007BE0, 0x90007BE2, 0x9131D800, 0x91338C42, 0x52803781, 0x94057D00},
     FX103_TO},
    {"df3766a2", 0x62395c, 9,
     {0xB9420268, 0x6B0802FF, 0x540000E9, 0xD0007C00, 0xD0007C02, 0x9133D800, 0x91358C42, 0x52803781, 0x94058339},
     FX103_TO},
};
#undef FX103_TO

void ApplyFxClusterFix(Runtime& rt) {
    const FxClusterFix* f = nullptr;
    for (const FxClusterFix& x : kFxClusterFixes)
        if (rt.main_nso.build_id_hex.rfind(x.build_id, 0) == 0) f = &x;
    if (!f) return;
    for (int i = 0; i < f->n; ++i) {
        const uint32_t* at = reinterpret_cast<const uint32_t*>(rt.main.base() + f->at + 4 * i);
        if (!rt.main.Contains(reinterpret_cast<uintptr_t>(at)) || *at != f->from[i]) {
            LOGW("hooks", "effect cluster fix not applied: main+0x%llx is not as expected",
                 (unsigned long long)(f->at + 4 * i));
            return;
        }
    }
    std::string err;
    for (int i = 0; i < f->n; ++i)
        if (!rt.main.PatchInstruction(f->at + 4 * i, f->from[i], f->to[i], &err)) {
            LOGW("hooks", "effect cluster fix: %s", err.c_str());
            return;
        }
    LOGI("hooks", "effect cluster fix: effects with more than 384 pieces are drawn in part instead of overwriting memory");
}

void SkipIntroLogos(Runtime& rt, const GameBuild& b) {
    const LogoSkip* p = nullptr;
    for (const LogoSkip& l : kLogoSkips)
        if (!strncmp(l.build_id, b.build_id, strlen(l.build_id))) p = &l;
    if (!p) return;
    struct Edit { uint64_t at; uint32_t from, to; };
    const Edit edits[] = {
        {p->open[0], p->open_insn[0], kNop},
        {p->open[1], p->open_insn[1], kNop},
        {p->timer, kLdrS1Timer, 0x14000000u | uint32_t(((p->timer_done - p->timer) / 4) & 0x3FFFFFF)},  // b
    };
    std::string err;
    size_t done = 0;
    while (done < std::size(edits) && rt.main.PatchInstruction(edits[done].at, edits[done].from, edits[done].to, &err))
        done++;
    if (done == std::size(edits)) {
        LOGI("hooks", "skip_intro: the intro logos are skipped");
        return;
    }
    LOGW("hooks", "skip_intro: patch failed (%s): the logos are shown", err.c_str());
    std::string undo_err;
    while (done-- > 0) rt.main.PatchInstruction(edits[done].at, edits[done].to, edits[done].from, &undo_err);
}

// ---- dynamic frame rate (fps = auto) ---------------------------------------------------------------------------
// After PR #13 ("Dynamic FPS" mods for console ports): the game's time step already follows the real frame time
// (FrameLimiter), so the game keeps its speed at any rate; this half picks the rate. A phone screen shows 60, 30, 20
// and 15 fps with even frame spacing (they divide 60 and 90 Hz), 22-25 fps would alternate short and long frames, so
// the cap moves between those rates with hysteresis: down quickly when frames do not fit their period, up slowly and
// only with clear headroom. Not while a video plays: videos are paced at the game version's movie rate.
std::atomic<int> g_movies{0};
std::atomic<int> g_cap_fps{60};  // the rate the limiter paces to with fps = auto
std::atomic<float> g_gpu_frame_ms{0.0f};
bool MoviePlaying() { return g_movies.load(std::memory_order_relaxed) > 0; }
int MovieFps() { return g_build ? g_build->movie_fps : 30; }
int MinFps() { return std::clamp(GetRuntime().config.min_fps, 15, 30); }

struct DynamicFps {
    double fast = 0, slow = 0;  // smoothed cost of a frame in ms (fast: ~7 frames, slow: ~90 frames)
    int over = 0, under = 0, hold = 0;
};
DynamicFps g_dyn;

int DynamicRates(int out[6]) {  // highest first: 60, 30, 20, 15 down to min_fps
    static constexpr int kRates[] = {60, 30, 20, 15};
    const int lo = MinFps();
    int n = 0;
    for (int r : kRates)
        if (r >= lo) out[n++] = r;
    if (out[n - 1] > lo) out[n++] = lo;
    return n;
}

void DynamicStep(double work_ms) {
    DynamicFps& s = g_dyn;
    int rates[6];
    const int n = DynamicRates(rates);
    int cap = g_cap_fps.load(std::memory_order_relaxed);
    int idx = n - 1;
    for (int i = 0; i < n; i++)
        if (rates[i] <= cap) {
            idx = i;
            break;
        }
    cap = rates[idx];
    const double period = 1000.0 / cap;
    // A frame's cost: what the game thread took, or the GPU's time if that is longer (with the cap leaving the GPU
    // idle, the game thread alone looks cheap). A loading stall is not the cost of a frame.
    const double gpu = g_gpu_frame_ms.load(std::memory_order_relaxed);
    const double cost = std::min(std::max(work_ms, gpu * 1.03), period * 3.0);
    if (s.fast == 0) s.fast = s.slow = cost;
    s.fast += 0.15 * (cost - s.fast);
    s.slow += (1.0 / 90.0) * (cost - s.slow);
    if (s.hold > 0) s.hold--;
    s.over = s.fast > period * 0.98 ? s.over + 1 : 0;
    if (s.over >= 10 && idx + 1 < n) {
        int next = idx + 1;
        while (next + 1 < n && 1000.0 / rates[next] < s.fast * 1.06) next++;
        LOGI("hooks", "fps auto: %d -> %d fps (frame cost %.1f ms, GPU %.1f ms)", cap, rates[next], s.fast, gpu);
        g_cap_fps.store(rates[next], std::memory_order_relaxed);
        s.over = s.under = 0;
        s.slow = s.fast;
        s.hold = 900;  // ~30-45 s before trying a higher rate again
        return;
    }
    if (idx > 0 && s.hold == 0) {
        s.under = s.slow * 1.15 < 1000.0 / rates[idx - 1] ? s.under + 1 : 0;
        if (s.under >= 240) {
            LOGI("hooks", "fps auto: %d -> %d fps (frame cost %.1f ms, GPU %.1f ms)", cap, rates[idx - 1], s.slow, gpu);
            g_cap_fps.store(rates[idx - 1], std::memory_order_relaxed);
            s.under = 0;
            s.hold = 300;
        }
    } else {
        s.under = 0;
    }
}

// Replacement for the game's frame limiter (1.0.0: main+0x70bd90, 1.0.3: main+0x706d28; this, fps): called once
// per game update on the render thread. Same contract as the original: skip when the engine says so (1.0.0: a
// global flag; 1.0.3: a flag in `this`); sleep until a frame period has passed since the last call; remember the
// tick (1.0.0: this+0x310). Then the game's time step is set to the period that actually passed, and the
// frame-skip state (1.0.0: this+0x31d/0x31e/0x320) is kept clear.
void FrameLimiter(uint8_t* self, int /*fps*/) {
    const Runtime& rt = GetRuntime();
    const GameBuild& b = *g_build;
    if (b.limiter_global ? *reinterpret_cast<const int32_t*>(rt.main.base() + b.limiter_global) != 0
                         : self[b.limiter_enabled] == 0)
        return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    auto now = clock::now();
    // fps = auto: what this frame cost before any sleeping (videos are paced at their own rate).
    if (rt.config.dynamic_fps && last.time_since_epoch().count() != 0 && !MoviePlaying())
        DynamicStep(std::chrono::duration<double, std::milli>(now - last).count());
    const int fps = FrameRateNow();
    const auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
    if (last.time_since_epoch().count() != 0 && now < last + period) {
        std::this_thread::sleep_until(last + period);
        now = clock::now();
    }
    const double seconds = last.time_since_epoch().count() ? std::chrono::duration<double>(now - last).count() : 1.0 / fps;
    last = now;
    // Up to 1/20 s per step (1/min_fps with fps = auto and min_fps 15): slower devices keep the game at normal speed
    // instead of slow motion.
    const double slowest = rt.config.dynamic_fps ? std::min(MinFps(), 20) : 20;
    // Down to 1/60 s (fps = unlocked: one period of the screen's refresh rate).
    const double fastest = rt.config.unlocked_fps && !rt.config.dynamic_fps ? std::max(60, FixedFps(rt.config)) : 60;
    if (g_game_step) {
        *g_game_step = float(std::clamp(seconds, 1.0 / fastest, 1.0 / slowest));
        SetGrazeFactors(*g_game_step);
    }
    *reinterpret_cast<int64_t*>(self + b.last_tick) = hle::os::NowTicks();
    self[b.skip_active] = 0;
    self[b.skip_toggle] = 0;
    *reinterpret_cast<double*>(self + b.skip_time) = 0;
}

void Trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

uintptr_t GuestOffset(const void* p) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const loader::Module& m = GetRuntime().main;
    return m.Contains(a) ? a - m.base() : 0;
}

// Engine debug print (1.0.0: main+0x7fea18), (int level, const char* fmt, ...)
void EngineLog(int level, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// The same with a va_list (1.0.0: main+0x7feb30)
void EngineLogV(int level, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// DL_PANIC(const char* file, int line, const char* fmt, ...) (1.0.0: main+0x7892d8)
void EnginePanic(const char* file, int line, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    Trim(msg);
    const char* base = file ? strrchr(file, '/') : nullptr;
    FATAL("engine", "DL_PANIC: %s  (%s:%d, called from main+0x%lx)", msg, base ? base + 1 : (file ? file : "?"), line,
          (unsigned long)GuestOffset(__builtin_return_address(0)));
}

// ---- aspect ratio: camera aspect (experimental) --------------------------------------------------------------
// With a wider or narrower render resolution the 3D view still came out stretched: the camera that renders the
// scene keeps a 16:9 aspect ratio. Cameras keep {fov, aspect, near, far} at +0x50..+0x5c (main+0xfa7f4 reads
// fov and aspect for the view frustum). This scans the guest heap and the game's data for such blocks with a
// 16:9 aspect, logs them (with the owner's vtable, to find the code that sets it) and writes the wanted aspect
// into them, again every second (rescanning every 2 s until cameras are found, then every 10 s).
float g_aspect = 0;
void CameraAspectThread() {
    pthread_setname_np(pthread_self(), "dsr-aspect");
    const loader::Module& m = GetRuntime().main;
    std::vector<float*> found;
    int scans = 0;
    auto plausible = [](const float* a) {  // a = &aspect: fov before it, near and far after
        const float fov = a[-1], nearz = a[1], farz = a[2];
        return fov > 0.2f && fov < 2.5f && nearz > 0.001f && nearz < 50.0f && farz > nearz && farz < 1e6f;
    };
    for (;;) {
        const auto scan_start = std::chrono::steady_clock::now();
        // Scan: 16:9 exactly (0x3FE38E39) at a 4-byte boundary.
        uintptr_t hb = 0;
        size_t hs = 0;
        hle::os::GuestHeapRange(&hb, &hs);
        std::vector<std::pair<uintptr_t, size_t>> ranges;
        if (hb && hs) ranges.push_back({hb, hs});
        const loader::NsoImage& nso = GetRuntime().main_nso;  // .data + .bss
        const size_t data = nso.data.mem_offset, data_size = size_t(nso.data.size) + nso.bss_size;
        if (data && m.size() > data) ranges.push_back({m.base() + data, std::min<size_t>(m.size() - data, data_size)});
        int new_found = 0;
        for (const auto& [base, size] : ranges) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(base + 4);
            const uint32_t* end = reinterpret_cast<const uint32_t*>(base + size - 12);
            for (; p < end; p++) {
                if (*p != 0x3FE38E39u) continue;
                float* a = reinterpret_cast<float*>(const_cast<uint32_t*>(p));
                if (!plausible(a)) continue;
                if (std::find(found.begin(), found.end(), a) != found.end()) continue;
                found.push_back(a);
                new_found++;
                const uintptr_t obj = reinterpret_cast<uintptr_t>(a) - 0x54;
                uintptr_t vt = 0;
                if ((obj & 7) == 0) memcpy(&vt, reinterpret_cast<const void*>(obj), 8);
                if (found.size() <= 40)
                    LOGI("hooks", "camera aspect: 16:9 at %p (fov %.3f near %.3f far %.1f), object vtable %s0x%lx", a,
                         a[-1], a[1], a[2], m.Contains(vt) ? "main+" : "", (unsigned long)(m.Contains(vt) ? vt - m.base() : vt));
            }
        }
        scans++;
        const double scan_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_start).count();
        if (new_found || scans <= 3 || scans % 60 == 0)
            LOGI("hooks", "camera aspect: scan %d (%.0f ms): %d new 16:9 cameras, %zu in all; setting them to %.3f", scans,
                 scan_ms, new_found, found.size(), g_aspect);
        const int rewrites = found.empty() ? 2 : 10;  // 2 s, then 10 s between scans
        for (int k = 0; k < rewrites; k++) {
            for (float* a : found)
                if (*a > 1.7777f && *a < 1.7779f && plausible(a)) *a = g_aspect;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

struct Hook {
    uint64_t offset;
    void* fn;
    const char* name;
};

}  // namespace

float GameFrameTime() { return g_game_step ? *g_game_step : 1.0f / 30.0f; }
void ReportGpuFrameTime(double ms) {
    if (!(ms > 0.0)) return;
    const float prev = g_gpu_frame_ms.load(std::memory_order_relaxed);
    g_gpu_frame_ms.store(prev > 0.0f ? prev + 0.1f * (float(ms) - prev) : float(ms), std::memory_order_relaxed);
}
void SetMoviePlaying(bool playing) {
    playing ? ++g_movies : --g_movies;
    LOGI("hooks", "movie %s: frame rate %d fps", playing ? "started" : "ended", FrameRateNow());
}
bool FrameRateAdjustable() { return g_game_step != nullptr; }
namespace {
std::atomic<int> g_gpu_has_bc{-1};
}
void SetGpuHasBc(bool has) { g_gpu_has_bc = has ? 1 : 0; }
int GpuHasBc() { return g_gpu_has_bc.load(); }
int FrameRateNow() {
    if (MoviePlaying()) return MovieFps();
    const RuntimeConfig& c = GetRuntime().config;
    return c.dynamic_fps ? g_cap_fps.load(std::memory_order_relaxed) : FixedFps(c);
}

const char* GameVersion() { return g_build ? g_build->version : nullptr; }

void InstallGameHooks() {
    Runtime& rt = GetRuntime();
    for (const GameBuild& b : kBuilds)
        if (rt.main_nso.build_id_hex.rfind(b.build_id, 0) == 0) g_build = &b;
    if (g_build && !rt.config.game_hooks) {
        LOGW("hooks", "game version %s: engine patches off (settings game_hooks = 0): 30 fps, 16:9, no engine log",
             g_build->version);
        rt.config.target_fps = 30;
        rt.config.dynamic_fps = false;
        rt.config.unlocked_fps = false;
        return;
    }
    if (!g_build) {
        LOGW("hooks", "unknown game build %.32s: engine hooks disabled (supported: 1.0.0, 1.0.3)",
             rt.main_nso.build_id_hex.c_str());
        rt.config.dynamic_fps = false;  // no frame limiter of ours: the game paces itself
        rt.config.unlocked_fps = false;
        return;
    }
    const GameBuild& b = *g_build;
    LOGI("hooks", "game version %s (build %.16s...)", b.version, rt.main_nso.build_id_hex.c_str());
    // The program and the data files must be the same version (e.g. the 1.0.3 exefs over 1.0.0 data is not).
    if (FILE* f = fopen((rt.config.game_dir + "/DataVersion.txt").c_str(), "r")) {
        char data[64] = {};
        if (fgets(data, sizeof(data), f)) {
            Trim(data);
            for (char* e = data + strlen(data); e > data && e[-1] == ' '; *--e = 0) {}
            for (const GameBuild& o : kBuilds)
                if (strcmp(o.data_version, b.data_version) != 0 && strcmp(data, o.data_version) == 0)
                    LOGW("hooks", "the game's data files are version %s (DataVersion %s) but the program is %s: "
                         "install the matching data (the update's RomFS) or the game may misbehave", o.version, data,
                         b.version);
            if (strcmp(data, b.data_version) == 0) LOGI("hooks", "data files: version %s (%s)", b.version, data);
        }
        fclose(f);
    }
    // Frame rate (fps = 60; offsets below are 1.0.0's). The same two places a well-known Switch 60 fps mod changes,
    // done differently:
    // 1. The game's per-frame update (main+0x66c99c) steps every system by a constant 1/30 s loaded from
    //    read-only data (main+0x66c9c8 adrp x8, page; +0x66c9cc ldr s8, [x8, #0x970]). It now loads a float in
    //    the scratch page instead.
    // 2. The render thread's frame limiter (main+0x70bd90, called once per game update with a hard-coded 30) is
    //    replaced by FrameLimiter: it paces to 60 and sets that float to the measured frame period (1/60..1/20 s),
    //    so the game keeps normal speed at 60 fps and when heavy areas run slower. It also keeps the game's
    //    frame-skip mode off (with it, slow frames ran two updates per shown frame).
    // Installed at every frame rate (30 included), so the cap can be changed while the game runs (in-game
    // settings menu): FrameLimiter reads the configured rate every frame.
    {
        std::string err;
        float* step = reinterpret_cast<float*>(rt.main.ScratchPage());
        *step = 1.0f / 30.0f;
        const uint32_t adrp = rt.main.EncodeAdrp(b.step_adrp, 8, reinterpret_cast<uintptr_t>(step));
        const bool adrp_ok = rt.main.PatchInstruction(b.step_adrp, b.step_adrp_insn, adrp, &err);
        bool ok = adrp_ok && rt.main.PatchInstruction(b.step_adrp + 4, b.step_ldr_insn, 0xBD400108, &err);  // ldr s8, [x8]
        if (ok) ok = rt.main.HookFunction(b.limiter, reinterpret_cast<void*>(&FrameLimiter), &err);
        if (ok) {
            g_game_step = step;
            ApplyGrazeFix(rt, step + 1);
            if (rt.config.dynamic_fps)
                LOGI("hooks", "frame rate: own frame limiter, fps auto (60 / 30 / 20 fps, not below %d; changeable in game), "
                     "game time step follows the frame time", MinFps());
            else if (rt.config.unlocked_fps)
                LOGI("hooks", "frame rate: own frame limiter, refresh rate (experimental): up to %d fps, the screen's refresh "
                     "rate (changeable in game); game time step follows the frame time, physics may misbehave above 60",
                     FixedFps(rt.config));
            else
                LOGI("hooks", "frame rate: own frame limiter at %d fps (changeable in game), game time step follows the frame time",
                     FixedFps(rt.config));
        } else {
            LOGW("hooks", "60 fps patches failed (%s): staying at 30 fps", err.c_str());
            rt.main.PatchInstruction(b.step_adrp + 4, 0xBD400108, b.step_ldr_insn, &err);  // undo what applied
            if (adrp_ok) rt.main.PatchInstruction(b.step_adrp, adrp, b.step_adrp_insn, &err);
            rt.config.target_fps = 30;
            rt.config.dynamic_fps = false;
            rt.config.unlocked_fps = false;
        }
    }
    // Aspect ratio (offsets below are 1.0.0's; kBuilds has each build's). Two places set the game's resolution, both
    // with built-in defaults:
    // - the render resolution, read from the game's settings with defaults 1280 / 720 (main+0x6a16b0:
    //   "Normal.Resolution.Width" main+0x6a170c mov w2, #1280; "...Height" main+0x6a1758 mov w2, #720), and
    //   again by the width getter main+0x6a18cc (main+0x6a18f8 mov w2, #1280) whose callers (main+0x101d80,
    //   +0x66e99c, +0x66e9cc) size the post-processing buffers and their copies of the scene. It
    //   also becomes the screen size floats (main+0x1af4a7c / +0x1af4a84) the cameras take their aspect ratio
    //   from (e.g. main+0xbca88), so the 3D view widens (or narrows) instead of stretching;
    // - the handheld display mode (main+0x716158: main+0x716170 mov w1, #1280; +0x716178 mov w2, #720).
    // Frame buffers are allocated at 1920x1080 (main+0x70a20c), the limit. The height always stays 720: the
    // game's half- and quarter-size buffers (640x360, 320x180) keep their 720-line sizes whatever the render
    // resolution, and a 4:3 test at 1280x960 died silently on loading a save. Only the width changes: wider
    // than 16:9 shows more at the sides, narrower (4:3: 960x720) less.
    // Render scale (settings render_scale, 0.5 - 1.0): the same resolution settings at a lower size, height and
    // width together (the height also has a getter with its own default, main+0x6a193c: main+0x6a1968 mov w2, #720).
    // The game draws less and the frame is enlarged to the screen at the end. Heights stay at or below 720 (the
    // fixed half- and quarter-size buffers are then larger than needed). One half-size target is sized from the
    // engine's screen size defaults instead (ApplyScreenSizeDefaults), which follow the render size.
    if (!rt.config.docked) {
        float ar = 16.0f / 9.0f;
        bool ar_ok = true;
        if (rt.config.aspect_ratio == "auto") {
            ar = rt.config.screen_aspect;
        } else if (rt.config.aspect_ratio != "16:9") {
            float w = 0, h = 0;
            if (sscanf(rt.config.aspect_ratio.c_str(), "%f:%f", &w, &h) == 2 && h > 0) ar = w / h;
            else ar = 0;
        }
        if (!(ar > 0.5f && ar < 4.0f)) {
            LOGW("hooks", "aspect_ratio %s: not understood (or screen size unknown), staying at 16:9",
                 rt.config.aspect_ratio.c_str());
            ar = 16.0f / 9.0f;
            ar_ok = false;
        }
        const float scale = std::clamp(rt.config.render_scale, 0.5f, 1.0f);
        const int height = std::clamp(int(std::lround(720.0f * scale / 8.0f)) * 8, 360, 720);
        const int width = std::clamp(int(std::lround(float(height) * ar / 8.0f)) * 8, 320, 1920);
        if (width != 1280 || height != 720) {
            std::string err;
            auto mov = [](int rd, int v) { return 0x52800000u | (uint32_t(v) << 5) | uint32_t(rd); };
            bool ok = true;
            if (width != 1280)
                ok = ok && rt.main.PatchInstruction(b.width_cfg, 0x5280A002, mov(2, width), &err) &&
                     rt.main.PatchInstruction(b.width_get, 0x5280A002, mov(2, width), &err) &&
                     rt.main.PatchInstruction(b.display_width, 0x5280A001, mov(1, width), &err);
            if (height != 720)
                ok = ok && rt.main.PatchInstruction(b.height_cfg, 0x52805A02, mov(2, height), &err) &&
                     rt.main.PatchInstruction(b.height_get, 0x52805A02, mov(2, height), &err) &&
                     rt.main.PatchInstruction(b.display_height, 0x52805A02, mov(2, height), &err);
            if (ok) {
                LOGI("hooks", "aspect ratio %s (%.3f), render scale %.2f: the game renders at %dx%d (was 1280x720)",
                     ar_ok ? rt.config.aspect_ratio.c_str() : "16:9", ar, scale, width, height);
                g_aspect = float(width) / float(height);
                rt.config.render_width = width;
                rt.config.render_height = height;
                ApplyScreenSizeDefaults(rt, b, width, height);
                if (std::fabs(g_aspect - 16.0f / 9.0f) > 0.01f) std::thread(CameraAspectThread).detach();
            } else {
                LOGW("hooks", "resolution patch failed: %s", err.c_str());
            }
        }
    }
    ApplyFxClusterFix(rt);
    if (rt.config.skip_intro) SkipIntroLogos(rt, b);
    if (rt.config.performance_mode) ApplyPerformanceMode(rt, b);
    ApplyShadowQuality(rt, b);
    if (rt.config.lighter_post_fx) ApplyLighterPostFx(rt, b);
    {
        constexpr uint32_t kAdd2MiB = 0x11480108;  // add w8, w8, #0x200, lsl #12
        constexpr uint32_t kAdd8MiB = 0x11600108;  // add w8, w8, #0x800, lsl #12
        std::string err;
        if (rt.main.PatchInstruction(b.dyn_buffer, kAdd2MiB, kAdd8MiB, &err))
            LOGI("hooks", "dynamic buffers: 8 MiB each (were 2 MiB)");
        else
            LOGW("hooks", "dynamic buffer size not changed (%s)", err.c_str());
    }
    InstallCameraInputProbe(rt, b);
    const Hook hooks[] = {
        {b.log, reinterpret_cast<void*>(&EngineLog), "engine log"},
        {b.vlog, reinterpret_cast<void*>(&EngineLogV), "engine vlog"},
        {b.panic, reinterpret_cast<void*>(&EnginePanic), "DL_PANIC"},
    };
    for (const Hook& h : hooks) {
        std::string err;
        if (rt.main.HookFunction(h.offset, h.fn, &err))
            LOGI("hooks", "hooked %s at main+0x%lx", h.name, (unsigned long)h.offset);
        else
            LOGW("hooks", "could not hook %s: %s", h.name, err.c_str());
    }
}

}  // namespace dsr
