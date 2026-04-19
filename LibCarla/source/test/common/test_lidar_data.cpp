// Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "test.h"

#include <carla/sensor/data/LidarData.h>
#include <carla/sensor/data/SemanticLidarData.h>

#include <vector>

using carla::sensor::data::LidarData;
using carla::sensor::data::SemanticLidarData;

// Bundle 1 of the VRAM ladder relaxes the boundary check in
// {Lidar,SemanticLidar}Data::ResetMemory from strict `>` to `>=` so a caller
// that supplies one slot per channel does not trip a Debug assertion. These
// tests exercise the boundary that was previously rejected. They are no-ops
// under NDEBUG (DEBUG_ASSERT compiles away); the suite is built and run as
// Debug specifically so the boundary case actually executes the assertion.

TEST(LidarData, reset_memory_accepts_one_entry_per_channel) {
  constexpr uint32_t channel_count{4u};
  LidarData data{channel_count};
  std::vector<uint32_t> points_per_channel(channel_count, 1u);
  data.ResetMemory(points_per_channel);
  EXPECT_EQ(data.GetChannelCount(), channel_count);
}

TEST(LidarData, reset_memory_accepts_fewer_entries_than_channels) {
  constexpr uint32_t channel_count{8u};
  LidarData data{channel_count};
  std::vector<uint32_t> points_per_channel(channel_count - 2u, 3u);
  data.ResetMemory(points_per_channel);
  EXPECT_EQ(data.GetChannelCount(), channel_count);
}

TEST(SemanticLidarData, reset_memory_accepts_one_entry_per_channel) {
  constexpr uint32_t channel_count{4u};
  SemanticLidarData data{channel_count};
  std::vector<uint32_t> points_per_channel(channel_count, 1u);
  data.ResetMemory(points_per_channel);
  EXPECT_EQ(data.GetChannelCount(), channel_count);
}

TEST(SemanticLidarData, reset_memory_accepts_fewer_entries_than_channels) {
  constexpr uint32_t channel_count{8u};
  SemanticLidarData data{channel_count};
  std::vector<uint32_t> points_per_channel(channel_count - 2u, 3u);
  data.ResetMemory(points_per_channel);
  EXPECT_EQ(data.GetChannelCount(), channel_count);
}
