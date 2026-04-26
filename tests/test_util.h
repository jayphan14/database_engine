#pragma once

#include "src/storage/disk_manager.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// RAII handle for a unique temp file path. Creates a unique path under the
// system temp dir on construction (with the file removed if anything stale
// existed) and removes it on destruction so test cases stay isolated.
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

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Fill a PAGE_SIZE buffer with a deterministic byte pattern keyed on `seed`,
// so distinct pages get visually distinguishable payloads.
inline std::vector<char> makePattern(uint8_t seed) {
    std::vector<char> buf(PAGE_SIZE);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        buf[i] = static_cast<char>((i + seed) & 0xff);
    }
    return buf;
}
