// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sandbox/external-pointer-table.h"

#include "src/execution/isolate.h"
#include "src/logging/counters.h"
#include "src/sandbox/external-pointer-table-inl.h"

#ifdef V8_COMPRESS_POINTERS

namespace v8 {
namespace internal {

void ExternalPointerTable::Initialize() { InitializeTable(); }

void ExternalPointerTable::TearDown() { TearDownTable(); }

void ExternalPointerTable::Initialize(Space* space) { InitializeSpace(space); }

void ExternalPointerTable::TearDown(Space* space) { TearDownSpace(space); }

uint32_t ExternalPointerTable::SweepAndCompact(Space* space,
                                               Counters* counters) {
  DCHECK(space->BelongsTo(this));

  // Lock the space. Technically this is not necessary since no other thread can
  // allocate entries at this point, but some of the methods we call on the
  // space assert that the lock is held.
  base::MutexGuard guard(&space->mutex_);

  // There must not be any entry allocations while the table is being swept as
  // that would not be safe. Set the freelist to this special marker value to
  // easily catch any violation of this requirement.
  space->freelist_head_.store(kEntryAllocationIsForbiddenMarker,
                              std::memory_order_relaxed);

  // When compacting, we can compute the number of unused segments at the end of
  // the table and skip those during sweeping.
  uint32_t start_of_evacuation_area =
      space->start_of_evacuation_area_.load(std::memory_order_relaxed);
  bool evacuation_was_successful = false;
  bool was_compacting = space->IsCompacting();
  if (space->IsCompacting()) {
    TableCompactionOutcome outcome;
    if (space->CompactingWasAborted()) {
      // Compaction was aborted during marking because the freelist grew to
      // short. In this case, it is not guaranteed that any segments will now
      // be completely free.
      outcome = TableCompactionOutcome::kAborted;
      // Extract the original start_of_evacuation_area value so that the
      // DCHECKs below and in ResolveEvacuationEntryDuringSweeping work.
      start_of_evacuation_area &= ~Space::kCompactionAbortedMarker;
    } else {
      // Entry evacuation was successful so all segments inside the evacuation
      // area are now guaranteed to be free and so can be deallocated.
      outcome = TableCompactionOutcome::kSuccess;
      evacuation_was_successful = true;
    }
    DCHECK(IsAligned(start_of_evacuation_area, kEntriesPerSegment));

    space->StopCompacting();

    counters->external_pointer_table_compaction_outcome()->AddSample(
        static_cast<int>(outcome));
  }

  // Sweep top to bottom and rebuild the freelist from newly dead and
  // previously freed entries while also clearing the marking bit on live
  // entries and resolving evacuation entries table when compacting the table.
  // This way, the freelist ends up sorted by index which already makes the
  // table somewhat self-compacting and is required for the compaction
  // algorithm so that evacuated entries are evacuated to the start of a space.
  // This method must run either on the mutator thread or while the mutator is
  // stopped.
  uint32_t current_freelist_head = 0;
  uint32_t current_freelist_length = 0;
  std::vector<Segment> segments_to_deallocate;
  for (auto segment : base::Reversed(space->segments_)) {
    // If we evacuated all live entries in this segment then we can skip it
    // here and directly deallocate it after this loop.
    if (evacuation_was_successful &&
        segment.first_entry() >= start_of_evacuation_area) {
      segments_to_deallocate.push_back(segment);
      continue;
    }

    // Remember the state of the freelist before this segment in case this
    // segment turns out to be completely empty and we deallocate it.
    uint32_t previous_freelist_head = current_freelist_head;
    uint32_t previous_freelist_length = current_freelist_length;

    // Process every entry in this segment, again going top to bottom.
    for (uint32_t i = segment.last_entry(); i >= segment.first_entry(); i--) {
      auto payload = at(i).GetRawPayload();
      if (payload.ContainsEvacuationEntry()) {
        CHECK(was_compacting);
        // Resolve the evacuation entry: take the pointer to the handle from the
        // evacuation entry, copy the entry to its new location, and finally
        // update the handle to point to the new entry.
        // While we now know that the entry being evacuated is free, we don't
        // add it to (the start of) the freelist because that would immediately
        // cause new fragmentation when the next entry is allocated. Instead, we
        // assume that the segments out of which entries are evacuated will all
        // be decommitted anyway after this loop, which is usually the case
        // unless compaction was already aborted during marking.
        ExternalPointerHandle* handle_location =
            reinterpret_cast<ExternalPointerHandle*>(
                payload.ExtractEvacuationEntryHandleLocation());
        ResolveEvacuationEntryDuringSweeping(i, handle_location,
                                             start_of_evacuation_area);
      } else if (!payload.HasMarkBitSet()) {
        at(i).MakeFreelistEntry(current_freelist_head);
        current_freelist_head = i;
        current_freelist_length++;
      } else {
        auto new_payload = payload;
        new_payload.ClearMarkBit();
        at(i).SetRawPayload(new_payload);
      }
    }

    // If a segment is completely empty, free it.
    uint32_t free_entries = current_freelist_length - previous_freelist_length;
    bool segment_is_empty = free_entries == kEntriesPerSegment;
    if (segment_is_empty) {
      segments_to_deallocate.push_back(segment);
      // Restore the state of the freelist before this segment.
      current_freelist_head = previous_freelist_head;
      current_freelist_length = previous_freelist_length;
    }
  }

  // We cannot deallocate the segments during the above loop, so do it now.
  for (auto segment : segments_to_deallocate) {
    FreeTableSegment(segment);
    space->segments_.erase(segment);
  }

  FreelistHead new_freelist(current_freelist_head, current_freelist_length);
  space->freelist_head_.store(new_freelist, std::memory_order_release);
  DCHECK_EQ(space->freelist_length(), current_freelist_length);

  uint32_t num_live_entries = space->capacity() - current_freelist_length;
  counters->external_pointers_count()->AddSample(num_live_entries);
  return num_live_entries;
}

void ExternalPointerTable::Space::StartCompactingIfNeeded() {
  // Take the lock so that we can be sure that no other thread modifies the
  // segments set concurrently.
  base::MutexGuard guard(&mutex_);

  // This method may be executed while other threads allocate entries from the
  // freelist. In that case, this method may use incorrect data to determine if
  // table compaction is necessary. That's fine however since in the worst
  // case, compaction will simply be aborted right away if the freelist became
  // too small.
  uint32_t num_free_entries = freelist_length();
  uint32_t num_total_entries = capacity();

  // Current (somewhat arbitrary) heuristic: need compacting if the space is
  // more than 1MB in size, is at least 10% empty, and if at least one segment
  // can be freed after successful compaction.
  double free_ratio = static_cast<double>(num_free_entries) /
                      static_cast<double>(num_total_entries);
  uint32_t num_segments_to_evacuate =
      (num_free_entries / 2) / kEntriesPerSegment;

  uint32_t space_size = num_total_entries * kEntrySize;
  bool should_compact = (space_size >= 1 * MB) && (free_ratio >= 0.10) &&
                        (num_segments_to_evacuate >= 1);

  if (should_compact) {
    // If we're compacting, attempt to free up the last N segments so that they
    // can be decommitted afterwards.
    Segment first_segment_to_evacuate =
        *std::prev(segments_.end(), num_segments_to_evacuate);
    uint32_t start_of_evacuation_area = first_segment_to_evacuate.first_entry();
    StartCompacting(start_of_evacuation_area);
  }
}

void ExternalPointerTable::ResolveEvacuationEntryDuringSweeping(
    uint32_t new_index, ExternalPointerHandle* handle_location,
    uint32_t start_of_evacuation_area) {
  ExternalPointerHandle old_handle = *handle_location;
  uint32_t old_index = HandleToIndex(old_handle);
  ExternalPointerHandle new_handle = IndexToHandle(new_index);

  // For the compaction algorithm to work optimally, double initialization
  // of entries is forbidden, see below. This DCHECK can detect double
  // initialization of external pointer fields in debug builds by checking
  // that the old_handle was visited during marking.
  // There's no need to clear the bit from the handle as the handle will be
  // replaced by a new, unmarked handle.
  DCHECK(HandleWasVisitedDuringMarking(old_handle));

  // The following DCHECKs assert that the compaction algorithm works
  // correctly: it always moves an entry from the evacuation area to the
  // front of the table. One reason this invariant can be broken is if an
  // external pointer slot is re-initialized, in which case the old_handle
  // may now also point before the evacuation area. For that reason,
  // re-initialization of external pointer slots is forbidden.
  DCHECK_GE(old_index, start_of_evacuation_area);
  DCHECK_LT(new_index, start_of_evacuation_area);

  auto& new_entry = at(new_index);
  at(old_index).UnmarkAndMigrateInto(new_entry);
  *handle_location = new_handle;
}

}  // namespace internal
}  // namespace v8

#endif  // V8_COMPRESS_POINTERS
