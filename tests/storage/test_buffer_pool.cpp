#include "tests/vendor/doctest.h"

#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/slotted_page.h"
#include "tests/test_util.h"

#include <cstring>
#include <map>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

// Seed `n` pages on disk via DiskManager directly. Page i gets makePattern(i).
// Returns the DiskManager so the caller can hand it to a BufferPool.
void seedPages(DiskManager& dm, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const PageId pid = dm.allocatePage();
        const auto pattern = makePattern(static_cast<uint8_t>(0x10 + i));
        dm.writePage(pid, pattern.data());
    }
}

}  // namespace

TEST_CASE("fetchPage loads page bytes from disk") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 1);

    BufferPool bp(3, &dm);
    Frame* f = bp.fetchPage(0);
    REQUIRE(f != nullptr);
    CHECK(f->page_id == 0);
    CHECK(f->pin_count == 1);
    CHECK_FALSE(f->is_dirty);

    const auto expected = makePattern(0x10);
    CHECK(std::memcmp(f->data, expected.data(), PAGE_SIZE) == 0);

    bp.unpinPage(0, false);
}

TEST_CASE("cache hit returns the same frame and bumps pin count") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 1);

    BufferPool bp(3, &dm);
    Frame* f1 = bp.fetchPage(0);
    Frame* f2 = bp.fetchPage(0);
    CHECK(f1 == f2);
    CHECK(f1->pin_count == 2);

    bp.unpinPage(0, false);
    CHECK(f1->pin_count == 1);
    bp.unpinPage(0, false);
    CHECK(f1->pin_count == 0);
}

TEST_CASE("LRU eviction reclaims the least recently used unpinned frame") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 4);

    BufferPool bp(3, &dm);

    // Fetch + immediately unpin pages 0,1,2 in order. After this, pool holds
    // {0,1,2} all unpinned with LRU order [0, 1, 2] (front = 0 = next victim).
    bp.fetchPage(0); bp.unpinPage(0, false);
    bp.fetchPage(1); bp.unpinPage(1, false);
    bp.fetchPage(2); bp.unpinPage(2, false);

    // Fetch page 3 — must evict page 0.
    Frame* f3 = bp.fetchPage(3);
    const auto expected3 = makePattern(0x13);
    CHECK(std::memcmp(f3->data, expected3.data(), PAGE_SIZE) == 0);
    bp.unpinPage(3, false);

    // Re-fetching page 0 should still work (re-read from disk).
    Frame* f0 = bp.fetchPage(0);
    const auto expected0 = makePattern(0x10);
    CHECK(std::memcmp(f0->data, expected0.data(), PAGE_SIZE) == 0);
    bp.unpinPage(0, false);
}

TEST_CASE("dirty page is written back when evicted") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 4);

    {
        BufferPool bp(3, &dm);

        // Modify page 0 through the pool.
        Frame* f0 = bp.fetchPage(0);
        const auto modified = makePattern(0xEE);
        std::memcpy(f0->data, modified.data(), PAGE_SIZE);
        bp.unpinPage(0, /*was_modified=*/true);

        // Force eviction of page 0 by filling the pool with other pages.
        bp.fetchPage(1); bp.unpinPage(1, false);
        bp.fetchPage(2); bp.unpinPage(2, false);
        bp.fetchPage(3); bp.unpinPage(3, false);  // evicts page 0
    }

    // After the BufferPool destructs (no flushAll called explicitly), the
    // eviction itself should have written page 0 back to disk.
    DiskManager dm2(tf.path());
    std::vector<char> buf(PAGE_SIZE);
    dm2.readPage(0, buf.data());
    const auto expected = makePattern(0xEE);
    CHECK(std::memcmp(buf.data(), expected.data(), PAGE_SIZE) == 0);
}

TEST_CASE("clean unpin discards in-memory edits on eviction") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 4);

    {
        BufferPool bp(3, &dm);
        Frame* f0 = bp.fetchPage(0);
        std::memcpy(f0->data, makePattern(0xEE).data(), PAGE_SIZE);
        bp.unpinPage(0, /*was_modified=*/false);  // discard the edit

        // Evict page 0 by filling pool.
        bp.fetchPage(1); bp.unpinPage(1, false);
        bp.fetchPage(2); bp.unpinPage(2, false);
        bp.fetchPage(3); bp.unpinPage(3, false);
    }

    DiskManager dm2(tf.path());
    std::vector<char> buf(PAGE_SIZE);
    dm2.readPage(0, buf.data());
    const auto original = makePattern(0x10);
    CHECK(std::memcmp(buf.data(), original.data(), PAGE_SIZE) == 0);
}

TEST_CASE("fetchPage throws when every frame is pinned") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 3);

    BufferPool bp(2, &dm);
    bp.fetchPage(0);
    bp.fetchPage(1);
    CHECK_THROWS_AS(bp.fetchPage(2), std::runtime_error);

    // Cleanup so the implicit destructor doesn't try to flush a still-pinned
    // page (it doesn't, but be tidy regardless).
    bp.unpinPage(0, false);
    bp.unpinPage(1, false);
}

TEST_CASE("a doubly-pinned page survives eviction pressure until fully unpinned") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 3);

    BufferPool bp(2, &dm);
    bp.fetchPage(0);  // pin_count = 1
    bp.fetchPage(0);  // pin_count = 2

    // Pool has 2 frames; page 0 occupies one and is doubly pinned. Fetch
    // page 1 — fits in the other frame.
    bp.fetchPage(1);

    // Now both frames are pinned; fetching page 2 must throw.
    CHECK_THROWS_AS(bp.fetchPage(2), std::runtime_error);

    // Unpin page 0 once — still pinned (count = 1).
    bp.unpinPage(0, false);
    CHECK_THROWS_AS(bp.fetchPage(2), std::runtime_error);

    // Unpin again — now evictable.
    bp.unpinPage(0, false);
    Frame* f2 = bp.fetchPage(2);
    REQUIRE(f2 != nullptr);
    CHECK(f2->page_id == 2);

    bp.unpinPage(1, false);
    bp.unpinPage(2, false);
}

TEST_CASE("newPage allocates a fresh zeroed page") {
    TempFile tf;
    DiskManager dm(tf.path());

    BufferPool bp(3, &dm);
    PageId new_pid = INVALID_PAGE_ID;
    Frame* f = bp.newPage(&new_pid);
    REQUIRE(f != nullptr);
    CHECK(new_pid == 0);
    CHECK(f->page_id == 0);
    CHECK(f->pin_count == 1);
    CHECK_FALSE(f->is_dirty);

    std::vector<char> zeros(PAGE_SIZE, 0);
    CHECK(std::memcmp(f->data, zeros.data(), PAGE_SIZE) == 0);

    bp.unpinPage(new_pid, false);
}

TEST_CASE("flushPage writes a dirty page through to disk") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 1);

    BufferPool bp(3, &dm);
    Frame* f = bp.fetchPage(0);
    std::memcpy(f->data, makePattern(0x99).data(), PAGE_SIZE);
    bp.unpinPage(0, /*was_modified=*/true);

    bp.flushPage(0);

    // Read directly via DiskManager (no shared state with bp's cache).
    std::vector<char> buf(PAGE_SIZE);
    dm.readPage(0, buf.data());
    CHECK(std::memcmp(buf.data(), makePattern(0x99).data(), PAGE_SIZE) == 0);
}

TEST_CASE("flushAll writes every dirty page") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 2);

    BufferPool bp(3, &dm);
    Frame* f0 = bp.fetchPage(0);
    std::memcpy(f0->data, makePattern(0xA1).data(), PAGE_SIZE);
    bp.unpinPage(0, true);

    Frame* f1 = bp.fetchPage(1);
    std::memcpy(f1->data, makePattern(0xA2).data(), PAGE_SIZE);
    bp.unpinPage(1, true);

    bp.flushAll();

    std::vector<char> buf(PAGE_SIZE);
    dm.readPage(0, buf.data());
    CHECK(std::memcmp(buf.data(), makePattern(0xA1).data(), PAGE_SIZE) == 0);
    dm.readPage(1, buf.data());
    CHECK(std::memcmp(buf.data(), makePattern(0xA2).data(), PAGE_SIZE) == 0);
}

TEST_CASE("unpinPage on a non-cached page throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 1);

    BufferPool bp(3, &dm);
    CHECK_THROWS_AS(bp.unpinPage(0, false), std::runtime_error);
}

TEST_CASE("unpinPage on an already fully unpinned page throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 1);

    BufferPool bp(3, &dm);
    bp.fetchPage(0);
    bp.unpinPage(0, false);
    CHECK_THROWS_AS(bp.unpinPage(0, false), std::runtime_error);
}

TEST_CASE("PageGuard pins on construction and unpins on scope exit (usage demo)") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 4);

    BufferPool bp(2, &dm);

    {
        PageGuard g = bp.pin(0);
        CHECK(g.valid());
        CHECK(g->page_id == 0);
        CHECK(g->pin_count == 1);

        // Read access — no markDirty needed.
        const auto expected = makePattern(0x10);
        CHECK(std::memcmp(g->data, expected.data(), PAGE_SIZE) == 0);
    }
    // Guard dropped: page 0 is now unpinned and evictable.

    // Sanity check by exhausting eviction. With 2 frames, fetching 1+2+3 in a
    // row would have failed if page 0 were still pinned.
    bp.fetchPage(1); bp.unpinPage(1, false);
    bp.fetchPage(2); bp.unpinPage(2, false);
    bp.fetchPage(3); bp.unpinPage(3, false);
}

TEST_CASE("PageGuard::markDirty causes write-back on eviction") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 4);

    {
        BufferPool bp(3, &dm);
        {
            PageGuard g = bp.pin(0);
            std::memcpy(g->data, makePattern(0xC1).data(), PAGE_SIZE);
            g.markDirty();
        }
        // Force eviction of page 0.
        bp.fetchPage(1); bp.unpinPage(1, false);
        bp.fetchPage(2); bp.unpinPage(2, false);
        bp.fetchPage(3); bp.unpinPage(3, false);
    }

    DiskManager dm2(tf.path());
    std::vector<char> buf(PAGE_SIZE);
    dm2.readPage(0, buf.data());
    CHECK(std::memcmp(buf.data(), makePattern(0xC1).data(), PAGE_SIZE) == 0);
}

TEST_CASE("PageGuard without markDirty discards in-memory edits") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 4);

    {
        BufferPool bp(3, &dm);
        {
            PageGuard g = bp.pin(0);
            std::memcpy(g->data, makePattern(0xC1).data(), PAGE_SIZE);
            // Note: forgot to call g.markDirty() — edits will be lost.
        }
        bp.fetchPage(1); bp.unpinPage(1, false);
        bp.fetchPage(2); bp.unpinPage(2, false);
        bp.fetchPage(3); bp.unpinPage(3, false);
    }

    DiskManager dm2(tf.path());
    std::vector<char> buf(PAGE_SIZE);
    dm2.readPage(0, buf.data());
    CHECK(std::memcmp(buf.data(), makePattern(0x10).data(), PAGE_SIZE) == 0);
}

TEST_CASE("PageGuard is move-only and the moved-from guard is empty") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 1);

    BufferPool bp(2, &dm);

    PageGuard a = bp.pin(0);
    CHECK(a.valid());

    PageGuard b = std::move(a);
    CHECK_FALSE(a.valid());
    CHECK(b.valid());
    CHECK(b->page_id == 0);
    CHECK(b->pin_count == 1);
    // When `b` goes out of scope, page 0 unpins exactly once.
}

TEST_CASE("pinNew returns a guard for a freshly allocated zeroed page") {
    TempFile tf;
    DiskManager dm(tf.path());

    BufferPool bp(3, &dm);

    PageId pid;
    {
        PageGuard g = bp.pinNew();
        REQUIRE(g.valid());
        pid = g->page_id;
        CHECK(pid == 0);
        CHECK(g->pin_count == 1);

        std::vector<char> zeros(PAGE_SIZE, 0);
        CHECK(std::memcmp(g->data, zeros.data(), PAGE_SIZE) == 0);

        std::memcpy(g->data, makePattern(0x77).data(), PAGE_SIZE);
        g.markDirty();
    }

    bp.flushPage(pid);
    std::vector<char> buf(PAGE_SIZE);
    dm.readPage(pid, buf.data());
    CHECK(std::memcmp(buf.data(), makePattern(0x77).data(), PAGE_SIZE) == 0);
}

TEST_CASE("constructor rejects num_frames == 0") {
    TempFile tf;
    DiskManager dm(tf.path());
    CHECK_THROWS_AS(BufferPool(0, &dm), std::runtime_error);
}

TEST_CASE("constructor rejects null DiskManager") {
    CHECK_THROWS_AS(BufferPool(4, nullptr), std::runtime_error);
}

TEST_CASE("a 1-frame pool still functions") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 3);

    BufferPool bp(1, &dm);
    Frame* f0 = bp.fetchPage(0);
    CHECK(std::memcmp(f0->data, makePattern(0x10).data(), PAGE_SIZE) == 0);
    bp.unpinPage(0, false);

    // Each subsequent fetch evicts the only frame.
    Frame* f1 = bp.fetchPage(1);
    CHECK(std::memcmp(f1->data, makePattern(0x11).data(), PAGE_SIZE) == 0);
    bp.unpinPage(1, false);

    Frame* f2 = bp.fetchPage(2);
    CHECK(std::memcmp(f2->data, makePattern(0x12).data(), PAGE_SIZE) == 0);
    bp.unpinPage(2, false);
}

TEST_CASE("eviction order tracks the LRU recency of unpins") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 4);

    BufferPool bp(3, &dm);

    // Touch order: 0, 1, 2 (LRU = 0 after this round of unpins).
    bp.fetchPage(0); bp.unpinPage(0, false);
    bp.fetchPage(1); bp.unpinPage(1, false);
    bp.fetchPage(2); bp.unpinPage(2, false);

    // Re-touch 0 — now LRU should be 1, not 0.
    bp.fetchPage(0); bp.unpinPage(0, false);

    // Fetching 3 should evict 1 (the LRU). 0 and 2 must still be cache hits.
    bp.fetchPage(3); bp.unpinPage(3, false);

    Frame* f0 = bp.fetchPage(0);
    CHECK(std::memcmp(f0->data, makePattern(0x10).data(), PAGE_SIZE) == 0);
    bp.unpinPage(0, false);

    Frame* f2 = bp.fetchPage(2);
    CHECK(std::memcmp(f2->data, makePattern(0x12).data(), PAGE_SIZE) == 0);
    bp.unpinPage(2, false);
}

TEST_CASE("newPage evicts an unpinned frame when the pool is full") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 2);

    BufferPool bp(2, &dm);
    bp.fetchPage(0); bp.unpinPage(0, false);
    bp.fetchPage(1); bp.unpinPage(1, false);

    PageId new_pid = INVALID_PAGE_ID;
    Frame* f = bp.newPage(&new_pid);
    REQUIRE(f != nullptr);
    CHECK(new_pid == 2);
    CHECK(f->page_id == 2);
    CHECK(f->pin_count == 1);
    bp.unpinPage(new_pid, false);
}

TEST_CASE("newPage throws when every frame is pinned") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 2);

    BufferPool bp(2, &dm);
    bp.fetchPage(0);
    bp.fetchPage(1);

    PageId out;
    CHECK_THROWS_AS(bp.newPage(&out), std::runtime_error);

    bp.unpinPage(0, false);
    bp.unpinPage(1, false);
}

TEST_CASE("flushPage on a page not in the pool is a silent no-op") {
    TempFile tf;
    DiskManager dm(tf.path());
    seedPages(dm, 1);

    BufferPool bp(2, &dm);
    // Not cached at all — must not throw.
    bp.flushPage(0);
    bp.flushPage(99);
}

TEST_CASE("integration: many tuples across many pages with heavy eviction") {
    TempFile tf;
    DiskManager dm(tf.path());

    constexpr int kPages   = 6;
    constexpr int kPerPage = 8;
    std::map<std::pair<PageId, SlotId>, std::vector<char>> model;
    std::vector<PageId> page_ids;

    {
        // Tiny pool relative to working set forces lots of eviction traffic.
        BufferPool bp(2, &dm);
        std::mt19937 rng(1234);
        std::uniform_int_distribution<int> len_dist(1, 80);
        std::uniform_int_distribution<int> byte_dist(0, 255);

        for (int p = 0; p < kPages; ++p) {
            PageGuard g = bp.pinNew();
            page_ids.push_back(g->page_id);

            SlottedPage sp(g->data);
            sp.init();

            for (int t = 0; t < kPerPage; ++t) {
                std::vector<char> data(len_dist(rng));
                for (auto& c : data) c = static_cast<char>(byte_dist(rng));
                auto sid = sp.insert(data.data(), data.size());
                REQUIRE(sid);
                model[{g->page_id, *sid}] = std::move(data);
            }
            g.markDirty();
        }
        bp.flushAll();
    }

    // Reopen everything fresh and verify each tuple is exactly recoverable.
    DiskManager dm2(tf.path());
    BufferPool bp(2, &dm2);
    for (const auto& [key, expected] : model) {
        PageGuard g = bp.pin(key.first);
        SlottedPage sp(g->data);
        auto [p, len] = sp.get(key.second);
        REQUIRE(p != nullptr);
        REQUIRE(len == expected.size());
        REQUIRE(std::memcmp(p, expected.data(), len) == 0);
    }
}
