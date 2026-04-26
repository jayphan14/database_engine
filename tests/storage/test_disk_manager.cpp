#include "tests/vendor/doctest.h"

#include "src/storage/disk_manager.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// RAII handle for a unique temp file path. Removes the file on destruction
// so each test case is isolated and the temp dir stays clean.
class TempFile {
public:
    TempFile() {
        static std::atomic<uint64_t> counter{0};
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto seq   = counter.fetch_add(1);
        path_ = std::filesystem::temp_directory_path() /
                ("dbms_test_" + std::to_string(stamp) + "_" + std::to_string(seq) + ".db");
        std::filesystem::remove(path_);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Fill a PAGE_SIZE buffer with a deterministic byte pattern keyed on `seed`,
// so every page in a test gets a distinguishable payload.
std::vector<char> makePattern(uint8_t seed) {
    std::vector<char> buf(PAGE_SIZE);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        buf[i] = static_cast<char>((i + seed) & 0xff);
    }
    return buf;
}

}  // namespace

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
