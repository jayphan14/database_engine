#include "src/storage/slotted_page.h"

#include <cstring>
#include <vector>

namespace {

uint16_t loadU16(const char* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t loadU32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void storeU16(char* p, uint16_t v) {
    std::memcpy(p, &v, sizeof(v));
}

void storeU32(char* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

}  // namespace

uint16_t SlottedPage::numSlotsRaw() const     { return loadU16(data_ + 8); }
void     SlottedPage::setNumSlots(uint16_t v) { storeU16(data_ + 8, v); }
uint16_t SlottedPage::freeSpaceOffset() const { return loadU16(data_ + 10); }
void     SlottedPage::setFreeSpaceOffset(uint16_t v) { storeU16(data_ + 10, v); }

PageId SlottedPage::nextPageId() const          { return loadU32(data_ + 4); }
void   SlottedPage::setNextPageId(PageId next)  { storeU32(data_ + 4, next); }

SlottedPage::SlotEntry SlottedPage::readSlot(SlotId i) const {
    const char* p = data_ + HEADER_SIZE + static_cast<size_t>(i) * SLOT_SIZE;
    return {loadU16(p), loadU16(p + 2)};
}

void SlottedPage::writeSlot(SlotId i, SlotEntry e) {
    char* p = data_ + HEADER_SIZE + static_cast<size_t>(i) * SLOT_SIZE;
    storeU16(p, e.offset);
    storeU16(p + 2, e.length);
}

size_t SlottedPage::endOfSlotArray() const {
    return HEADER_SIZE + static_cast<size_t>(numSlotsRaw()) * SLOT_SIZE;
}

size_t SlottedPage::contiguousFree() const {
    return freeSpaceOffset() - endOfSlotArray();
}

size_t SlottedPage::deadTupleBytes() const {
    // Bytes inside the tuple area (between freeSpaceOffset and PAGE_SIZE)
    // that are not pointed to by any live slot. Reclaimed by compact().
    size_t live = 0;
    const uint16_t n = numSlotsRaw();
    for (SlotId i = 0; i < n; ++i) {
        SlotEntry s = readSlot(i);
        if (isLive(s)) live += s.length;
    }
    const size_t tuple_area = PAGE_SIZE - freeSpaceOffset();
    return tuple_area - live;
}

void SlottedPage::init() {
    storeU32(data_ + 0, 0);                           // lsn
    storeU32(data_ + 4, INVALID_PAGE_ID);             // next_page_id
    setNumSlots(0);
    setFreeSpaceOffset(static_cast<uint16_t>(PAGE_SIZE));
    // Zero the rest of the page so reads from uninitialized regions are
    // deterministic. Cheap and helpful when debugging hex dumps.
    std::memset(data_ + HEADER_SIZE, 0, PAGE_SIZE - HEADER_SIZE);
}

size_t SlottedPage::numSlots() const {
    return numSlotsRaw();
}

size_t SlottedPage::freeSpace() const {
    return contiguousFree() + deadTupleBytes();
}

std::optional<SlotId> SlottedPage::findDeadSlot() const {
    const uint16_t n = numSlotsRaw();
    for (SlotId i = 0; i < n; ++i) {
        if (!isLive(readSlot(i))) return i;
    }
    return std::nullopt;
}

void SlottedPage::compact() {
    const uint16_t n = numSlotsRaw();
    std::vector<char> scratch(PAGE_SIZE);
    uint16_t new_offset = static_cast<uint16_t>(PAGE_SIZE);
    for (SlotId i = 0; i < n; ++i) {
        SlotEntry s = readSlot(i);
        if (!isLive(s)) continue;
        new_offset -= s.length;
        std::memcpy(scratch.data() + new_offset, data_ + s.offset, s.length);
        writeSlot(i, {new_offset, s.length});
    }
    // Copy the repacked tuple area back. Region above new_offset is untouched.
    std::memcpy(data_ + new_offset,
                scratch.data() + new_offset,
                PAGE_SIZE - new_offset);
    setFreeSpaceOffset(new_offset);
}

std::optional<SlotId> SlottedPage::insert(const char* tuple, size_t len) {
    if (len == 0 || len > MAX_TUPLE_SIZE) return std::nullopt;

    const auto reuse = findDeadSlot();
    const size_t needed_contig = len + (reuse.has_value() ? 0 : SLOT_SIZE);
    const size_t needed_total  = needed_contig;  // same after compaction

    if (contiguousFree() < needed_contig) {
        if (freeSpace() < needed_total) return std::nullopt;
        compact();
        if (contiguousFree() < needed_contig) return std::nullopt;  // defensive
    }

    const uint16_t new_offset = static_cast<uint16_t>(freeSpaceOffset() - len);
    std::memcpy(data_ + new_offset, tuple, len);
    setFreeSpaceOffset(new_offset);

    SlotId slot_id;
    if (reuse) {
        slot_id = *reuse;
    } else {
        slot_id = static_cast<SlotId>(numSlotsRaw());
        setNumSlots(static_cast<uint16_t>(slot_id + 1));
    }
    writeSlot(slot_id, {new_offset, static_cast<uint16_t>(len)});
    return slot_id;
}

std::pair<const char*, size_t> SlottedPage::get(SlotId i) const {
    if (i >= numSlotsRaw()) return {nullptr, 0};
    SlotEntry s = readSlot(i);
    if (!isLive(s)) return {nullptr, 0};
    return {data_ + s.offset, s.length};
}

bool SlottedPage::remove(SlotId i) {
    if (i >= numSlotsRaw()) return false;
    SlotEntry s = readSlot(i);
    if (!isLive(s)) return false;
    writeSlot(i, {0, 0});
    return true;
}

bool SlottedPage::update(SlotId i, const char* tuple, size_t len) {
    if (i >= numSlotsRaw()) return false;
    SlotEntry s = readSlot(i);
    if (!isLive(s)) return false;
    if (len == 0 || len > MAX_TUPLE_SIZE) return false;

    if (len <= s.length) {
        // In-place. Wastes (s.length - len) bytes until the next compaction.
        std::memcpy(data_ + s.offset, tuple, len);
        writeSlot(i, {s.offset, static_cast<uint16_t>(len)});
        return true;
    }

    // New length is larger. Tombstoning the old bytes adds s.length to the
    // free pool; if even that is not enough, the update cannot fit.
    const size_t available_after_tombstone = freeSpace() + s.length;
    if (available_after_tombstone < len) return false;

    writeSlot(i, {0, 0});  // tombstone — old bytes now count as dead
    if (contiguousFree() < len) compact();

    const uint16_t new_offset = static_cast<uint16_t>(freeSpaceOffset() - len);
    std::memcpy(data_ + new_offset, tuple, len);
    setFreeSpaceOffset(new_offset);
    writeSlot(i, {new_offset, static_cast<uint16_t>(len)});
    return true;
}
