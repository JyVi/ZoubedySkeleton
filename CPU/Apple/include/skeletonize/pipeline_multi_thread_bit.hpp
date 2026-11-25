#pragma once

#include <algorithm>
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
  uint64_t start_idx; // Linear voxel index in the packed volume
  uint64_t length;    // Number of interior voxels to process
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
__attribute__((noinline)) inline BitWorkUnit
get_bit_work_unit(size_t unit_index, uint32_t D, uint32_t H, uint32_t W) {
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

template <uint32_t VPU> struct BitUnitGrid {
  uint32_t upr;                  // units per row, padded to power of two
  uint32_t shift;                // log2(upr)
  uint32_t vpr;                  // W - 2
  size_t total_units;            // rows * upr  (padded!)
  std::vector<size_t> row_start; // (D-2)*(H-2) entries
};

template <uint32_t VPU>
BitUnitGrid<VPU> make_bit_unit_grid(uint32_t D, uint32_t H, uint32_t W) {
  BitUnitGrid<VPU> g;
  g.vpr = W - 2;
  const size_t upr_real = (size_t(g.vpr) + VPU - 1) / VPU;
  g.upr = uint32_t(std::bit_ceil(upr_real)); // C++20 <bit>; bit_ceil(0)==1
  g.shift = uint32_t(std::countr_zero(g.upr));

  const size_t stride_z = size_t(W) * H;
  g.row_start.resize(size_t(D - 2) * (H - 2));
  size_t r = 0;
  for (uint32_t d = 1; d < D - 1; ++d)
    for (uint32_t h = 1; h < H - 1; ++h)
      g.row_start[r++] = d * stride_z + h * size_t(W) + 1;

  g.total_units = g.row_start.size() * g.upr;
  return g;
}

template <uint32_t VPU>
inline BitWorkUnit get_bit_work_unit(size_t id, const BitUnitGrid<VPU> &g) {
  const size_t row = id >> g.shift;
  const size_t off = (id & (g.upr - 1)) * VPU; // compiler turns into shl
  if (off >= g.vpr)
    return {0, 0}; // padding unit
  const uint32_t len = uint32_t(std::min<size_t>(g.vpr - off, VPU));
  return {g.row_start[row] + off, len};
}

void find_simple_point_candidates_packed_parallel(
    std::span<const uint64_t> img, std::vector<uint64_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset);

void find_candidates_bit_work_unit(std::span<const uint64_t> img,
                                   const BitWorkUnit &unit,
                                   const std::array<AnchorConfig, 9> &anchors,
                                   int64_t b_offset,
                                   std::vector<uint64_t> &local_candidates);

void find_candidates_bit_work_unit_mutex(
    std::span<const uint64_t> img, const BitWorkUnit &unit,
    const std::array<AnchorConfig, 9> &anchors, int64_t b_offset,
    std::array<uint64_t, 256> &buffer_candidates,
    uint64_t &index_candidates_array);

void mutex_write(WriteBuffer &local_write_buffer,
                 std::span<uint64_t> span_array,
                 uint64_t &index_candidates_array, std::mutex &candidates_mutex,
                 std::vector<uint64_t> &candidates);

void find_candidates_bit_work_unit_mutex_new_scheduler(
    std::span<const uint64_t> img, const BitWorkUnit &unit,
    const std::array<AnchorConfig, 9> &anchors, int64_t b_offset,
    std::array<uint64_t, 256> &buffer_candidates,
    uint64_t &index_candidates_array);

void find_simple_point_candidates_packed_parallel_mutex(
    std::span<const uint64_t> img, std::vector<uint64_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset);

void compute_thin_image_multi_threaded_bit(std::span<uint64_t> img,
                                           const uint32_t D, const uint32_t H,
                                           const uint32_t W);

void find_candidates_bit_work_unit_mutex_perf(
    std::span<const uint64_t> img, const BitWorkUnit &unit,
    const std::array<AnchorConfig, 9> &anchors, int64_t b_offset,
    std::array<uint64_t, 8192> &buffer_candidates,
    uint64_t &index_candidates_array);

template <uint32_t VoxelsPerUnit>
void find_simple_point_candidates_packed_parallel_mutex(
    std::span<const uint64_t> img, std::vector<uint64_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs,
    const BitUnitGrid<VoxelsPerUnit> &grid, int64_t b_offset) {
  constexpr size_t LocalCandidateCapacity = VoxelsPerUnit;
  const size_t total_units = grid.total_units;

  candidates.clear();
  if (total_units == 0)
    return;

  std::mutex candidates_mutex;
  std::vector<WriteBuffer> writebuffer(pool.get_thread_count());

  auto loop_future =
      pool.submit_loop(size_t{0}, total_units, [&](size_t unit_id) {
        const BitWorkUnit unit =
            get_bit_work_unit<VoxelsPerUnit>(unit_id, grid);
        if (unit.length == 0)
          return;

        const size_t worker_id = BS::this_thread::get_index().value();
        WriteBuffer &local_write_buffer = writebuffer[worker_id];
        thread_local std::array<uint64_t, LocalCandidateCapacity>
            buffer_candidates;
        uint64_t index_candidates_array = 0;

        find_candidates_bit_work_unit_mutex(img, unit, anchor_configs, b_offset,
                                            buffer_candidates,
                                            index_candidates_array);
        if (index_candidates_array == 0)
          return;

        std::span<uint64_t> span_array(buffer_candidates.data(),
                                       index_candidates_array);
        mutex_write(local_write_buffer, span_array, index_candidates_array,
                    candidates_mutex, candidates);
      });

  loop_future.wait();

  auto active_buffers = writebuffer | std::views::filter([](const auto &buf) {
                          return buf.size > 0;
                        });
  std::ranges::for_each(active_buffers, [&](auto &buf) {
    candidates.insert(candidates.end(), buf.data.begin(),
                      buf.data.begin() + buf.size);
    buf.size = 0;
  });
}

} // namespace skel
