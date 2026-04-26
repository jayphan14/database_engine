#pragma once

#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/slotted_page.h"

#include <cstddef>
#include <string>
#include <utility>

// Record identifier — locates a single tuple in a heap file.
// Slot ids are stable for the lifetime of the slot, so an RID handed back
// from insert remains valid until the tuple is removed.
struct RID {
    PageId page_id;
    SlotId slot_id;

    bool operator==(const RID& o) const {
        return page_id == o.page_id && slot_id == o.slot_id;
    }
    bool operator!=(const RID& o) const { return !(*this == o); }
    bool operator<(const RID& o) const {
        return page_id != o.page_id ? page_id < o.page_id : slot_id < o.slot_id;
    }
};

// Layer 4 of the storage stack: a heap file is an unordered collection of
// tuples spread across one or more slotted pages chained together via the
// `next_page_id` field of each page's header.
//
// Usage:
//
//   // First time:
//   HeapFile hf = HeapFile::create(&bp);
//   PageId root = hf.firstPageId();      // stash this somewhere durable
//   RID r = hf.insert(data, len);
//
//   // Later, possibly in another process, after reopening the file:
//   HeapFile hf2(&bp, root);
//   for (auto it = hf2.begin(); it != hf2.end(); ++it) {
//       const auto& [rid, bytes] = *it;
//       ...
//   }
//
// Thread-safety: not thread-safe. Concurrency belongs to a higher layer.
class HeapFile {
public:
    // Open an existing heap file. The page at `first_page_id` must already
    // have been initialized as a slotted page (typically by create()).
    HeapFile(BufferPool* bp, PageId first_page_id);

    // Create a brand new heap file by allocating a fresh first page from
    // `bp`'s disk manager. Persist the returned firstPageId() to find this
    // file again later.
    static HeapFile create(BufferPool* bp);

    // Insert a tuple. Allocates and links a new page if no existing page
    // has room. Throws if `len == 0` or `len > SlottedPage::MAX_TUPLE_SIZE`.
    RID insert(const char* tuple, size_t len);

    // Read a tuple by RID. Returns false (and leaves *out untouched) if the
    // slot is tombstoned. The bytes are copied into *out.
    bool get(RID rid, std::string* out);

    // Tombstone the tuple at `rid`. Returns false if it was already dead.
    bool remove(RID rid);

    PageId firstPageId() const { return first_page_; }

    // Forward iterator yielding every live tuple in the file in
    // (page, slot) order. The iterator does not hold a pin between calls,
    // so you can hold many of them and freely interleave with other
    // HeapFile operations.
    class Iterator {
    public:
        using Entry = std::pair<RID, std::string>;

        Iterator() = default;                       // end iterator
        Iterator(BufferPool* bp, PageId start);

        const Entry& operator*()  const { return entry_; }
        const Entry* operator->() const { return &entry_; }
        Iterator& operator++();
        bool operator==(const Iterator& o) const {
            return cur_page_ == o.cur_page_ && cur_slot_ == o.cur_slot_;
        }
        bool operator!=(const Iterator& o) const { return !(*this == o); }

    private:
        BufferPool* bp_ = nullptr;
        PageId cur_page_ = INVALID_PAGE_ID;
        SlotId cur_slot_ = 0;
        Entry entry_;

        // Walk forward from (cur_page_, cur_slot_) to the next live tuple,
        // crossing page boundaries as needed. Sets the state to end if no
        // live tuple remains.
        void advanceToLive();
    };

    Iterator begin() { return Iterator(bp_, first_page_); }
    Iterator end()   { return Iterator(); }

private:
    BufferPool* bp_;
    PageId first_page_;
};
