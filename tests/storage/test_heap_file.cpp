#include "tests/vendor/doctest.h"

#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/heap_file.h"
#include "src/storage/slotted_page.h"
#include "tests/test_util.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Collect every (rid, tuple) pair yielded by a heap file's iterator.
std::vector<HeapFile::Iterator::Entry> scanAll(HeapFile& hf) {
    std::vector<HeapFile::Iterator::Entry> out;
    for (auto it = hf.begin(); it != hf.end(); ++it) {
        out.push_back(*it);
    }
    return out;
}

// Insert helper that takes a string and returns the RID.
RID insertStr(HeapFile& hf, const std::string& s) {
    return hf.insert(s.data(), s.size());
}

}  // namespace

TEST_CASE("HeapFile::create + insert + get round trip (usage demo)") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);

    HeapFile hf = HeapFile::create(&bp);
    RID r = insertStr(hf, "hello world");

    std::string out;
    REQUIRE(hf.get(r, &out));
    CHECK(out == "hello world");

    // The freshly created file's first page is page 0.
    CHECK(hf.firstPageId() == 0);
    CHECK(r.page_id == 0);
}

TEST_CASE("iterator on an empty file yields nothing") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    CHECK(hf.begin() == hf.end());
    CHECK(scanAll(hf).empty());
}

TEST_CASE("iterator yields every inserted tuple, in (page, slot) order") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    const std::vector<std::string> tuples = {"alpha", "beta", "gamma", "delta", "epsilon"};
    std::vector<RID> rids;
    for (const auto& s : tuples) rids.push_back(insertStr(hf, s));

    auto entries = scanAll(hf);
    REQUIRE(entries.size() == tuples.size());

    // Iteration order matches the order of slot ids on each page; on a
    // single page that's the order tuples were inserted.
    for (size_t i = 0; i < tuples.size(); ++i) {
        CHECK(entries[i].first == rids[i]);
        CHECK(entries[i].second == tuples[i]);
    }
}

TEST_CASE("inserting beyond a single page allocates and links new pages") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    // Each tuple is roughly half a page, so two fit per page comfortably.
    // Inserting many of them must spill into multiple chained pages.
    std::vector<char> blob(2000, 'q');
    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        blob[0] = static_cast<char>('A' + i);  // distinguishes the tuples
        rids.push_back(hf.insert(blob.data(), blob.size()));
    }

    // The chain definitely grew beyond the first page.
    std::set<PageId> pages_used;
    for (auto r : rids) pages_used.insert(r.page_id);
    CHECK(pages_used.size() > 1);

    // Every tuple must still round trip.
    std::string out;
    for (size_t i = 0; i < rids.size(); ++i) {
        REQUIRE(hf.get(rids[i], &out));
        REQUIRE(out.size() == blob.size());
        CHECK(out[0] == static_cast<char>('A' + i));
    }
}

TEST_CASE("remove makes get return false and the iterator skip the slot") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    RID a = insertStr(hf, "a");
    RID b = insertStr(hf, "b");
    RID c = insertStr(hf, "c");

    REQUIRE(hf.remove(b));
    CHECK_FALSE(hf.remove(b));   // double-remove

    std::string out;
    CHECK(hf.get(a, &out));
    CHECK(out == "a");
    CHECK_FALSE(hf.get(b, &out));
    CHECK(hf.get(c, &out));
    CHECK(out == "c");

    auto entries = scanAll(hf);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].first == a);
    CHECK(entries[0].second == "a");
    CHECK(entries[1].first == c);
    CHECK(entries[1].second == "c");
}

TEST_CASE("iterator advances across an entirely-tombstoned page") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    // Fill the first page with chunky tuples so the next insert spills
    // onto a fresh page.
    std::vector<char> filler(1500, 'x');
    std::vector<RID> first_page_rids;
    for (int i = 0; i < 3; ++i) {
        first_page_rids.push_back(hf.insert(filler.data(), filler.size()));
    }

    // Insert a small tuple. Either it fits on the first page or rolls onto
    // a new page; either way, removing every tuple on the first page leaves
    // the iterator with a fully-tombstoned page to scan past.
    RID after = insertStr(hf, "afterwards");

    for (RID r : first_page_rids) REQUIRE(hf.remove(r));

    auto entries = scanAll(hf);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].first == after);
    CHECK(entries[0].second == "afterwards");
}

TEST_CASE("RID comparisons behave the way tests expect") {
    RID a{0, 0}, b{0, 1}, c{1, 0};
    CHECK(a == a);
    CHECK(a != b);
    CHECK(a < b);
    CHECK(b < c);
    CHECK_FALSE(c < a);
}

TEST_CASE("insert rejects len == 0 and len > MAX_TUPLE_SIZE") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    CHECK_THROWS_AS(hf.insert(nullptr, 0), std::runtime_error);

    std::vector<char> oversize(SlottedPage::MAX_TUPLE_SIZE + 1, 'x');
    CHECK_THROWS_AS(hf.insert(oversize.data(), oversize.size()), std::runtime_error);
}

TEST_CASE("data persists across BufferPool and DiskManager lifetimes") {
    TempFile tf;

    PageId root_pid;
    std::map<RID, std::string> model;
    {
        DiskManager dm(tf.path());
        BufferPool bp(4, &dm);
        HeapFile hf = HeapFile::create(&bp);
        root_pid = hf.firstPageId();

        // Sized to span a few pages.
        std::vector<char> blob(1500, '.');
        for (int i = 0; i < 8; ++i) {
            blob[0] = static_cast<char>('A' + i);
            RID r = hf.insert(blob.data(), blob.size());
            model[r] = std::string(blob.begin(), blob.end());
        }

        bp.flushAll();
    }

    // Reopen and verify everything is recoverable from `root_pid` alone.
    DiskManager dm2(tf.path());
    BufferPool bp(4, &dm2);
    HeapFile hf(&bp, root_pid);

    auto entries = scanAll(hf);
    REQUIRE(entries.size() == model.size());

    std::map<RID, std::string> got;
    for (const auto& [rid, bytes] : entries) got[rid] = bytes;
    CHECK(got == model);
}

TEST_CASE("free-space reuse: removing a tuple lets a same-sized one fit on the same page") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    // Fill the first page.
    std::vector<RID> rids;
    std::vector<char> blob(1000, 'X');
    for (int i = 0; i < 4; ++i) rids.push_back(hf.insert(blob.data(), blob.size()));

    // The next insert at this size would normally spill — remove one and
    // verify the freed space is reused on the same page.
    REQUIRE(hf.remove(rids[1]));

    RID r = hf.insert(blob.data(), blob.size());
    CHECK(r.page_id == rids[1].page_id);  // same page
    // The lowest tombstoned slot should be reused.
    CHECK(r.slot_id == rids[1].slot_id);
}

TEST_CASE("range-based for iteration works (STL conformance)") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf = HeapFile::create(&bp);

    insertStr(hf, "one");
    insertStr(hf, "two");
    insertStr(hf, "three");

    std::vector<std::string> seen;
    for (auto& entry : hf) {
        seen.push_back(entry.second);
    }
    REQUIRE(seen.size() == 3);
    CHECK(std::count(seen.begin(), seen.end(), std::string("one"))   == 1);
    CHECK(std::count(seen.begin(), seen.end(), std::string("two"))   == 1);
    CHECK(std::count(seen.begin(), seen.end(), std::string("three")) == 1);
}

TEST_CASE("stress: many tuples across many pages with eviction-heavy buffer pool") {
    TempFile tf;
    DiskManager dm(tf.path());
    // Tiny pool relative to working set forces lots of eviction during the
    // workload; the heap file API must keep things consistent throughout.
    BufferPool bp(2, &dm);
    HeapFile hf = HeapFile::create(&bp);

    constexpr int N = 600;
    std::mt19937 rng(0xCAFEBABEu);
    std::uniform_int_distribution<int> len_dist(1, 80);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    std::map<RID, std::string> model;
    for (int i = 0; i < N; ++i) {
        std::string data(len_dist(rng), '\0');
        for (auto& c : data) c = static_cast<char>(byte_dist(rng));
        RID r = hf.insert(data.data(), data.size());
        REQUIRE(model.emplace(r, std::move(data)).second);  // RIDs unique
    }

    // Tombstone roughly a third of them and verify get / iterator agree.
    std::vector<RID> all_rids;
    for (const auto& [rid, _] : model) all_rids.push_back(rid);
    std::shuffle(all_rids.begin(), all_rids.end(), rng);
    for (size_t i = 0; i < all_rids.size() / 3; ++i) {
        REQUIRE(hf.remove(all_rids[i]));
        model.erase(all_rids[i]);
    }

    // get() agrees with the model on every surviving RID.
    std::string out;
    for (const auto& [rid, expected] : model) {
        REQUIRE(hf.get(rid, &out));
        REQUIRE(out == expected);
    }
    // get() refuses every removed RID.
    for (size_t i = 0; i < all_rids.size() / 3; ++i) {
        CHECK_FALSE(hf.get(all_rids[i], &out));
    }

    // Iterator yields exactly the surviving tuples.
    std::map<RID, std::string> seen;
    for (const auto& [rid, bytes] : hf) seen[rid] = bytes;
    CHECK(seen == model);
}
