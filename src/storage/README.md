# Storage

Four-layer stack. Each layer exposes a small surface to the one above and
hides everything below.

```
┌──────────────────────────────────────────────────────────────────┐
│ heap_file.h    HeapFile    table abstraction; insert / get / scan │
├──────────────────────────────────────────────────────────────────┤
│ slotted_page.h SlottedPage byte layout inside one page            │
├──────────────────────────────────────────────────────────────────┤
│ buffer_pool.h  BufferPool  page cache with LRU eviction + pinning │
├──────────────────────────────────────────────────────────────────┤
│ disk_manager.h DiskManager read/write fixed-size pages on a file  │
└──────────────────────────────────────────────────────────────────┘
```

## What each layer owns

- **DiskManager** — one file, page id ↔ byte offset. Has no opinion on what's
  in a page.
- **BufferPool** — fixed array of `Frame`s caching pages from disk. Hands out
  `Frame*` via `pin()`/`pinNew()`, returning a `PageGuard` (RAII; calls
  `unpinPage` on scope exit). Evicts the LRU unpinned frame on a miss; writes
  dirty frames back when evicted or on `flushAll()`.
- **SlottedPage** — wraps a `char*` of `PAGE_SIZE` bytes. Header + slot array
  growing forward, tuple bytes packed at the high end. Slot ids are stable
  across compaction; `remove` tombstones; insert auto-compacts when needed.
  The 12-byte header includes a `next_page_id` field used by HeapFile to
  chain pages.
- **HeapFile** — unordered collection of tuples spread across a chain of
  slotted pages. Allocates and links new pages as the chain fills. RIDs
  (`{page_id, slot_id}`) are stable for the life of a tuple. Provides a
  forward iterator that yields every live tuple in (page, slot) order.

## Insert flow

```
HeapFile::insert(bytes, len)
    └─> walk page chain via SlottedPage::nextPageId()
        └─> BufferPool::pin(page) → Frame
            └─> SlottedPage::insert(bytes, len) → SlotId
        ↑ if no page has room: BufferPool::pinNew()
          and link via setNextPageId()
```

A miss inside `pin()` triggers `DiskManager::readPage()`; an eviction of a
dirty frame triggers `DiskManager::writePage()`.

## Read flow

```
HeapFile::get(rid)            HeapFile::Iterator::operator++
    BufferPool::pin(rid.page)     BufferPool::pin(cur_page)
    SlottedPage::get(rid.slot)    SlottedPage::get(cur_slot)
                                  → if exhausted: cur_page = nextPageId()
```

## Tests

Each layer has its own test file under `tests/storage/`. The end-to-end
persistence test lives in `tests/storage/test_integration.cpp` (loads 1000
rows, simulates a program restart by destroying every storage object, then
scans the rows back).
