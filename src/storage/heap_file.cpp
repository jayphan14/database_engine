#include "src/storage/heap_file.h"

#include <stdexcept>
#include <string>

HeapFile::HeapFile(BufferPool* bp, PageId first_page_id)
    : bp_(bp), first_page_(first_page_id) {}

HeapFile HeapFile::create(BufferPool* bp) {
    PageGuard g = bp->pinNew();
    SlottedPage sp(g->data);
    sp.init();
    g.markDirty();
    return HeapFile(bp, g->page_id);
}

RID HeapFile::insert(const char* tuple, size_t len) {
    if (len == 0 || len > SlottedPage::MAX_TUPLE_SIZE) {
        throw std::runtime_error("HeapFile::insert: invalid tuple length " +
                                 std::to_string(len));
    }

    // Walk the chain looking for a page that can hold this tuple. Track the
    // tail in case we need to allocate a new page and link to it.
    PageId current = first_page_;
    PageId tail = first_page_;
    while (current != INVALID_PAGE_ID) {
        PageGuard g = bp_->pin(current);
        SlottedPage sp(g->data);
        auto sid = sp.insert(tuple, len);
        if (sid) {
            g.markDirty();
            return RID{current, *sid};
        }
        tail = current;
        current = sp.nextPageId();
    }

    // No page in the chain has room. Allocate a new page, insert into it,
    // then re-pin the tail to update its next pointer. Doing the work in
    // two stages avoids pinning two pages simultaneously, so this stays
    // correct even on a single-frame buffer pool.
    PageId new_pid;
    SlotId new_sid;
    {
        PageGuard g = bp_->pinNew();
        new_pid = g->page_id;
        SlottedPage sp(g->data);
        sp.init();
        auto sid = sp.insert(tuple, len);
        if (!sid) {
            // MAX_TUPLE_SIZE check above means this is a bug, not user error.
            throw std::runtime_error("HeapFile::insert: tuple did not fit on a fresh page");
        }
        new_sid = *sid;
        g.markDirty();
    }
    {
        PageGuard g = bp_->pin(tail);
        SlottedPage sp(g->data);
        sp.setNextPageId(new_pid);
        g.markDirty();
    }
    return RID{new_pid, new_sid};
}

bool HeapFile::get(RID rid, std::string* out) {
    PageGuard g = bp_->pin(rid.page_id);
    SlottedPage sp(g->data);
    auto [p, len] = sp.get(rid.slot_id);
    if (p == nullptr) return false;
    out->assign(p, len);
    return true;
}

bool HeapFile::remove(RID rid) {
    PageGuard g = bp_->pin(rid.page_id);
    SlottedPage sp(g->data);
    bool ok = sp.remove(rid.slot_id);
    if (ok) g.markDirty();
    return ok;
}

HeapFile::Iterator::Iterator(BufferPool* bp, PageId start)
    : bp_(bp), cur_page_(start), cur_slot_(0) {
    advanceToLive();
}

HeapFile::Iterator& HeapFile::Iterator::operator++() {
    ++cur_slot_;
    advanceToLive();
    return *this;
}

void HeapFile::Iterator::advanceToLive() {
    while (cur_page_ != INVALID_PAGE_ID) {
        PageGuard g = bp_->pin(cur_page_);
        SlottedPage sp(g->data);
        const size_t n = sp.numSlots();
        while (cur_slot_ < n) {
            auto [p, len] = sp.get(cur_slot_);
            if (p != nullptr) {
                entry_.first = RID{cur_page_, cur_slot_};
                entry_.second.assign(p, len);
                return;
            }
            ++cur_slot_;
        }
        cur_page_ = sp.nextPageId();
        cur_slot_ = 0;
    }
    // Exhausted: cur_page_ == INVALID_PAGE_ID, cur_slot_ == 0 — matches
    // the default-constructed end iterator.
}
