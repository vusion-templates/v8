// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SANDBOX_EXTERNAL_ENTITY_TABLE_INL_H_
#define V8_SANDBOX_EXTERNAL_ENTITY_TABLE_INL_H_

#include "src/base/atomicops.h"
#include "src/base/emulated-virtual-address-subspace.h"
#include "src/common/assert-scope.h"
#include "src/sandbox/external-entity-table.h"
#include "src/utils/allocation.h"

#ifdef V8_COMPRESS_POINTERS

namespace v8 {
namespace internal {

template <typename Entry, size_t size>
typename ExternalEntityTable<Entry, size>::Segment
ExternalEntityTable<Entry, size>::Segment::At(uint32_t offset) {
  DCHECK(IsAligned(offset, kSegmentSize));
  uint32_t number = offset / kSegmentSize;
  return Segment(number);
}

template <typename Entry, size_t size>
typename ExternalEntityTable<Entry, size>::Segment
ExternalEntityTable<Entry, size>::Segment::Containing(uint32_t entry_index) {
  uint32_t number = entry_index / kEntriesPerSegment;
  return Segment(number);
}

template <typename Entry, size_t size>
ExternalEntityTable<Entry, size>::Space::~Space() {
  // The segments belonging to this space must have already been deallocated
  // (through TearDownSpace()), otherwise we may leak memory.
  DCHECK(segments_.empty());
}

template <typename Entry, size_t size>
uint32_t ExternalEntityTable<Entry, size>::Space::freelist_length() const {
  auto freelist = freelist_head_.load(std::memory_order_relaxed);
  return freelist.length();
}

template <typename Entry, size_t size>
uint32_t ExternalEntityTable<Entry, size>::Space::num_segments() {
  mutex_.AssertHeld();
  return static_cast<uint32_t>(segments_.size());
}

template <typename Entry, size_t size>
bool ExternalEntityTable<Entry, size>::Space::Contains(uint32_t index) {
  base::MutexGuard guard(&mutex_);
  Segment segment = Segment::Containing(index);
  return segments_.find(segment) != segments_.end();
}

template <typename Entry, size_t size>
Entry& ExternalEntityTable<Entry, size>::at(uint32_t index) {
  return base_[index];
}

template <typename Entry, size_t size>
const Entry& ExternalEntityTable<Entry, size>::at(uint32_t index) const {
  return base_[index];
}

template <typename Entry, size_t size>
bool ExternalEntityTable<Entry, size>::is_initialized() const {
  DCHECK(!base_ || reinterpret_cast<Address>(base_) == vas_->base());
  return base_ != nullptr;
}

template <typename Entry, size_t size>
void ExternalEntityTable<Entry, size>::InitializeTable() {
  DCHECK(!is_initialized());
  DCHECK_EQ(vas_, nullptr);

  VirtualAddressSpace* root_space = GetPlatformVirtualAddressSpace();
  DCHECK(IsAligned(kReservationSize, root_space->allocation_granularity()));

  if (root_space->CanAllocateSubspaces()) {
    auto subspace = root_space->AllocateSubspace(VirtualAddressSpace::kNoHint,
                                                 kReservationSize, kSegmentSize,
                                                 PagePermissions::kReadWrite);
    vas_ = subspace.release();
  } else {
    // This may be required on old Windows versions that don't support
    // VirtualAlloc2, which is required for subspaces. In that case, just use a
    // fully-backed emulated subspace.
    Address reservation_base = root_space->AllocatePages(
        VirtualAddressSpace::kNoHint, kReservationSize, kSegmentSize,
        PagePermissions::kReadWrite);
    if (reservation_base) {
      vas_ = new base::EmulatedVirtualAddressSubspace(
          root_space, reservation_base, kReservationSize, kReservationSize);
    }
  }
  if (!vas_) {
    V8::FatalProcessOutOfMemory(
        nullptr, "ExternalEntityTable::InitializeTable (subspace allocation)");
  }
  base_ = reinterpret_cast<Entry*>(vas_->base());

  // Allocate the first segment of the table as read-only memory. This segment
  // will contain the null entry, which should always contain nullptr.
  auto first_segment = vas_->AllocatePages(
      vas_->base(), kSegmentSize, kSegmentSize, PagePermissions::kRead);
  if (first_segment != vas_->base()) {
    V8::FatalProcessOutOfMemory(
        nullptr,
        "ExternalEntityTable::InitializeTable (first segment allocation)");
  }
}

template <typename Entry, size_t size>
void ExternalEntityTable<Entry, size>::TearDownTable() {
  DCHECK(is_initialized());

  // Deallocate the (read-only) first segment.
  vas_->FreePages(vas_->base(), kSegmentSize);

  base_ = nullptr;
  delete vas_;
  vas_ = nullptr;
}

template <typename Entry, size_t size>
void ExternalEntityTable<Entry, size>::InitializeSpace(Space* space) {
#ifdef DEBUG
  DCHECK_EQ(space->owning_table_, nullptr);
  space->owning_table_ = this;
#endif
}

template <typename Entry, size_t size>
void ExternalEntityTable<Entry, size>::TearDownSpace(Space* space) {
  DCHECK(is_initialized());
  DCHECK(space->BelongsTo(this));
  for (auto segment : space->segments_) {
    FreeTableSegment(segment);
  }
  space->segments_.clear();
}

template <typename Entry, size_t size>
uint32_t ExternalEntityTable<Entry, size>::AllocateEntry(Space* space) {
  DCHECK(is_initialized());
  DCHECK(space->BelongsTo(this));

  // We currently don't want entry allocation to trigger garbage collection as
  // this may cause seemingly harmless pointer field assignments to trigger
  // garbage collection. This is especially true for lazily-initialized
  // external pointer slots which will typically only allocate the external
  // pointer table entry when the pointer is first set to a non-null value.
  DisallowGarbageCollection no_gc;

  FreelistHead freelist;
  bool success = false;
  while (!success) {
    // This is essentially DCLP (see
    // https://preshing.com/20130930/double-checked-locking-is-fixed-in-cpp11/)
    // and so requires an acquire load as well as a release store in Grow() to
    // prevent reordering of memory accesses, which could for example cause one
    // thread to read a freelist entry before it has been properly initialized.
    freelist = space->freelist_head_.load(std::memory_order_acquire);
    if (freelist.is_empty()) {
      // Freelist is empty. Need to take the lock, then attempt to allocate a
      // new segment if no other thread has done it in the meantime.
      base::MutexGuard guard(&space->mutex_);

      // Reload freelist head in case another thread already grew the table.
      freelist = space->freelist_head_.load(std::memory_order_relaxed);

      if (freelist.is_empty()) {
        // Freelist is (still) empty so extend this space by another segment.
        freelist = Extend(space);
        // Extend() adds one segment to the space and so to its freelist.
        DCHECK_EQ(freelist.length(), kEntriesPerSegment);
      }
    }

    success = TryAllocateEntryFromFreelist(space, freelist);
  }

  uint32_t allocated_entry = freelist.next();
  DCHECK(space->Contains(allocated_entry));
  DCHECK_NE(allocated_entry, 0);
  return allocated_entry;
}

template <typename Entry, size_t size>
uint32_t ExternalEntityTable<Entry, size>::AllocateEntryBelow(
    Space* space, uint32_t threshold_index) {
  DCHECK(is_initialized());

  FreelistHead freelist;
  bool success = false;
  while (!success) {
    freelist = space->freelist_head_.load(std::memory_order_acquire);
    // Check that the next free entry is below the threshold.
    if (freelist.is_empty() || freelist.next() >= threshold_index) return 0;

    success = TryAllocateEntryFromFreelist(space, freelist);
  }

  uint32_t allocated_entry = freelist.next();
  DCHECK(space->Contains(allocated_entry));
  DCHECK_NE(allocated_entry, 0);
  DCHECK_LT(allocated_entry, threshold_index);
  return allocated_entry;
}

template <typename Entry, size_t size>
bool ExternalEntityTable<Entry, size>::TryAllocateEntryFromFreelist(
    Space* space, FreelistHead freelist) {
  DCHECK(!freelist.is_empty());
  DCHECK(space->Contains(freelist.next()));

  Entry& freelist_entry = at(freelist.next());
  uint32_t next_freelist_entry = freelist_entry.GetNextFreelistEntryIndex();
  FreelistHead new_freelist(next_freelist_entry, freelist.length() - 1);
  bool success = space->freelist_head_.compare_exchange_strong(
      freelist, new_freelist, std::memory_order_relaxed);

  // When the CAS succeeded, the entry must've been a freelist entry.
  // Otherwise, this is not guaranteed as another thread may have allocated
  // and overwritten the same entry in the meantime.
  if (success) {
    DCHECK_IMPLIES(freelist.length() > 1, !new_freelist.is_empty());
    DCHECK_IMPLIES(freelist.length() == 1, new_freelist.is_empty());
  }
  return success;
}

template <typename Entry, size_t size>
typename ExternalEntityTable<Entry, size>::FreelistHead
ExternalEntityTable<Entry, size>::Extend(Space* space) {
  // Freelist should be empty when calling this method.
  DCHECK_EQ(space->freelist_length(), 0);
  // The caller must lock the space's mutex before extending it.
  space->mutex_.AssertHeld();

  // Allocate the new segment.
  Segment segment = AllocateTableSegment();
  space->segments_.insert(segment);
  DCHECK_NE(segment.number(), 0);

  // Refill the freelist with the entries in the newly allocated segment.
  uint32_t first = segment.first_entry();
  uint32_t last = segment.last_entry();
  for (uint32_t i = first; i < last; i++) {
    uint32_t next_free_entry = i + 1;
    at(i).MakeFreelistEntry(next_free_entry);
  }
  at(last).MakeFreelistEntry(0);

  // This must be a release store to prevent reordering of the preceeding
  // stores to the freelist from being reordered past this store. See
  // AllocateEntry() for more details.
  FreelistHead new_freelist_head(first, last - first + 1);
  space->freelist_head_.store(new_freelist_head, std::memory_order_release);

  return new_freelist_head;
}

template <typename Entry, size_t size>
typename ExternalEntityTable<Entry, size>::Segment
ExternalEntityTable<Entry, size>::AllocateTableSegment() {
  Address start =
      vas_->AllocatePages(VirtualAddressSpace::kNoHint, kSegmentSize,
                          kSegmentSize, PagePermissions::kReadWrite);
  if (!start) {
    V8::FatalProcessOutOfMemory(nullptr,
                                "ExternalEntityTable::AllocateSegment");
  }
  uint32_t offset = static_cast<uint32_t>((start - vas_->base()));
  return Segment::At(offset);
}

template <typename Entry, size_t size>
void ExternalEntityTable<Entry, size>::FreeTableSegment(Segment segment) {
  // Segment zero is reserved.
  DCHECK_NE(segment.number(), 0);
  Address segment_start = vas_->base() + segment.offset();
  vas_->FreePages(segment_start, kSegmentSize);
}

}  // namespace internal
}  // namespace v8

#endif  // V8_COMPRESS_POINTERS

#endif  // V8_SANDBOX_EXTERNAL_ENTITY_TABLE_INL_H_
