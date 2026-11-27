#pragma once
// Write tracking of guest heap pages (settings.txt geo_watch). The geometry cache re-hashed every vertex / index
// range a frame uses, every frame, to notice the game changing it (three worker threads reading tens of MB per
// frame). A range that stayed the same is instead watched: the pages wholly inside it are made read-only, and the
// first write to one (by the game, or by our own code) faults once, is noted and the page made writable again. A
// watched range whose pages saw no write since it was armed is unchanged and needs no hash (its partial first and
// last pages, which other data may share, are never protected: they are hashed, a few KB at most).
//
// Writes the fault handler cannot see must be announced with WatchBeforeWrite: the kernel writing guest memory
// (a file read into it fails with EFAULT on a read-only page instead of faulting) and pages dropped with madvise.
#include <cstddef>
#include <cstdint>

namespace dsr::hle::os {

// The guest heap's reserved address range (memory_time.cpp, once): sets up the page table and the fault handler,
// when settings.txt geo_watch is on.
void WatchSetHeap(uintptr_t base, size_t reserve);
bool WatchEnabled();
// Protects the pages wholly inside [p, p + size). Returns the arming epoch, or 0 when the range cannot be
// watched (outside the heap, no whole page, a page written too often, mprotect refused). After a non-zero
// return the caller must verify the content once more: a write that landed while the pages were being protected
// is not seen by the handler.
uint64_t WatchArm(const void* p, size_t size);
// True when no page wholly inside [p, p + size) was written since WatchArm returned `epoch`.
bool WatchClean(const void* p, size_t size, uint64_t epoch);
// [p, p + size) is about to be written by something that does not fault (the kernel, madvise).
void WatchBeforeWrite(const void* p, size_t size);
// The partial first and last pages of [p, p + size) (never protected): {offset, length} pairs; *n = 0..2.
struct WatchEdge {
    size_t off, len;
};
void WatchEdges(const void* p, size_t size, WatchEdge out[2], int* n);

struct WatchStats {
    uint64_t faults = 0;       // write faults handled (since the last call)
    uint64_t armed_pages = 0;  // pages protected (since the last call)
    uint64_t busy_pages = 0;   // pages no longer watched: written too often (in total)
};
WatchStats WatchTakeStats();

}  // namespace dsr::hle::os
