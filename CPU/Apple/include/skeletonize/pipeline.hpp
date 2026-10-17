#pragma once

#include "BS_thread_pool.hpp"
#include "lut.hpp"
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <span>
#include <sys/types.h>
#include <vector>

namespace skel {

static BS::thread_pool pool;

struct Byte4 {
  uint8_t x, y, z, overflow_value;
};

enum class TaskState : uint8_t {
  Working = 0,
  Done_Is_Waiting = 1,
  Zeros = 2,
  Inserted = 3
};

template <size_t Byte4PerUnit> struct WorkUnit {
  uint32_t start_idx;
  uint8_t length;
};

// This fixes the memory order needed for the LUT
struct AnchorConfig {
  int mem_offset; // Precomputed: (dz * stride_z) + (dy * stride_y) - 1
  int bit_x;      // Precomputed base bit
  int bit_y;      // Precomputed base bit + 3
  int bit_z;      // Precomputed base bit + 6
};

// To determine the strides: generally for the Y axis, it would be the width
// and for the z axis it would be width * height.
// I have no stride for x specifically here, I assume 1.
// I am loading 4 bytes at a time so X axis is just -1
constexpr std::array<AnchorConfig, 9>
generate_anchor_configs_old(int stride_y, int stride_z) {
  std::array<AnchorConfig, 9> configs{};
  int idx = 0;

  for (int dz = -1; dz <= 1; ++dz) {
    for (int dy = -1; dy <= 1; ++dy) {
      int mem_offset = (dz * stride_z) + (dy * stride_y) - 1;
      int base_bit = (dz + 1) * 9 + (dy + 1);

      configs[idx++] = {mem_offset, base_bit, base_bit + 3, base_bit + 6};
    }
  }
  return configs;
}

constexpr std::array<AnchorConfig, 9> generate_anchor_configs(int stride_y,
                                                              int stride_z) {
  std::array<AnchorConfig, 9> configs{};
  int idx = 0;

  for (int dz = -1; dz <= 1; ++dz) {
    for (int dy = -1; dy <= 1; ++dy) {
      int mem_offset = (dz * stride_z) + (dy * stride_y) - 1; // Points to dx=-1

      // Cython base index for this Z,Y plane
      int base_cython_idx = (dz + 1) * 9 + (dy + 1) * 3;

      // Bits should be base, base+1, base+2
      configs[idx++] = {
          mem_offset,
          base_cython_idx,     // bit_x corresponds to dx=-1
          base_cython_idx + 1, // bit_y corresponds to dx=0
          base_cython_idx + 2  // bit_z corresponds to dx=1
      };
    }
  }
  return configs;
}

// Get the neighborhood of a 3d volume voxel
uint32_t
get_neighborhood(const std::span<const uint8_t> volume_array, int voxPosition,
                 const std::array<AnchorConfig, 9> &precomputed_configs);
// Returns true if the voxel is an endpoint (exactly 1 neighbor + the center
// voxel)
bool is_endpoint(const uint32_t packed_neighborhood);

// Generate the adjency masks array so that the bfs can just check the supposed
// bits that supposed neighboring the packed_neighborhood.
constexpr std::array<uint32_t, 27> generate_adjacency_masks() {
  std::array<uint32_t, 27> masks{};

  for (int i = 0; i < 27; ++i) {
    // 1. Decode voxel 'i' into 3D coordinates
    int dy1 = (i % 3) - 1;
    int dx1 = ((i / 3) % 3) - 1;
    int dz1 = (i / 9) - 1;

    uint32_t mask = 0;

    // 2. Check against every other voxel 'j'
    for (int j = 0; j < 27; ++j) {
      if (i == j)
        continue; // A voxel is not connected to itself

      // Decode voxel 'j'
      int dy2 = (j % 3) - 1;
      int dx2 = ((j / 3) % 3) - 1;
      int dz2 = (j / 9) - 1;

      int ddy = dy1 - dy2;
      if (ddy < 0)
        ddy = -ddy;
      int ddx = dx1 - dx2;
      if (ddx < 0)
        ddx = -ddx;
      int ddz = dz1 - dz2;
      if (ddz < 0)
        ddz = -ddz;

      // 3. If they touch (26-connectivity), set the bit for 'j'
      if (ddy <= 1 && ddx <= 1 && ddz <= 1) {
        mask |= (1u << j);
      }
    }
    masks[i] = mask;
  }
  return masks;
}

constexpr auto ADJACENCY_MASKS = generate_adjacency_masks();

// This check if a point is simple, if it can be removed without issues.
// BFS rewrite of the Octant method from the Lee94 paper with the bitpacking
// trick.
bool is_simple_point(const uint32_t packed_neighborhood);

// O(1) Lazy Evaluation: Computes the WorkUnit purely via math. Zero heap
// allocation.
template <size_t Byte4PerUnit, uint32_t bytes_per_unit>
inline WorkUnit<Byte4PerUnit> get_work_unit(uint32_t unit_index, uint32_t D,
                                            uint32_t H, uint32_t W) {
  //constexpr uint32_t bytes_per_unit = Byte4PerUnit * 16;
  uint32_t bytes_per_row =
      W - 2; // Valid bytes due to 1-voxel padding on both sides

  // Ceiling division for units per row
  uint32_t units_per_row =
      (bytes_per_row + bytes_per_unit - 1) / bytes_per_unit;

  uint32_t row_index = unit_index / units_per_row;
  uint32_t unit_in_row = unit_index % units_per_row;

  uint32_t d = 1 + (row_index / (H - 2));
  uint32_t h = 1 + (row_index % (H - 2));

  uint32_t stride_y = W;
  uint32_t stride_z = W * H;
  uint32_t row_start = d * stride_z + h * stride_y + 1;

  uint32_t start_byte_in_row = unit_in_row * bytes_per_unit;
  uint32_t remaining_bytes = bytes_per_row - start_byte_in_row;

  // The actual length is either the full unit size, or whatever is left in the
  // row
  uint32_t actual_length =
      (remaining_bytes < bytes_per_unit) ? remaining_bytes : bytes_per_unit;

  return {row_start + start_byte_in_row, static_cast<uint8_t>(actual_length)};
}

template <size_t Byte4PerUnit, uint32_t bytes_per_unit>
void
Thread_local_work(std::span<const uint8_t> img,
                  std::vector<uint32_t> &candidates,
                  const std::array<AnchorConfig, 9> &anchor_configs,
                  const uint32_t D, const uint32_t H, const uint32_t W,
                  const int64_t b_offset, std::atomic<uint32_t> &atomic_counter/* uint32_t id*/,
                  std::vector<TaskState>& TaskStates) {
  thread_local std::array<uint32_t, bytes_per_unit> buffer_candidates;
  uint32_t id = atomic_counter.fetch_add(1);
  uint8_t index_candidates_array = 0;
  WorkUnit<Byte4PerUnit> unit = get_work_unit<Byte4PerUnit, bytes_per_unit>(id, D, H, W);
  uint32_t idx = unit.start_idx;
  uint8_t len = unit.length;

  // 2. WIDE LOAD: Compiler optimizes this into a single SIMD register load
  std::array<uint8_t, bytes_per_unit> main_bytes;
  std::array<uint8_t, bytes_per_unit> offset_bytes;

  std::memcpy(main_bytes.data(), &img[idx], bytes_per_unit);
  std::memcpy(offset_bytes.data(), &img[idx + b_offset], bytes_per_unit);

  uint8_t i = 0;
  for (; i + 3 < len; i += 4) {

    // Because main_bytes is a local std::array, the compiler optimizes
    // these 4-byte memcpys into zero-cost register reads.
    uint32_t main_32;
    uint32_t offset_32;
    std::memcpy(&main_32, &main_bytes[i], 4);
    std::memcpy(&offset_32, &offset_bytes[i], 4);

    if (main_32 == 0)
      continue;
    if (offset_32 == 0x01010101)
      continue;

    if (main_bytes[i] == 1 && offset_bytes[i] == 0) {
      uint32_t packed = get_neighborhood(img, idx + i, anchor_configs);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i + 0;
        index_candidates_array++;
      }
    }
    if (main_bytes[i + 1] == 1 && offset_bytes[i + 1] == 0) {
      uint32_t packed = get_neighborhood(img, idx + i + 1, anchor_configs);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i + 1;
        index_candidates_array++;
      }
    }
    if (main_bytes[i + 2] == 1 && offset_bytes[i + 2] == 0) {
      uint32_t packed = get_neighborhood(img, idx + i + 2, anchor_configs);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i + 2;
        index_candidates_array++;
      }
    }
    if (main_bytes[i + 3] == 1 && offset_bytes[i + 3] == 0) {
      uint32_t packed = get_neighborhood(img, idx + i + 3, anchor_configs);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i + 3;
        index_candidates_array++;
      }
    }
  }

  // 4. TAIL LOOP: Handle the remainder (len % 4)
  for (; i < len; ++i) {
    if (main_bytes[i] == 1 && offset_bytes[i] == 0) {
      uint32_t packed = get_neighborhood(img, idx + i, anchor_configs);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i;
        index_candidates_array++;
      }
    }
  }
  std::span<uint32_t> span_array(buffer_candidates.data(),
                                 index_candidates_array);
  std::atomic_ref<TaskState> my_flag(TaskStates[id]);

  if (index_candidates_array == 0) {
      if (id == 0)
          my_flag.store(TaskState::Inserted, std::memory_order_release);
      else
          my_flag.store(TaskState::Zeros, std::memory_order_release);
      return;
  }
  my_flag.store(TaskState::Done_Is_Waiting, std::memory_order_release);
  if (id > 0)
  {
      uint32_t back_counter = 1;
      uint8_t spin_wait = 0;
      while (true)
      {
          TaskState prev_state = std::atomic_ref<TaskState>(TaskStates[id - back_counter])
                                               .load(std::memory_order_relaxed);
          if (prev_state == TaskState::Inserted)
              break;
          else if (prev_state == TaskState::Zeros)
          {
              back_counter++;
              continue;

          }
      }
  }
  my_flag.store(TaskState::Inserted, std::memory_order_release);
  return;
}

template <size_t Byte4PerUnit = 4, uint32_t bytes_per_unit>
void find_simple_point_candidates_Iteratite_preParallel_Byte4(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset) {
  uint32_t bytes_per_row = W - 2;
  uint32_t units_per_row =
      (bytes_per_row + bytes_per_unit - 1) / bytes_per_unit;
  uint32_t total_units = (D - 2) * (H - 2) * units_per_row;

  std::atomic<uint32_t> atomic_counter{0};
  std::vector<TaskState> TaskStates(total_units, TaskState::Working);

  auto loop_future = pool.submit_loop(0, total_units, [&](auto) {
      Thread_local_work<Byte4PerUnit, bytes_per_unit>(
          img, candidates, anchor_configs, D, H, W, b_offset, atomic_counter,
          TaskStates);
  }
  );
  loop_future.wait();
  pool.wait();
  std::sort(candidates.begin(), candidates.end());
}

template <size_t Byte4PerUnit = 8> // 1 = 4 bytes, 4 = 16 bytes, etc.
void find_simple_point_candidates_Iterative_Byte4(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset) {

  uint32_t stride_y = W;
  uint32_t stride_z = W * H;
  constexpr uint32_t bytes_per_unit = Byte4PerUnit * 4;

  // Standard for loops guarantee zero overhead and perfect compiler
  // optimization
  for (uint32_t d = 1; d < D - 1; ++d) {
    uint32_t d_offset = d * stride_z;

    for (uint32_t h = 1; h < H - 1; ++h) {
      uint32_t row_start = d_offset + (h * stride_y) + 1;
      uint32_t bytes_per_row = W - 2;

      // DIVISION HAPPENS ONCE PER ROW, NOT PER CHUNK!
      uint32_t units_per_row = bytes_per_row / bytes_per_unit;
      uint32_t bytes_processed = units_per_row * bytes_per_unit;
      uint32_t tail_bytes = bytes_per_row - bytes_processed;

      // ── 1. PROCESS FULL UNITS ──
      for (uint32_t i = 0; i < units_per_row; ++i) {
        uint32_t unit_idx = row_start + i * bytes_per_unit;

        // Wide load: Compiler optimizes this to a single SIMD register load
        std::array<uint8_t, bytes_per_unit> main_bytes;
        std::array<uint8_t, bytes_per_unit> offset_bytes;
        std::memcpy(main_bytes.data(), &img[unit_idx], bytes_per_unit);
        std::memcpy(offset_bytes.data(), &img[unit_idx + b_offset],
                    bytes_per_unit);

        // Unrolled processing by 4 bytes
        uint32_t j = 0;
        for (; j + 3 < bytes_per_unit; j += 4) {
          uint32_t main_32, offset_32;
          std::memcpy(&main_32, &main_bytes[j], 4);
          std::memcpy(&offset_32, &offset_bytes[j], 4);

          // FAST 4-BYTE SKIP
          if (main_32 == 0)
            continue;
          if (offset_32 == 0x01010101)
            continue;

          // Byte 0
          if (main_bytes[j] == 1 && offset_bytes[j] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j);
          }
          // Byte 1
          if (main_bytes[j + 1] == 1 && offset_bytes[j + 1] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j + 1, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j + 1);
          }
          // Byte 2
          if (main_bytes[j + 2] == 1 && offset_bytes[j + 2] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j + 2, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j + 2);
          }
          // Byte 3
          if (main_bytes[j + 3] == 1 && offset_bytes[j + 3] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j + 3, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j + 3);
          }
        }
      }

      // ── 2. PROCESS TAIL (at most bytes_per_unit - 1 bytes) ──
      uint32_t tail_start = row_start + bytes_processed;
      for (uint32_t i = 0; i < tail_bytes; ++i) {
        uint32_t idx = tail_start + i;
        if (img[idx] == 1 && img[idx + b_offset] == 0) {
          uint32_t packed = get_neighborhood(img, idx, anchor_configs);
          if (!is_endpoint(packed) && is_euler_invariant(packed) &&
              is_simple_point(packed)) {
            candidates.emplace_back(idx);
          }
        }
      }
    }
  }
}

void find_simple_point_candidates_Byte4_prepara(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset);

void find_simple_point_candidates(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset);

// Version of simple point detetion with loading more data at once
void find_simple_point_candidates_Byte4(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset);

int sequential_recheck(std::span<uint8_t> img,
                       std::vector<uint32_t> &candidates,
                       const std::array<AnchorConfig, 9> &anchor_configs);

// Lee94 skeleton, img is supposed to be padded before.
void compute_thin_image(std::span<uint8_t> img, const uint32_t D,
                        const uint32_t H, const uint32_t W);
} // namespace skel
