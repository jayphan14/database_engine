#include "tests/vendor/doctest.h"

#include "src/storage/disk_manager.h"
#include "tests/test_util.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("allocate-write-read round trip (usage demo)") {
    TempFile tf;
    DiskManager dm(tf.path());

    // 1. Allocate a page — file grows by PAGE_SIZE bytes.
    const PageId pid = dm.allocatePage();
    CHECK(pid == 0);

    // 2. Write a known pattern into that page.
    const auto pattern = makePattern(0xAB);
    dm.writePage(pid, pattern.data());

    // 3. Read it back into a fresh buffer and verify byte-for-byte equality.
    std::vector<char> read_back(PAGE_SIZE);
    dm.readPage(pid, read_back.data());
    CHECK(std::memcmp(read_back.data(), pattern.data(), PAGE_SIZE) == 0);
}

TEST_CASE("multiple pages are independent") {
    TempFile tf;
    DiskManager dm(tf.path());

    const PageId p0 = dm.allocatePage();
    const PageId p1 = dm.allocatePage();
    const PageId p2 = dm.allocatePage();

    const auto pat0 = makePattern(0x11);
    const auto pat1 = makePattern(0x22);
    const auto pat2 = makePattern(0x33);
    dm.writePage(p0, pat0.data());
    dm.writePage(p1, pat1.data());
    dm.writePage(p2, pat2.data());

    std::vector<char> buf(PAGE_SIZE);
    dm.readPage(p0, buf.data()); CHECK(std::memcmp(buf.data(), pat0.data(), PAGE_SIZE) == 0);
    dm.readPage(p1, buf.data()); CHECK(std::memcmp(buf.data(), pat1.data(), PAGE_SIZE) == 0);
    dm.readPage(p2, buf.data()); CHECK(std::memcmp(buf.data(), pat2.data(), PAGE_SIZE) == 0);
}

TEST_CASE("numPages() reflects allocations") {
    TempFile tf;
    DiskManager dm(tf.path());

    CHECK(dm.numPages() == 0);
    dm.allocatePage();
    CHECK(dm.numPages() == 1);
    dm.allocatePage();
    dm.allocatePage();
    CHECK(dm.numPages() == 3);
}

TEST_CASE("allocatePage returns sequential ids starting at 0") {
    TempFile tf;
    DiskManager dm(tf.path());

    CHECK(dm.allocatePage() == 0);
    CHECK(dm.allocatePage() == 1);
    CHECK(dm.allocatePage() == 2);
}

TEST_CASE("data persists across DiskManager instances") {
    TempFile tf;

    const auto pattern = makePattern(0x5A);
    PageId pid;
    {
        DiskManager dm(tf.path());
        pid = dm.allocatePage();
        dm.writePage(pid, pattern.data());
    }  // dm destroyed, file closed

    DiskManager dm2(tf.path());
    CHECK(dm2.numPages() == 1);
    std::vector<char> buf(PAGE_SIZE);
    dm2.readPage(pid, buf.data());
    CHECK(std::memcmp(buf.data(), pattern.data(), PAGE_SIZE) == 0);
}

TEST_CASE("readPage on out-of-range page id throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    dm.allocatePage();

    std::vector<char> buf(PAGE_SIZE);
    CHECK_THROWS_AS(dm.readPage(5, buf.data()), std::runtime_error);
}

TEST_CASE("writePage on out-of-range page id throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    dm.allocatePage();

    const auto pattern = makePattern(0x01);
    CHECK_THROWS_AS(dm.writePage(5, pattern.data()), std::runtime_error);
}

TEST_CASE("opening a file whose size is not a multiple of PAGE_SIZE throws") {
    TempFile tf;
    {
        std::ofstream out(tf.path(), std::ios::binary);
        const std::string garbage(PAGE_SIZE + 17, '\0');  // deliberately misaligned
        out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    CHECK_THROWS_AS(DiskManager dm(tf.path()), std::runtime_error);
}
