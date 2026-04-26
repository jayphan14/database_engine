#include "src/storage/disk_manager.h"

#include <filesystem>
#include <stdexcept>
#include <vector>

DiskManager::DiskManager(const std::string& filename)
    : filename_(filename), next_page_id_(0) {
    namespace fs = std::filesystem;

    // Ensure the file exists before opening for read+write; std::fstream with
    // in|out will not create a missing file on its own.
    if (!fs::exists(filename_)) {
        std::ofstream create(filename_, std::ios::binary);
        if (!create) {
            throw std::runtime_error("DiskManager: failed to create file '" + filename_ + "'");
        }
    }

    file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) {
        throw std::runtime_error("DiskManager: failed to open file '" + filename_ + "'");
    }

    const auto size = fs::file_size(filename_);
    if (size % PAGE_SIZE != 0) {
        throw std::runtime_error("DiskManager: file size " + std::to_string(size) +
                                 " is not a multiple of PAGE_SIZE");
    }
    next_page_id_ = static_cast<PageId>(size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.close();
    }
}

// just seek to where that page started and read the next PAGE_SIZE bytes
void DiskManager::readPage(PageId page_id, char* dest) {
    if (page_id >= next_page_id_) {
        throw std::runtime_error("DiskManager::readPage: page_id " + std::to_string(page_id) +
                                 " out of range (numPages=" + std::to_string(next_page_id_) + ")");
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(page_id) * PAGE_SIZE, std::ios::beg);
    file_.read(dest, PAGE_SIZE);
    if (file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        throw std::runtime_error("DiskManager::readPage: short read on page " + std::to_string(page_id));
    }
}

void DiskManager::writePage(PageId page_id, const char* src) {
    if (page_id >= next_page_id_) {
        throw std::runtime_error("DiskManager::writePage: page_id " + std::to_string(page_id) +
                                 " out of range (numPages=" + std::to_string(next_page_id_) + ")");
    }
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE, std::ios::beg);
    file_.write(src, PAGE_SIZE);
    if (!file_) {
        throw std::runtime_error("DiskManager::writePage: write failed on page " + std::to_string(page_id));
    }
    file_.flush();
}

// TODO: may be double to amortize cost?
PageId DiskManager::allocatePage() {
    const PageId new_id = next_page_id_;
    std::vector<char> zeros(PAGE_SIZE, 0);
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(new_id) * PAGE_SIZE, std::ios::beg);
    file_.write(zeros.data(), PAGE_SIZE);
    if (!file_) {
        throw std::runtime_error("DiskManager::allocatePage: write failed");
    }
    file_.flush();
    ++next_page_id_;
    return new_id;
}

PageId DiskManager::numPages() const {
    return next_page_id_;
}
