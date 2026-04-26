#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

constexpr size_t PAGE_SIZE = 4096; // 4KB to align with OS pages
using PageId = uint32_t;
constexpr PageId INVALID_PAGE_ID = std::numeric_limits<PageId>::max();

/*
Lowest layer of the storage stack: a thin wrapper over a single file that
reads and writes fixed-size pages by page_id. Page N lives at byte offset
N * PAGE_SIZE. The file grows only via allocatePage().


mydb.dat (one OS file):
NOTE: 1 OS file maps to multiple files to offset small file overhead, and limit on os file handle limits.
┌────────┬────────┬────────┬────────┬────────┬────────┬────────┐
│ page 0 │ page 1 │ page 2 │ page 3 │ page 4 │ page 5 │ page 6 │ ...
│catalog │ users  │ users  │ orders │ users  │ orders │  idx   │
└────────┴────────┴────────┴────────┴────────┴────────┴────────┘
*/

class DiskManager {
public:
    explicit DiskManager(const std::string& filename);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void readPage(PageId page_id, char* dest);
    void writePage(PageId page_id, const char* src);
    PageId allocatePage();
    PageId numPages() const;

private:
    std::string filename_;
    std::fstream file_;
    PageId next_page_id_;
};
