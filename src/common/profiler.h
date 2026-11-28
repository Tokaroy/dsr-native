#pragma once
// Sampling CPU profiler (diagnostics). Registered threads are sampled on their own CPU time (a SIGPROF per
// 2 ms of CPU used, so idle threads cost nothing); every 10 s the busiest places are logged per thread:
// where the CPU was (self), the first frame in our library and the first frame in the game's code.
// Addresses in our library are logged as dsr+0x<offset> for symbolization against the unstripped libdsr.so.

#include <cstdint>

namespace dsr::profiler {

// Turns profiling on (settings). Threads registered before this are sampled from now on.
void Enable();
bool Enabled();
// Samples the calling thread under `name` (any thread; cheap no-op while disabled).
void RegisterCurrentThread(const char* name);
// A frame that took long (frame limiter): what the registered threads ran on the CPU in [from_ns, to_ns]
// (CLOCK_MONOTONIC) is logged within a second, from the reporter thread (the samples stay in the ring ~10 s).
void NoteSlowFrame(int64_t from_ns, int64_t to_ns, double ms, const char* what = "slow frame");
// A thread was blocked (or in a slow file system call) over [from_ns, to_ns]: `who` the thread, `what` the call
// (copied), `where` its caller (a return address: logged as main+0x... / dsr+0x...). Kept for ~10 s; a slow
// frame's report lists the ones that covered much of it ("blocked: ..."). Only call for spans of a few ms on.
void NoteBlockedSpan(const char* who, const char* what, uintptr_t where, int64_t from_ns, int64_t to_ns);
// Called by the reporter thread every 10 s (after the CPU report): extra diagnostics lines (one hook).
void SetReportHook(void (*hook)());

}  // namespace dsr::profiler
