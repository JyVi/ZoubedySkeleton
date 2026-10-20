#pragma once

#include "BS_thread_pool.hpp"
#include "helpers.hpp"
#include "lut.hpp"
// #include "skeletonize/perfetto_tracing.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <sys/types.h>
#include <vector>

namespace skel {

static BS::thread_pool pool;

enum class TaskState : uint8_t {
  Working = 0,
  Done_Is_Waiting = 1,
  Zeros = 2,
  Inserted = 3
};

template <size_t Byte4PerUnit> struct WorkUnit {
  uint32_t start_idx;
  uint16_t length;
};

constexpr size_t WRITE_BUFFER_CAPACITY = 8192;
struct WriteBuffer {
  std::array<uint32_t, WRITE_BUFFER_CAPACITY> data;
  uint16_t size = 0;
};

// O(1) Lazy Evaluation: Computes the WorkUnit purely via math. Zero heap
// allocation.
template <size_t Byte4PerUnit, uint32_t bytes_per_unit>
inline WorkUnit<Byte4PerUnit> get_work_unit(uint32_t unit_index, uint32_t D,
                                            uint32_t H, uint32_t W) {
  // Valid bytes due to 1-voxel padding on both sides
  uint32_t bytes_per_row = W - 2;

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

  return {row_start + start_byte_in_row, static_cast<uint16_t>(actual_length)};
}

// this is a decoupled lookback implementation.
template <size_t Byte4PerUnit, uint32_t bytes_per_unit>
void Thread_local_work(std::span<const uint8_t> img,
                       std::vector<uint32_t> &candidates,
                       const std::array<AnchorConfig, 9> &anchor_configs,
                       const uint32_t D, const uint32_t H, const uint32_t W,
                       const int64_t b_offset,
                       std::atomic<uint32_t> &atomic_counter,
                       std::vector<TaskState> &TaskStates,
                       std::mutex &candidates_mutex) {
  thread_local std::array<uint32_t, bytes_per_unit> buffer_candidates;
  uint32_t id = atomic_counter.fetch_add(1);
  uint8_t index_candidates_array = 0;
  WorkUnit<Byte4PerUnit> unit =
      get_work_unit<Byte4PerUnit, bytes_per_unit>(id, D, H, W);
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
  if (id > 0) {
    uint32_t back_counter = 1;
    uint8_t spin_wait = 0;
    while (true) {
      TaskState prev_state =
          std::atomic_ref<TaskState>(TaskStates[id - back_counter])
              .load(std::memory_order_relaxed);
      if (prev_state == TaskState::Inserted)
        break;
      else if (prev_state == TaskState::Zeros) {
        back_counter++;
        continue;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(candidates_mutex);

    candidates.insert(candidates.end(), span_array.begin(), span_array.end());
  }
  // candidates.insert(candidates.end(), span_array.begin(), span_array.end());
  my_flag.store(TaskState::Inserted, std::memory_order_release);
  return;
}

// Local work completely non decoupled.
template <size_t Byte4PerUnit, uint32_t bytes_per_unit>
void Thread_local_work_no_decoupled(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset, uint32_t id,
    std::mutex &candidates_mutex) {
  std::array<uint32_t, bytes_per_unit> buffer_candidates;
  uint8_t index_candidates_array = 0;
  WorkUnit<Byte4PerUnit> unit =
      get_work_unit<Byte4PerUnit, bytes_per_unit>(id, D, H, W);
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

  if (index_candidates_array == 0)
    return;
  std::span<uint32_t> span_array(buffer_candidates.data(),
                                 index_candidates_array);
  {
    std::lock_guard<std::mutex> lock(candidates_mutex);

    candidates.insert(candidates.end(), span_array.begin(), span_array.end());
  }
  return;
}

// Local work completely non decoupled.
template <size_t Byte4PerUnit, uint32_t bytes_per_unit>
void Thread_local_work_no_decoupled_writebuffer(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset, uint32_t id,
    std::mutex &candidates_mutex, std::vector<WriteBuffer> &writebuffer,
    uint32_t stride_y, uint32_t stride_z) {

  // get the id for the fetch of the local write buffer to global
  const auto worker_id = BS::this_thread::get_index();
  WriteBuffer &local_write_buffer = writebuffer[worker_id.value()];
  std::array<uint32_t, bytes_per_unit> buffer_candidates;
  size_t index_candidates_array = 0;
  WorkUnit<Byte4PerUnit> unit =
      get_work_unit<Byte4PerUnit, bytes_per_unit>(id, D, H, W);
  uint32_t idx = unit.start_idx;
  const size_t len = unit.length;

  // 2. WIDE LOAD: Compiler optimizes this into a single SIMD register load
  std::array<uint8_t, bytes_per_unit> main_bytes;
  std::array<uint8_t, bytes_per_unit> offset_bytes;

  std::memcpy(main_bytes.data(), &img[idx], bytes_per_unit);
  std::memcpy(offset_bytes.data(), &img[idx + b_offset], bytes_per_unit);

  size_t i = 0;
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
      // uint32_t packed = get_neighborhood(img, idx + i, anchor_configs);
      uint32_t packed = get_neighborhood_fast(img, idx + i, stride_y, stride_z);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i + 0;
        index_candidates_array++;
      }
    }
    if (main_bytes[i + 1] == 1 && offset_bytes[i + 1] == 0) {
      // uint32_t packed = get_neighborhood(img, idx + i + 1, anchor_configs);
      uint32_t packed =
          get_neighborhood_fast(img, idx + i + 1, stride_y, stride_z);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i + 1;
        index_candidates_array++;
      }
    }
    if (main_bytes[i + 2] == 1 && offset_bytes[i + 2] == 0) {
      // uint32_t packed = get_neighborhood(img, idx + i + 2, anchor_configs);
      uint32_t packed =
          get_neighborhood_fast(img, idx + i + 2, stride_y, stride_z);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i + 2;
        index_candidates_array++;
      }
    }
    if (main_bytes[i + 3] == 1 && offset_bytes[i + 3] == 0) {
      // uint32_t packed = get_neighborhood(img, idx + i + 3, anchor_configs);
      uint32_t packed =
          get_neighborhood_fast(img, idx + i + 3, stride_y, stride_z);
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
      // uint32_t packed = get_neighborhood(img, idx + i, anchor_configs);
      uint32_t packed = get_neighborhood_fast(img, idx + i, stride_y, stride_z);
      if (!is_endpoint(packed) && is_euler_invariant(packed) &&
          is_simple_point(packed)) {
        buffer_candidates[index_candidates_array] = idx + i;
        index_candidates_array++;
      }
    }
  }
  if (index_candidates_array == 0)
    return;
  std::span<uint32_t> span_array(buffer_candidates.data(),
                                 index_candidates_array);
  // global write
  if ((local_write_buffer.size + index_candidates_array) >=
      WRITE_BUFFER_CAPACITY) {
    {
      std::lock_guard<std::mutex> lock(candidates_mutex);
      candidates.insert(candidates.end(), local_write_buffer.data.begin(),
                        local_write_buffer.data.begin() +
                            local_write_buffer.size);
      candidates.insert(candidates.end(), span_array.begin(), span_array.end());
    }
    local_write_buffer.size = 0;
  } else {
    std::copy(span_array.begin(), span_array.end(),
              local_write_buffer.data.begin() + local_write_buffer.size);
    local_write_buffer.size += index_candidates_array;
  }
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

  std::mutex candidates_mutex;

  /*
  auto loop_future = pool.submit_loop(0, total_units, [&](auto) {
      Thread_local_work<Byte4PerUnit, bytes_per_unit>(
          img, candidates, anchor_configs, D, H, W, b_offset, atomic_counter,
          TaskStates, candidates_mutex);
  }
  );
  */
  auto loop_future = pool.submit_loop(0, total_units, [&](uint32_t id) {
    Thread_local_work_no_decoupled<Byte4PerUnit, bytes_per_unit>(
        img, candidates, anchor_configs, D, H, W, b_offset, id,
        candidates_mutex);
  });
  loop_future.wait();
  pool.wait();
}

template <size_t Byte4PerUnit = 4, uint32_t bytes_per_unit>
void find_simple_point_candidates_Parallel_Byte4(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset) {
  uint32_t bytes_per_row = W - 2;
  uint32_t units_per_row =
      (bytes_per_row + bytes_per_unit - 1) / bytes_per_unit;
  uint32_t total_units = (D - 2) * (H - 2) * units_per_row;
  uint32_t stride_y = W;
  uint32_t stride_z = W * H;

  std::mutex candidates_mutex;
  std::vector<WriteBuffer> WriteBuffers(pool.get_thread_count());

  std::atomic<uint64_t> callback_count{0};

  auto loop_future = pool.submit_loop(0, total_units, [&](uint32_t id) {
    Thread_local_work_no_decoupled_writebuffer<Byte4PerUnit, bytes_per_unit>(
        img, candidates, anchor_configs, D, H, W, b_offset, id,
        candidates_mutex, WriteBuffers, stride_y, stride_z);
  });
  loop_future.wait();
  pool.wait();
  for (auto &writebuffer : WriteBuffers) {
    candidates.insert(candidates.end(), writebuffer.data.begin(),
                      writebuffer.data.begin() + writebuffer.size);
  }
}

// Lee94 skeleton, img is supposed to be padded before.
void compute_thin_image_multi_threaded(std::span<uint8_t> img, const uint32_t D,
                                       const uint32_t H, const uint32_t W);
} // namespace skel
