// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/v8-isolate.h"
#include "src/flags/flags.h"
#include "src/heap/heap.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8 {

TEST(ResourceConstraints, ConfigureDefaultsFromHeapSizeSmall) {
  const size_t MB = static_cast<size_t>(i::MB);
  v8::ResourceConstraints constraints;
  constraints.ConfigureDefaultsFromHeapSize(1 * MB, 1 * MB);
  ASSERT_EQ(i::Heap::MinOldGenerationSize(),
            constraints.max_old_generation_size_in_bytes());
  ASSERT_EQ(i::Heap::MinYoungGenerationSize(),
            constraints.max_young_generation_size_in_bytes());
  ASSERT_EQ(0u, constraints.initial_old_generation_size_in_bytes());
  ASSERT_EQ(0u, constraints.initial_young_generation_size_in_bytes());
}

TEST(ResourceConstraints, ConfigureDefaultsFromHeapSizeLarge) {
  const size_t KB = static_cast<size_t>(i::KB);
  const size_t MB = static_cast<size_t>(i::MB);
  const size_t GB = static_cast<size_t>(i::GB);
  const size_t pm = i::Heap::kPointerMultiplier;
  v8::ResourceConstraints constraints;
  constraints.ConfigureDefaultsFromHeapSize(50u * MB, 3u * GB);
  // Check that for large heap sizes max semi space size is set to the maximum
  // supported capacity (i.e. 8MB with pointer compression and 16MB without;
  // MinorMC supports double capacity).
  ASSERT_EQ(internal::v8_flags.minor_mc ? 2 * i::Heap::DefaultMaxSemiSpaceSize()
                                        : 3 * 8 * pm * MB,
            constraints.max_young_generation_size_in_bytes());
  ASSERT_EQ(3u * GB - (internal::v8_flags.minor_mc
                           ? 2 * i::Heap::DefaultMaxSemiSpaceSize()
                           : 3 * 8 * pm * MB),
            constraints.max_old_generation_size_in_bytes());
  // Check that for small initial heap sizes initial semi space size is set to
  // the minimum supported capacity (i.e. 1MB with pointer compression and 512KB
  // without).
  ASSERT_EQ((internal::v8_flags.minor_mc ? 2 : 3) * 512 * pm * KB,
            constraints.initial_young_generation_size_in_bytes());
  ASSERT_EQ(50u * MB - (internal::v8_flags.minor_mc ? 2 : 3) * 512 * pm * KB,
            constraints.initial_old_generation_size_in_bytes());
}

TEST(ResourceConstraints, ConfigureDefaults) {
  const size_t MB = static_cast<size_t>(i::MB);
  const size_t GB = static_cast<size_t>(i::GB);
  const size_t pm = i::Heap::kPointerMultiplier;
  const size_t hlm = i::Heap::kHeapLimitMultiplier;
  v8::ResourceConstraints constraints;
  constraints.ConfigureDefaults(2u * GB, 0u);
  ASSERT_EQ(512u * hlm * MB, constraints.max_old_generation_size_in_bytes());
  ASSERT_EQ(0u, constraints.initial_old_generation_size_in_bytes());
  ASSERT_EQ(internal::v8_flags.minor_mc ? 2 * i::Heap::DefaultMaxSemiSpaceSize()
                                        : 3 * 4 * pm * MB,
            constraints.max_young_generation_size_in_bytes());
  ASSERT_EQ(0u, constraints.initial_young_generation_size_in_bytes());
}

}  // namespace v8
