#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "helpers.hpp"
#include "helpers_bit.hpp"
#include "pipeline_multi_thread.hpp"

namespace skel {

struct BitWorkUnit {
    size_t start_idx; // Linear voxel index in the packed volume
    uint32_t length;  // Number of interior voxels to process
};

template <uint32_t VoxelsPerUnit>
constexpr size_t get_total_bit_work_units(uint32_t D, uint32_t H, uint32_t W) {
  static_assert(VoxelsPerUnit > 0);

  const size_t voxels_per_row = W - 2;
  const size_t units_per_row =
      (voxels_per_row + VoxelsPerUnit - 1) / VoxelsPerUnit;

  return size_t(D - 2) * (H - 2) * units_per_row;
}

template <uint32_t VoxelsPerUnit>
inline BitWorkUnit get_bit_work_unit(size_t unit_index, uint32_t D, uint32_t H,
                                     uint32_t W) {
  static_assert(VoxelsPerUnit > 0);

  const size_t voxels_per_row = W - 2;
  const size_t units_per_row =
      (voxels_per_row + VoxelsPerUnit - 1) / VoxelsPerUnit;

  const size_t row_index = unit_index / units_per_row;
  const size_t unit_in_row = unit_index % units_per_row;

  const size_t d = 1 + row_index / (H - 2);
  const size_t h = 1 + row_index % (H - 2);

  const size_t stride_z = size_t(W) * H;

  const size_t row_start = d * stride_z + h * size_t(W) + 1;
  const size_t offset_in_row = unit_in_row * VoxelsPerUnit;

  const size_t remaining = voxels_per_row - offset_in_row;
  const size_t length = std::min<size_t>(remaining, VoxelsPerUnit);

  return {row_start + offset_in_row, static_cast<uint32_t>(length)};
}

void find_simple_point_candidates_packed_parallel(
    std::span<const uint64_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset);

void find_candidates_bit_work_unit(std::span<const uint64_t> img,
                                   const BitWorkUnit &unit,
                                   const std::array<AnchorConfig, 9> &anchors,
                                   int64_t b_offset,
                                   std::vector<uint32_t> &local_candidates);

void find_candidates_bit_work_unit_mutex(
    std::span<const uint64_t> img, const BitWorkUnit &unit,
    const std::array<AnchorConfig, 9> &anchors, int64_t b_offset,
    std::array<uint32_t, 8192> &buffer_candidates,
    size_t &index_candidates_array);

void find_simple_point_candidates_packed_parallel_mutex(
    std::span<const uint64_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset);

void compute_thin_image_multi_threaded_bit(std::span<uint64_t> img,
                                           const uint32_t D, const uint32_t H,
                                           const uint32_t W);
} // namespace skel
