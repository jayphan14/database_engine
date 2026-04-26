#pragma once

#include "src/storage/disk_manager.h"

#include <cstddef>
#include <list>
#include <unordered_map>
#include <vector>

// Layer 2 of the storage stack: a fixed-size array of `Frame`s that caches
// pages from a DiskManager. Eviction is plain LRU among unpinned frames.
//
// ----------------------------------------------------------------------------
// Recommended usage: PageGuard (RAII)
// ----------------------------------------------------------------------------
//
// `fetchPage`/`unpinPage`/`newPage` are the low-level primitives. Mixing pin
// and unpin manually is error-prone:
//   - forgetting unpinPage permanently leaks a frame slot;
//   - dereferencing the returned Frame* after unpinPage is undefined.
//
// Prefer `pin()` and `pinNew()`, which return a `PageGuard` that calls
// `unpinPage` for you when it goes out of scope:
//
//   {
//       PageGuard g = bp.pin(page_id);     // pinned for the scope
//       std::memcpy(g->data, src, PAGE_SIZE);
//       g.markDirty();                     // <-- if you mutated the bytes
//   }                                       // unpin happens here
//
// Always call `markDirty()` if you wrote into `g->data`; otherwise the edits
// are silently dropped on eviction. PageGuard is move-only — pass it across
// function boundaries with std::move, never copy.

class BufferPool;  // forward declaration for PageGuard

// One slot in the buffer pool. Holds the bytes of a cached page plus the
// bookkeeping needed to evict it safely.
//
// `pin_count` is the buffer pool's most important invariant: while a frame
// has pin_count > 0, callers may hold raw pointers into `data` and the frame
// must not be evicted. fetchPage pins; unpinPage unpins.
struct Frame {
    char data[PAGE_SIZE];
    PageId page_id;     // INVALID_PAGE_ID if the slot is empty
    bool is_dirty;      // set when caller unpins with was_modified=true
    int pin_count;      // number of outstanding fetches
};

// RAII handle for a pinned page. Constructed only by BufferPool::pin /
// BufferPool::pinNew. On destruction, calls unpinPage on the buffer pool,
// passing whatever dirty flag has been set via markDirty().
//
// Move-only: copying would double-unpin on destruction.
class PageGuard {
public:
    PageGuard() = default;  // empty/disarmed (e.g. moved-from)
    ~PageGuard();

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& other) noexcept;
    PageGuard& operator=(PageGuard&& other) noexcept;

    // True if this guard currently owns a pin. Default-constructed and
    // moved-from guards are not valid.
    bool valid() const { return frame_ != nullptr; }

    // Direct access to the underlying Frame. Undefined if !valid().
    Frame& operator*()  const { return *frame_; }
    Frame* operator->() const { return frame_; }
    Frame* get()        const { return frame_; }

    // Mark the page as modified so the buffer pool writes it back before
    // evicting (or on flush). Call this whenever you mutate `data`. Idempotent.
    void markDirty() { dirty_ = true; }

private:
    friend class BufferPool;
    PageGuard(BufferPool* bp, Frame* frame) noexcept
        : bp_(bp), frame_(frame), dirty_(false) {}

    BufferPool* bp_ = nullptr;
    Frame* frame_ = nullptr;
    bool dirty_ = false;
};

class BufferPool {
public:
    BufferPool(size_t num_frames, DiskManager* disk);

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // ------------------------------------------------------------------------
    // Recommended high-level API
    // ------------------------------------------------------------------------

    // Pin `page_id` and return an RAII guard. The guard unpins on destruction.
    // Throws if every frame is currently pinned.
    PageGuard pin(PageId page_id);

    // Allocate a new page on disk and return a guard that owns its pin. The
    // page bytes are zeroed. Recover the new page's id via `g->page_id`.
    PageGuard pinNew();

    // ------------------------------------------------------------------------
    // Low-level primitives
    // ------------------------------------------------------------------------
    // Use these only if you need explicit control over the pin lifetime
    // (e.g. across non-RAII boundaries). Prefer pin()/pinNew() otherwise.

    Frame* fetchPage(PageId page_id);
    void   unpinPage(PageId page_id, bool was_modified);
    Frame* newPage(PageId* out_page_id);

    // ------------------------------------------------------------------------
    // Flushing
    // ------------------------------------------------------------------------

    // Flush a single cached page to disk (no-op if not cached or not dirty).
    void flushPage(PageId page_id);

    // Flush every dirty cached page to disk.
    void flushAll();

    size_t numFrames() const { return frames_.size(); }

private:
    DiskManager* disk_;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, size_t> page_table_;  // page_id -> frame index

    // LRU list of frame indices that are eviction candidates (pin_count == 0).
    // Front = least recently used (next victim). Empty frames live here too.
    std::list<size_t> lru_;
    std::unordered_map<size_t, std::list<size_t>::iterator> lru_pos_;

    // Pick a frame to evict (front of LRU). Throws if no frame is unpinned.
    size_t pickVictim();

    // Remove `frame_idx` from the LRU list. Caller asserts it is currently in.
    void removeFromLRU(size_t frame_idx);

    // Push `frame_idx` to the back of the LRU list (most recently used).
    void addToLRU(size_t frame_idx);

    // If the frame at `frame_idx` holds a page, write it back if dirty and
    // remove it from page_table_. Used before reusing the slot.
    void evict(size_t frame_idx);
};
