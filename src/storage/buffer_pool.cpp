#include "src/storage/buffer_pool.h"

#include <cstring>
#include <stdexcept>
#include <string>

BufferPool::BufferPool(size_t num_frames, DiskManager* disk)
    : disk_(disk), frames_(num_frames) {
    if (num_frames == 0) {
        throw std::runtime_error("BufferPool: num_frames must be > 0");
    }
    if (disk_ == nullptr) {
        throw std::runtime_error("BufferPool: disk must not be null");
    }
    for (size_t i = 0; i < num_frames; ++i) {
        Frame& f = frames_[i];
        f.page_id = INVALID_PAGE_ID;
        f.is_dirty = false;
        f.pin_count = 0;
        std::memset(f.data, 0, PAGE_SIZE);
        addToLRU(i);
    }
}

void BufferPool::removeFromLRU(size_t frame_idx) {
    auto it = lru_pos_.find(frame_idx);
    if (it == lru_pos_.end()) return;
    lru_.erase(it->second);
    lru_pos_.erase(it);
}

void BufferPool::addToLRU(size_t frame_idx) {
    lru_.push_back(frame_idx);
    lru_pos_[frame_idx] = std::prev(lru_.end());
}

size_t BufferPool::pickVictim() {
    if (lru_.empty()) {
        throw std::runtime_error("BufferPool: all frames pinned, no victim available");
    }
    return lru_.front();
}

void BufferPool::evict(size_t frame_idx) {
    Frame& f = frames_[frame_idx];
    if (f.page_id == INVALID_PAGE_ID) return;  // empty slot, nothing to evict
    if (f.is_dirty) {
        disk_->writePage(f.page_id, f.data);
        f.is_dirty = false;
    }
    page_table_.erase(f.page_id);
    f.page_id = INVALID_PAGE_ID;
}

Frame* BufferPool::fetchPage(PageId page_id) {
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        // Cache hit. If currently unpinned, pull it out of the LRU list so
        // it can't be evicted while pinned.
        size_t idx = it->second;
        Frame& f = frames_[idx];
        if (f.pin_count == 0) {
            removeFromLRU(idx);
        }
        ++f.pin_count;
        return &f;
    }

    // Miss: pick a victim and load the page into its slot.
    size_t idx = pickVictim();
    removeFromLRU(idx);
    evict(idx);

    Frame& f = frames_[idx];
    disk_->readPage(page_id, f.data);
    f.page_id = page_id;
    f.is_dirty = false;
    f.pin_count = 1;
    page_table_[page_id] = idx;
    return &f;
}

void BufferPool::unpinPage(PageId page_id, bool was_modified) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        throw std::runtime_error("BufferPool::unpinPage: page " +
                                 std::to_string(page_id) + " not in pool");
    }
    size_t idx = it->second;
    Frame& f = frames_[idx];
    if (f.pin_count <= 0) {
        throw std::runtime_error("BufferPool::unpinPage: page " +
                                 std::to_string(page_id) + " not pinned");
    }
    if (was_modified) f.is_dirty = true;
    --f.pin_count;
    if (f.pin_count == 0) {
        addToLRU(idx);
    }
}

Frame* BufferPool::newPage(PageId* out_page_id) {
    PageId new_id = disk_->allocatePage();
    if (out_page_id) *out_page_id = new_id;

    size_t idx = pickVictim();
    removeFromLRU(idx);
    evict(idx);

    Frame& f = frames_[idx];
    std::memset(f.data, 0, PAGE_SIZE);
    f.page_id = new_id;
    f.is_dirty = false;
    f.pin_count = 1;
    page_table_[new_id] = idx;
    return &f;
}

void BufferPool::flushPage(PageId page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;
    Frame& f = frames_[it->second];
    if (f.is_dirty) {
        disk_->writePage(f.page_id, f.data);
        f.is_dirty = false;
    }
}

void BufferPool::flushAll() {
    for (auto& f : frames_) {
        if (f.page_id != INVALID_PAGE_ID && f.is_dirty) {
            disk_->writePage(f.page_id, f.data);
            f.is_dirty = false;
        }
    }
}

PageGuard BufferPool::pin(PageId page_id) {
    Frame* f = fetchPage(page_id);
    return PageGuard(this, f);
}

PageGuard BufferPool::pinNew() {
    PageId pid = INVALID_PAGE_ID;
    Frame* f = newPage(&pid);
    return PageGuard(this, f);
}

PageGuard::~PageGuard() {
    if (frame_ && bp_) {
        // Frame::is_dirty was already set by markDirty() (if at all), so the
        // unpin call doesn't need to OR in any additional bit.
        bp_->unpinPage(frame_->page_id, false);
    }
}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : bp_(other.bp_), frame_(other.frame_) {
    other.bp_ = nullptr;
    other.frame_ = nullptr;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        if (frame_ && bp_) {
            bp_->unpinPage(frame_->page_id, false);
        }
        bp_ = other.bp_;
        frame_ = other.frame_;
        other.bp_ = nullptr;
        other.frame_ = nullptr;
    }
    return *this;
}
