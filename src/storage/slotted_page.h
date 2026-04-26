#pragma once

#include "src/storage/disk_manager.h"  // for PAGE_SIZE

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

using SlotId = uint16_t;

// Layer 3 of the storage stack: the byte-level layout of a page.
//
// SlottedPage wraps a `char*` of exactly PAGE_SIZE bytes (typically the
// `data` field of a buffer pool Frame) and treats it as:
//
//   ┌────────────────────────────────────────────────────────┐
//   │ header (HEADER_SIZE bytes)                             │
//   ├────────────────────────────────────────────────────────┤
//   │ slot[0], slot[1], ... slot[num_slots-1]   ← grows down │
//   │                                                        │
//   │     ← free space →                                     │
//   │                                                        │
//   │ ...tuple bytes packed at the high end ←  grows up      │
//   └────────────────────────────────────────────────────────┘
//
// A tuple is identified by its SlotId. Slot IDs are *stable*: once a tuple
// is inserted, its slot id does not change for the lifetime of the slot,
// even across compaction (which moves tuple bytes around). remove() turns
// the slot into a tombstone (length = 0). A subsequent insert may reuse
// the tombstoned slot id; otherwise it appends a new one.
//
// SlottedPage stores opaque byte sequences; it knows nothing about column
// types. Higher layers decide what's inside a tuple.
class SlottedPage {
public:
    // Number of bytes the header occupies at the start of the page.
    static constexpr size_t HEADER_SIZE = 8;

    // Number of bytes per slot entry in the slot array.
    static constexpr size_t SLOT_SIZE = 4;

    // Largest tuple that could ever fit on a fresh empty page.
    static constexpr size_t MAX_TUPLE_SIZE = PAGE_SIZE - HEADER_SIZE - SLOT_SIZE;

    // Wrap an existing block of PAGE_SIZE bytes. The bytes are NOT touched
    // until you call init() (for a fresh page) or any mutating method.
    explicit SlottedPage(char* data) : data_(data) {}

    // Initialize the bytes as an empty slotted page. Call this exactly once
    // when the page is first allocated; do NOT call it on a page that
    // already holds data, or you will lose every tuple.
    void init();

    // Insert `tuple` (`len` bytes, must be > 0). Returns the slot id on
    // success, std::nullopt if the page does not have room. Will compact
    // automatically if there is enough total free space but it is fragmented.
    std::optional<SlotId> insert(const char* tuple, size_t len);

    // Read the tuple at `slot`. Returns {nullptr, 0} if the slot id is out
    // of range or has been tombstoned. The returned pointer aliases the
    // page bytes — do not retain it past the next mutation of this page.
    std::pair<const char*, size_t> get(SlotId slot) const;

    // Tombstone `slot`. Returns true if the slot was live, false if the
    // slot was already dead or out of range. The slot id remains reserved
    // (numSlots() does not decrease) but may be reused by a future insert.
    bool remove(SlotId slot);

    // Replace the tuple at `slot` with new bytes. Returns true on success,
    // false if `slot` is dead/out-of-range or the new tuple does not fit.
    // Slot id is preserved across the update even if the tuple bytes move.
    bool update(SlotId slot, const char* tuple, size_t len);

    // Total free bytes available for new tuple data after a compaction.
    // (Slot array growth is not counted; budget SLOT_SIZE separately if you
    // intend to append a new slot rather than reuse a tombstone.)
    size_t freeSpace() const;

    // Number of slot ids currently in use. Includes tombstones — a fresh
    // insert without a free tombstone would receive slot id `numSlots()`.
    size_t numSlots() const;

private:
    char* data_;

    // Header field accessors. The header lives at offset 0 with layout:
    //   [0..4) lsn (uint32_t, currently always 0; reserved for recovery)
    //   [4..6) num_slots (uint16_t)
    //   [6..8) free_space_offset (uint16_t) — where the next tuple goes
    uint16_t numSlotsRaw() const;
    void setNumSlots(uint16_t v);
    uint16_t freeSpaceOffset() const;
    void setFreeSpaceOffset(uint16_t v);

    struct SlotEntry { uint16_t offset; uint16_t length; };
    SlotEntry readSlot(SlotId i) const;
    void writeSlot(SlotId i, SlotEntry e);
    static bool isLive(SlotEntry s) { return s.length > 0; }

    size_t endOfSlotArray() const;
    size_t contiguousFree() const;
    size_t deadTupleBytes() const;

    // Walk live tuples and pack them at the high end of the page, updating
    // slot offsets. Slot ids are preserved.
    void compact();

    // First slot id whose entry is a tombstone, or std::nullopt if none.
    std::optional<SlotId> findDeadSlot() const;
};
