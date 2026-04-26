#include "tests/vendor/doctest.h"

#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/slotted_page.h"
#include "tests/test_util.h"

#include <array>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

// Build a tuple buffer from a string for ergonomic test code.
std::vector<char> tup(const std::string& s) {
    return std::vector<char>(s.begin(), s.end());
}

// Comparison helper: returns true if SlottedPage::get(slot) yields exactly
// the bytes of `expected`.
bool slotEquals(const SlottedPage& sp, SlotId slot, const std::string& expected) {
    auto [p, len] = sp.get(slot);
    if (p == nullptr) return false;
    if (len != expected.size()) return false;
    return std::memcmp(p, expected.data(), len) == 0;
}

}  // namespace

TEST_CASE("init produces an empty page") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    CHECK(sp.numSlots() == 0);
    CHECK(sp.freeSpace() == PAGE_SIZE - SlottedPage::HEADER_SIZE);
}

TEST_CASE("insert / get round trip a single tuple") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    const auto t = tup("hello");
    auto sid = sp.insert(t.data(), t.size());
    REQUIRE(sid.has_value());
    CHECK(*sid == 0);
    CHECK(sp.numSlots() == 1);
    CHECK(slotEquals(sp, *sid, "hello"));
}

TEST_CASE("multiple inserts get distinct sequential slot ids and round trip") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto a = tup("alpha");
    auto b = tup("beta-much-longer-than-alpha");
    auto c = tup("gamma");

    auto sa = sp.insert(a.data(), a.size()); REQUIRE(sa);
    auto sb = sp.insert(b.data(), b.size()); REQUIRE(sb);
    auto sc = sp.insert(c.data(), c.size()); REQUIRE(sc);
    CHECK(*sa == 0);
    CHECK(*sb == 1);
    CHECK(*sc == 2);
    CHECK(sp.numSlots() == 3);

    CHECK(slotEquals(sp, *sa, "alpha"));
    CHECK(slotEquals(sp, *sb, "beta-much-longer-than-alpha"));
    CHECK(slotEquals(sp, *sc, "gamma"));
}

TEST_CASE("freeSpace decreases by len + SLOT_SIZE on each fresh insert") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    const size_t free0 = sp.freeSpace();
    auto t = tup("twenty-byte-tuple..");  // 19 bytes
    REQUIRE(sp.insert(t.data(), t.size()).has_value());
    const size_t free1 = sp.freeSpace();
    CHECK(free0 - free1 == t.size() + SlottedPage::SLOT_SIZE);
}

TEST_CASE("remove returns false for out-of-range or already-dead slots") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto t = tup("x");
    auto sid = sp.insert(t.data(), t.size());
    REQUIRE(sid);

    CHECK(sp.remove(*sid));
    CHECK_FALSE(sp.remove(*sid));        // already dead
    CHECK_FALSE(sp.remove(99));          // out of range
}

TEST_CASE("get on a tombstoned or out-of-range slot returns {nullptr, 0}") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto t = tup("doomed");
    auto sid = sp.insert(t.data(), t.size());
    REQUIRE(sid);
    REQUIRE(sp.remove(*sid));

    auto [p1, l1] = sp.get(*sid);
    CHECK(p1 == nullptr);
    CHECK(l1 == 0);

    auto [p2, l2] = sp.get(99);
    CHECK(p2 == nullptr);
    CHECK(l2 == 0);
}

TEST_CASE("insert reuses the first tombstoned slot id") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto a = tup("a");
    auto b = tup("b");
    auto c = tup("c");
    auto sa = sp.insert(a.data(), a.size()); REQUIRE(sa);
    auto sb = sp.insert(b.data(), b.size()); REQUIRE(sb);
    auto sc = sp.insert(c.data(), c.size()); REQUIRE(sc);

    REQUIRE(sp.remove(*sb));
    CHECK(sp.numSlots() == 3);  // tombstone keeps slot id reserved

    auto d = tup("d");
    auto sd = sp.insert(d.data(), d.size());
    REQUIRE(sd);
    CHECK(*sd == *sb);          // slot id reused
    CHECK(sp.numSlots() == 3);  // no new slot appended

    CHECK(slotEquals(sp, *sa, "a"));
    CHECK(slotEquals(sp, *sd, "d"));
    CHECK(slotEquals(sp, *sc, "c"));
}

TEST_CASE("update with shorter or equal length is in-place; slot id stable") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto t = tup("abcdefghij");          // 10 bytes
    auto sid = sp.insert(t.data(), t.size());
    REQUIRE(sid);

    const auto same = tup("0123456789"); // same length
    CHECK(sp.update(*sid, same.data(), same.size()));
    CHECK(slotEquals(sp, *sid, "0123456789"));

    const auto shorter = tup("xy");      // shorter
    CHECK(sp.update(*sid, shorter.data(), shorter.size()));
    CHECK(slotEquals(sp, *sid, "xy"));
}

TEST_CASE("update with a longer tuple relocates but preserves slot id") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto t = tup("short");
    auto sid = sp.insert(t.data(), t.size());
    REQUIRE(sid);

    auto longer = tup("a-much-longer-replacement-tuple");
    CHECK(sp.update(*sid, longer.data(), longer.size()));
    CHECK(*sid == 0);
    CHECK(slotEquals(sp, *sid, "a-much-longer-replacement-tuple"));
}

TEST_CASE("update returns false for out-of-range, tombstoned, or oversized") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto t = tup("hello");
    auto sid = sp.insert(t.data(), t.size());
    REQUIRE(sid);

    auto repl = tup("world");
    CHECK_FALSE(sp.update(99, repl.data(), repl.size()));      // out of range
    REQUIRE(sp.remove(*sid));
    CHECK_FALSE(sp.update(*sid, repl.data(), repl.size()));    // tombstoned

    // Re-add and try a too-large tuple.
    auto sid2 = sp.insert(t.data(), t.size()); REQUIRE(sid2);
    std::vector<char> huge(SlottedPage::MAX_TUPLE_SIZE + 1, 'x');
    CHECK_FALSE(sp.update(*sid2, huge.data(), huge.size()));
}

TEST_CASE("insert returns nullopt when the page cannot fit even one more byte") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    // Fill the page with a single near-maximum tuple, then try to insert again.
    std::vector<char> big(SlottedPage::MAX_TUPLE_SIZE, 'A');
    auto sid = sp.insert(big.data(), big.size());
    REQUIRE(sid);

    auto small = tup("nope");
    CHECK_FALSE(sp.insert(small.data(), small.size()).has_value());

    // Length 0 and oversized are also rejected.
    CHECK_FALSE(sp.insert(nullptr, 0).has_value());
    std::vector<char> oversize(SlottedPage::MAX_TUPLE_SIZE + 1, 'x');
    CHECK_FALSE(sp.insert(oversize.data(), oversize.size()).has_value());
}

TEST_CASE("compaction is triggered when free space is fragmented") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    // Fill the page with several large tuples, then delete the middle ones
    // so the surviving tuples leave a gap that requires compaction to use.
    const size_t per = 800;  // 5 * 800 = 4000 bytes; leaves headroom
    std::vector<char> a(per, 'A'); auto sa = sp.insert(a.data(), per); REQUIRE(sa);
    std::vector<char> b(per, 'B'); auto sb = sp.insert(b.data(), per); REQUIRE(sb);
    std::vector<char> c(per, 'C'); auto sc = sp.insert(c.data(), per); REQUIRE(sc);
    std::vector<char> d(per, 'D'); auto sd = sp.insert(d.data(), per); REQUIRE(sd);
    std::vector<char> e(per, 'E'); auto se = sp.insert(e.data(), per); REQUIRE(se);

    // Tombstone two interior tuples — leaves dead bytes scattered.
    REQUIRE(sp.remove(*sb));
    REQUIRE(sp.remove(*sd));

    // After tombstoning, contiguous free is small but total free is large.
    // A new tuple of size ~1500 should still fit because compaction kicks in.
    std::vector<char> big(1500, 'X');
    auto sx = sp.insert(big.data(), big.size());
    REQUIRE(sx);

    // Survivors and the new insert are all readable.
    {
        auto [p, n] = sp.get(*sa);
        REQUIRE(n == per);
        CHECK(std::memcmp(p, a.data(), per) == 0);
    }
    {
        auto [p, n] = sp.get(*sc);
        REQUIRE(n == per);
        CHECK(std::memcmp(p, c.data(), per) == 0);
    }
    {
        auto [p, n] = sp.get(*se);
        REQUIRE(n == per);
        CHECK(std::memcmp(p, e.data(), per) == 0);
    }
    {
        auto [p, n] = sp.get(*sx);
        REQUIRE(n == big.size());
        CHECK(std::memcmp(p, big.data(), big.size()) == 0);
    }
}

TEST_CASE("slot ids are stable across compaction") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    const std::vector<char> a(100, 'A');
    const std::vector<char> b(100, 'B');
    const std::vector<char> c(100, 'C');

    auto sa = sp.insert(a.data(), a.size()); REQUIRE(sa); CHECK(*sa == 0);
    auto sb = sp.insert(b.data(), b.size()); REQUIRE(sb); CHECK(*sb == 1);
    auto sc = sp.insert(c.data(), c.size()); REQUIRE(sc); CHECK(*sc == 2);

    REQUIRE(sp.remove(*sa));

    // contigFree at this point = PAGE_SIZE - HEADER - 3*SLOT - 300.
    // a contributed 100 dead bytes that compaction can reclaim. Pick a tuple
    // size that exceeds contigFree but fits after a's bytes are recovered —
    // compaction is then required to satisfy the insert.
    const size_t contig = PAGE_SIZE
                          - SlottedPage::HEADER_SIZE
                          - 3 * SlottedPage::SLOT_SIZE
                          - 300;
    std::vector<char> filler(contig + 50, 'F');
    auto sf = sp.insert(filler.data(), filler.size());
    REQUIRE(sf);
    CHECK(*sf == 0);  // reused a's tombstoned slot id

    // Slot ids 1 and 2 (b and c) must still resolve to the original bytes
    // even though compaction relocated them.
    CHECK(slotEquals(sp, *sb, std::string(b.begin(), b.end())));
    CHECK(slotEquals(sp, *sc, std::string(c.begin(), c.end())));
}

TEST_CASE("SlottedPage round-trips through BufferPool and DiskManager") {
    TempFile tf;
    DiskManager dm(tf.path());

    // Insert a few tuples, modify, evict the page, then reload via a fresh
    // BufferPool and a fresh DiskManager and verify everything is readable.
    PageId pid;
    {
        BufferPool bp(2, &dm);
        PageGuard g = bp.pinNew();
        pid = g->page_id;

        SlottedPage sp(g->data);
        sp.init();
        auto a = tup("hello");
        auto b = tup("world");
        REQUIRE(sp.insert(a.data(), a.size()));
        REQUIRE(sp.insert(b.data(), b.size()));
        g.markDirty();

        bp.flushAll();
    }

    {
        DiskManager dm2(tf.path());
        BufferPool bp(2, &dm2);
        PageGuard g = bp.pin(pid);

        SlottedPage sp(g->data);
        CHECK(sp.numSlots() == 2);
        CHECK(slotEquals(sp, 0, "hello"));
        CHECK(slotEquals(sp, 1, "world"));
    }
}

TEST_CASE("a tuple of exactly MAX_TUPLE_SIZE fits on a fresh page") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    std::vector<char> big(SlottedPage::MAX_TUPLE_SIZE, 'M');
    auto sid = sp.insert(big.data(), big.size());
    REQUIRE(sid);
    CHECK(*sid == 0);

    auto [p, len] = sp.get(*sid);
    REQUIRE(p != nullptr);
    REQUIRE(len == big.size());
    CHECK(std::memcmp(p, big.data(), big.size()) == 0);
}

TEST_CASE("many one-byte tuples fill the page; all are independently readable") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    std::vector<SlotId> ids;
    for (int i = 0; i < 200; ++i) {
        const char b = static_cast<char>(i & 0xff);
        auto sid = sp.insert(&b, 1);
        if (!sid) break;
        ids.push_back(*sid);
    }
    REQUIRE(ids.size() >= 100);  // sanity floor; the page should fit many

    for (size_t i = 0; i < ids.size(); ++i) {
        auto [p, len] = sp.get(ids[i]);
        REQUIRE(p != nullptr);
        REQUIRE(len == 1);
        CHECK(*p == static_cast<char>(i & 0xff));
    }
}

TEST_CASE("update shrink-then-grow preserves slot id and final content") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto t = tup("MMMMMMMMMM");          // 10 bytes
    auto sid = sp.insert(t.data(), t.size());
    REQUIRE(sid);

    auto small = tup("xy");
    REQUIRE(sp.update(*sid, small.data(), small.size()));
    CHECK(slotEquals(sp, *sid, "xy"));

    auto larger = tup("now-this-tuple-is-quite-a-bit-larger");
    REQUIRE(sp.update(*sid, larger.data(), larger.size()));
    CHECK(slotEquals(sp, *sid, "now-this-tuple-is-quite-a-bit-larger"));
}

TEST_CASE("removing every tuple makes all tuple bytes reclaimable") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    constexpr int N = 6;
    constexpr size_t kTupleLen = 200;

    std::vector<SlotId> ids;
    for (int i = 0; i < N; ++i) {
        std::vector<char> data(kTupleLen, static_cast<char>('a' + i));
        auto sid = sp.insert(data.data(), data.size());
        REQUIRE(sid);
        ids.push_back(*sid);
    }
    for (auto sid : ids) REQUIRE(sp.remove(sid));

    // Slot ids stay reserved after remove (tombstones), so freeSpace is the
    // page minus header minus the tombstoned slot array.
    const size_t expected = PAGE_SIZE - SlottedPage::HEADER_SIZE
                          - static_cast<size_t>(N) * SlottedPage::SLOT_SIZE;
    CHECK(sp.freeSpace() == expected);

    // We can insert a tuple of size `expected`: slot 0 is the lowest
    // tombstone and gets reused (so no extra slot space is needed), and
    // compaction reclaims the dead bytes in the tuple area.
    std::vector<char> filler(expected, 'Z');
    auto sf = sp.insert(filler.data(), filler.size());
    REQUIRE(sf);
    CHECK(*sf == 0);

    auto [p, len] = sp.get(*sf);
    REQUIRE(p != nullptr);
    REQUIRE(len == expected);
    CHECK(std::memcmp(p, filler.data(), expected) == 0);
}

TEST_CASE("insert reuses the LOWEST tombstoned slot id, not an arbitrary one") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    auto a = tup("a");
    auto b = tup("b");
    auto c = tup("c");
    auto d = tup("d");
    auto sa = sp.insert(a.data(), 1); REQUIRE(sa);
    auto sb = sp.insert(b.data(), 1); REQUIRE(sb);
    auto sc = sp.insert(c.data(), 1); REQUIRE(sc);
    auto sd = sp.insert(d.data(), 1); REQUIRE(sd);

    REQUIRE(sp.remove(*sb));  // tombstone slot 1
    REQUIRE(sp.remove(*sc));  // tombstone slot 2

    auto repl = tup("R");
    auto sr = sp.insert(repl.data(), 1);
    REQUIRE(sr);
    CHECK(*sr == *sb);  // slot 1, not slot 2 — the lowest tombstone wins
}

TEST_CASE("fill-empty-fill cycle reuses bytes correctly") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    // First fill: insert until the page is full.
    std::vector<SlotId> ids;
    for (int i = 0; ; ++i) {
        std::vector<char> data(50, static_cast<char>('A' + (i % 26)));
        auto sid = sp.insert(data.data(), data.size());
        if (!sid) break;
        ids.push_back(*sid);
    }
    REQUIRE(ids.size() > 10);

    // Empty: tombstone everything.
    for (auto sid : ids) REQUIRE(sp.remove(sid));

    // Refill: every fresh insert should now succeed (compaction reclaims
    // dead bytes). Verify each new tuple round-trips.
    std::vector<SlotId> new_ids;
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
        std::vector<char> data(50, static_cast<char>('z' - (i % 26)));
        auto sid = sp.insert(data.data(), data.size());
        REQUIRE(sid);
        new_ids.push_back(*sid);
    }
    for (size_t i = 0; i < new_ids.size(); ++i) {
        auto [p, len] = sp.get(new_ids[i]);
        REQUIRE(p != nullptr);
        REQUIRE(len == 50);
        for (size_t j = 0; j < 50; ++j) {
            REQUIRE(p[j] == static_cast<char>('z' - (static_cast<int>(i) % 26)));
        }
    }
}

TEST_CASE("randomized insert/remove/update sequence stays consistent with a model") {
    std::array<char, PAGE_SIZE> page{};
    SlottedPage sp(page.data());
    sp.init();

    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> len_dist(1, 120);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    // Source of truth: what we *expect* each live slot to currently hold.
    std::map<SlotId, std::vector<char>> model;

    for (int op = 0; op < 2000; ++op) {
        const int choice = rng() % 4;
        if (model.empty() || choice == 0) {
            // INSERT
            std::vector<char> data(len_dist(rng));
            for (auto& c : data) c = static_cast<char>(byte_dist(rng));
            auto sid = sp.insert(data.data(), data.size());
            if (sid) model[*sid] = std::move(data);
            // else page genuinely full; future removes will free space.
        } else if (choice == 1) {
            // REMOVE
            auto it = std::next(model.begin(),
                                rng() % static_cast<unsigned>(model.size()));
            REQUIRE(sp.remove(it->first));
            model.erase(it);
        } else if (choice == 2) {
            // UPDATE
            auto it = std::next(model.begin(),
                                rng() % static_cast<unsigned>(model.size()));
            std::vector<char> data(len_dist(rng));
            for (auto& c : data) c = static_cast<char>(byte_dist(rng));
            if (sp.update(it->first, data.data(), data.size())) {
                it->second = std::move(data);
            }
            // else update didn't fit; model unchanged.
        } else {
            // VERIFY a random live slot
            auto it = std::next(model.begin(),
                                rng() % static_cast<unsigned>(model.size()));
            auto [p, len] = sp.get(it->first);
            REQUIRE(p != nullptr);
            REQUIRE(len == it->second.size());
            REQUIRE(std::memcmp(p, it->second.data(), len) == 0);
        }
    }

    // Final sweep: every remaining slot in the model must round-trip.
    for (const auto& [sid, expected] : model) {
        auto [p, len] = sp.get(sid);
        REQUIRE(p != nullptr);
        REQUIRE(len == expected.size());
        REQUIRE(std::memcmp(p, expected.data(), len) == 0);
    }
}
