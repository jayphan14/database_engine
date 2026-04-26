#include "tests/vendor/doctest.h"

#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/heap_file.h"
#include "tests/test_util.h"

#include <map>
#include <random>
#include <string>
#include <utility>

// End-to-end persistence test: load 1000 rows into a heap file, simulate a
// program restart by destroying every storage object and reopening on the
// same file, then scan the rows back. Every row must round-trip exactly,
// and the iterator must yield the same set as the in-memory model.
TEST_CASE("integration: load 1000 rows, restart, scan them back") {
    TempFile tf;
    constexpr int N = 1000;

    PageId root_pid;
    std::map<RID, std::string> expected;

    // ----- Phase 1: load 1000 rows and shut down -----
    {
        DiskManager dm(tf.path());
        // Small pool relative to the working set forces lots of eviction
        // during the load, exercising the dirty-write-back path.
        BufferPool bp(4, &dm);
        HeapFile hf = HeapFile::create(&bp);
        root_pid = hf.firstPageId();

        std::mt19937 rng(0xDEC1DEDu);
        std::uniform_int_distribution<int> len_dist(1, 200);
        std::uniform_int_distribution<int> byte_dist(0, 255);

        for (int i = 0; i < N; ++i) {
            std::string row(len_dist(rng), '\0');
            for (auto& c : row) c = static_cast<char>(byte_dist(rng));
            RID r = hf.insert(row.data(), row.size());
            REQUIRE(expected.emplace(r, std::move(row)).second);  // RIDs unique
        }
        REQUIRE(expected.size() == static_cast<size_t>(N));

        // Flush every dirty frame before shutdown — without this the writes
        // sitting in evictable-but-clean-on-disk frames would be lost.
        bp.flushAll();
    }
    // BufferPool and DiskManager destructors run here, closing the file.
    // Anything still in memory is gone — we have nothing but `tf.path()` and
    // `root_pid` to find the data again.

    // ----- Phase 2: cold reopen and verify -----
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    HeapFile hf(&bp, root_pid);

    // Iterator yields exactly the rows we inserted, with matching RIDs.
    std::map<RID, std::string> seen;
    for (const auto& [rid, bytes] : hf) {
        REQUIRE(seen.emplace(rid, bytes).second);
    }
    REQUIRE(seen.size() == expected.size());
    CHECK(seen == expected);

    // Random-access get() resolves every original RID to the exact bytes.
    std::string out;
    for (const auto& [rid, bytes] : expected) {
        REQUIRE(hf.get(rid, &out));
        REQUIRE(out == bytes);
    }
}
