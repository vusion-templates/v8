// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SANDBOX_EXTERNAL_POINTER_TABLE_INL_H_
#define V8_SANDBOX_EXTERNAL_POINTER_TABLE_INL_H_

#include "src/sandbox/external-entity-table-inl.h"
#include "src/sandbox/external-pointer-table.h"
#include "src/sandbox/external-pointer.h"

#ifdef V8_COMPRESS_POINTERS

namespace v8 {
namespace internal {

void ExternalPointerTableEntry::MakeExternalPointerEntry(
    Address value, ExternalPointerTag tag) {
  DCHECK_EQ(0, value & kExternalPointerTagMask);
  DCHECK(tag & kExternalPointerMarkBit);
  DCHECK_NE(tag, kExternalPointerFreeEntryTag);
  DCHECK_NE(tag, kExternalPointerEvacuationEntryTag);

  Payload new_payload(value, tag);
  payload_.store(new_payload, std::memory_order_relaxed);
  MaybeUpdateRawPointerForLSan(value);
}

Address ExternalPointerTableEntry::GetExternalPointer(
    ExternalPointerTag tag) const {
  auto payload = payload_.load(std::memory_order_relaxed);
  DCHECK(payload.ContainsExternalPointer());
  return payload.Untag(tag);
}

void ExternalPointerTableEntry::SetExternalPointer(Address value,
                                                   ExternalPointerTag tag) {
  DCHECK_EQ(0, value & kExternalPointerTagMask);
  DCHECK(tag & kExternalPointerMarkBit);
  DCHECK(payload_.load(std::memory_order_relaxed).ContainsExternalPointer());

  Payload new_payload(value, tag);
  payload_.store(new_payload, std::memory_order_relaxed);
  MaybeUpdateRawPointerForLSan(value);
}

Address ExternalPointerTableEntry::ExchangeExternalPointer(
    Address value, ExternalPointerTag tag) {
  DCHECK_EQ(0, value & kExternalPointerTagMask);
  DCHECK(tag & kExternalPointerMarkBit);

  Payload new_payload(value, tag);
  Payload old_payload =
      payload_.exchange(new_payload, std::memory_order_relaxed);
  DCHECK(old_payload.ContainsExternalPointer());
  MaybeUpdateRawPointerForLSan(value);
  return old_payload.Untag(tag);
}

void ExternalPointerTableEntry::MakeFreelistEntry(uint32_t next_entry_index) {
  // The next freelist entry is stored in the lower bits of the entry.
  static_assert(kMaxExternalPointers <= std::numeric_limits<uint32_t>::max());
  Payload new_payload(next_entry_index, kExternalPointerFreeEntryTag);
  payload_.store(new_payload, std::memory_order_relaxed);
}

uint32_t ExternalPointerTableEntry::GetNextFreelistEntryIndex() const {
  auto payload = payload_.load(std::memory_order_relaxed);
  return payload.ExtractFreelistLink();
}

void ExternalPointerTableEntry::Mark() {
  auto old_payload = payload_.load(std::memory_order_relaxed);
  DCHECK(old_payload.ContainsExternalPointer());

  auto new_payload = old_payload;
  new_payload.SetMarkBit();

  // We don't need to perform the CAS in a loop: if the new value is not equal
  // to the old value, then the mutator must've just written a new value into
  // the entry. This in turn must've set the marking bit already (see e.g.
  // StoreExternalPointer), so we don't need to do it again.
  bool success = payload_.compare_exchange_strong(old_payload, new_payload,
                                                  std::memory_order_relaxed);
  DCHECK(success || old_payload.HasMarkBitSet());
  USE(success);
}

void ExternalPointerTableEntry::MakeEvacuationEntry(Address handle_location) {
  Payload new_payload(handle_location, kExternalPointerEvacuationEntryTag);
  payload_.store(new_payload, std::memory_order_relaxed);
}

void ExternalPointerTableEntry::UnmarkAndMigrateInto(
    ExternalPointerTableEntry& other) {
  auto payload = payload_.load(std::memory_order_relaxed);
  // We expect to only migrate entries containing external pointers.
  DCHECK(payload.ContainsExternalPointer());

  // During compaction, entries that are evacuated may not be visited during
  // sweeping and may therefore still have their marking bit set. As such, we
  // should clear that here.
  payload.ClearMarkBit();

  other.payload_.store(payload, std::memory_order_relaxed);
#if defined(LEAK_SANITIZER)
  other.raw_pointer_for_lsan_ = raw_pointer_for_lsan_;
#endif  // LEAK_SANITIZER

#ifdef DEBUG
  // In debug builds, we clobber this old entry so that any sharing of table
  // entries is easily detected. Shared entries would require write barriers,
  // so we'd like to avoid them. See the compaction algorithm explanation in
  // external-pointer-table.h for more details.
  constexpr Address kClobberedEntryMarker = static_cast<Address>(-1);
  Payload clobbered(kClobberedEntryMarker, kExternalPointerNullTag);
  DCHECK_NE(payload, clobbered);
  payload_.store(clobbered, std::memory_order_relaxed);
#endif  // DEBUG
}

Address ExternalPointerTable::Get(ExternalPointerHandle handle,
                                  ExternalPointerTag tag) const {
  uint32_t index = HandleToIndex(handle);
  return at(index).GetExternalPointer(tag);
}

void ExternalPointerTable::Set(ExternalPointerHandle handle, Address value,
                               ExternalPointerTag tag) {
  DCHECK_NE(kNullExternalPointerHandle, handle);
  uint32_t index = HandleToIndex(handle);
  at(index).SetExternalPointer(value, tag);
}

Address ExternalPointerTable::Exchange(ExternalPointerHandle handle,
                                       Address value, ExternalPointerTag tag) {
  DCHECK_NE(kNullExternalPointerHandle, handle);
  uint32_t index = HandleToIndex(handle);
  return at(index).ExchangeExternalPointer(value, tag);
}

ExternalPointerHandle ExternalPointerTable::AllocateAndInitializeEntry(
    Space* space, Address initial_value, ExternalPointerTag tag) {
  DCHECK(space->BelongsTo(this));
  uint32_t index = AllocateEntry(space);
  at(index).MakeExternalPointerEntry(initial_value, tag);

  // When we're compacting a space, we're trying to move all entries above a
  // threshold index (the start of the evacuation area) into segments below
  // that threshold. However, if the freelist becomes too short and we start
  // allocating entries inside the area that is supposed to be evacuated, we
  // need to abort compaction. This is not just an optimization but is also
  // required for correctness: during sweeping we might otherwise assume that
  // all entries inside the evacuation area have been moved and that these
  // segments can therefore be deallocated. In particular, this check will also
  // make sure that we abort compaction if we extend the space with a new
  // segment and allocate at least one entry in it (if that segment is located
  // after the threshold, otherwise it is unproblematic).
  uint32_t start_of_evacuation_area =
      space->start_of_evacuation_area_.load(std::memory_order_relaxed);
  if (V8_UNLIKELY(index >= start_of_evacuation_area)) {
    space->AbortCompacting(start_of_evacuation_area);
  }

  return IndexToHandle(index);
}

void ExternalPointerTable::Mark(Space* space, ExternalPointerHandle handle,
                                Address handle_location) {
  DCHECK(space->BelongsTo(this));
  // The null entry is immortal and immutable, so no need to mark it as alive.
  if (handle == kNullExternalPointerHandle) return;

  // The handle_location must contain the given handle. The only exception to
  // this is when the handle is zero, which may mean that it hasn't yet been
  // initialized. However, in that case we'll have already returned from this
  // function above.
  CHECK(handle ==
        base::AsAtomic32::Acquire_Load(
            reinterpret_cast<ExternalPointerHandle*>(handle_location)));

  uint32_t index = HandleToIndex(handle);
  CHECK(space->Contains(index));

  // If the table is being compacted and the entry is inside the evacuation
  // area, then allocate and set up an evacuation entry for it.
  MaybeCreateEvacuationEntry(space, index, handle_location);

  // Even if the entry is marked for evacuation, it still needs to be marked as
  // alive as it may be visited during sweeping before being evacuation.
  at(index).Mark();
}

void ExternalPointerTable::MaybeCreateEvacuationEntry(Space* space,
                                                      uint32_t index,
                                                      Address handle_location) {
  // Check if the entry should be evacuated for table compaction.
  // The current value of the start of the evacuation area is cached in a local
  // variable here as it otherwise may be changed by another marking thread
  // while this method runs, causing non-optimal behaviour (for example, the
  // allocation of an evacuation entry _after_ the entry that is evacuated).
  uint32_t start_of_evacuation_area =
      space->start_of_evacuation_area_.load(std::memory_order_relaxed);
  if (index >= start_of_evacuation_area) {
    DCHECK(space->IsCompacting());
    uint32_t new_index = AllocateEntryBelow(space, start_of_evacuation_area);
    if (new_index) {
      DCHECK_LT(new_index, start_of_evacuation_area);
      CHECK(space->Contains(new_index));
      // Even though the new entry will only be accessed during sweeping, this
      // still needs to be an atomic write as another thread may attempt (and
      // fail) to allocate the same table entry, thereby causing a read from
      // this memory location. Without an atomic store here, TSan would then
      // complain about a data race.
      at(new_index).MakeEvacuationEntry(handle_location);
#ifdef DEBUG
      // Mark the handle as visited in debug builds to detect double
      // initialization of external pointer fields.
      auto handle_ptr = reinterpret_cast<base::Atomic32*>(handle_location);
      auto marked_handle = IndexToHandle(index) | kVisitedHandleMarker;
      base::Relaxed_Store(handle_ptr, marked_handle);
#endif  // DEBUG
    } else {
      // In this case, the application has allocated a sufficiently large
      // number of entries from the freelist so that new entries would now be
      // allocated inside the area that is being compacted. While it would be
      // possible to shrink that area and continue compacting, we probably do
      // not want to put more pressure on the freelist and so instead simply
      // abort compaction here. Entries that have already been visited will
      // still be compacted during Sweep, but there is no guarantee that any
      // blocks at the end of the table will now be completely free.
      space->AbortCompacting(start_of_evacuation_area);
    }
  }
}

bool ExternalPointerTable::IsValidHandle(ExternalPointerHandle handle) const {
#ifdef DEBUG
  handle &= ~kVisitedHandleMarker;
#endif  // DEBUG
  uint32_t index = handle >> kExternalPointerIndexShift;
  return handle == index << kExternalPointerIndexShift;
}

uint32_t ExternalPointerTable::HandleToIndex(
    ExternalPointerHandle handle) const {
  DCHECK(IsValidHandle(handle));
  uint32_t index = handle >> kExternalPointerIndexShift;
#if defined(LEAK_SANITIZER)
  // When LSan is active, we use "fat" entries that also store the raw pointer
  // to that LSan can find live references. However, we do this transparently:
  // we simply multiply the handle by two so that `(handle >> index_shift) * 8`
  // still produces the correct offset of the entry in the table. However, this
  // is not secure as an attacker could reference the raw pointer instead of
  // the encoded pointer in an entry, thereby bypassing the type checks. As
  // such, this mode must only be used in testing environments. Alternatively,
  // all places that access external pointer table entries must be made aware
  // that the entries are 16 bytes large when LSan is active.
  index /= 2;
#endif  // LEAK_SANITIZER
  DCHECK_LE(index, kMaxExternalPointers);
  return index;
}

ExternalPointerHandle ExternalPointerTable::IndexToHandle(
    uint32_t index) const {
  DCHECK_LE(index, kMaxExternalPointers);
  ExternalPointerHandle handle = index << kExternalPointerIndexShift;
#if defined(LEAK_SANITIZER)
  handle *= 2;
#endif  // LEAK_SANITIZER
  return handle;
}

void ExternalPointerTable::Space::StartCompacting(
    uint32_t start_of_evacuation_area) {
  start_of_evacuation_area_.store(start_of_evacuation_area,
                                  std::memory_order_relaxed);
}

void ExternalPointerTable::Space::StopCompacting() {
  start_of_evacuation_area_.store(kNotCompactingMarker,
                                  std::memory_order_relaxed);
}

void ExternalPointerTable::Space::AbortCompacting(
    uint32_t start_of_evacuation_area) {
  uint32_t compaction_aborted_marker =
      start_of_evacuation_area | kCompactionAbortedMarker;
  DCHECK_NE(compaction_aborted_marker, kNotCompactingMarker);
  start_of_evacuation_area_.store(compaction_aborted_marker,
                                  std::memory_order_relaxed);
}

bool ExternalPointerTable::Space::IsCompacting() {
  return start_of_evacuation_area_.load(std::memory_order_relaxed) !=
         kNotCompactingMarker;
}

bool ExternalPointerTable::Space::CompactingWasAborted() {
  auto value = start_of_evacuation_area_.load(std::memory_order_relaxed);
  return (value & kCompactionAbortedMarker) == kCompactionAbortedMarker;
}

}  // namespace internal
}  // namespace v8

#endif  // V8_COMPRESS_POINTERS

#endif  // V8_SANDBOX_EXTERNAL_POINTER_TABLE_INL_H_
