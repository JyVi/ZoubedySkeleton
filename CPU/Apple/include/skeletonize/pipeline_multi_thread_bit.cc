#include "pipeline_multi_thread_bit.hpp"


void skel::find_candidates_bit_work_unit(
    std::span<const uint64_t> img, const BitWorkUnit &unit,
    const std::array<AnchorConfig, 9> &anchors, int64_t b_offset,
    std::vector<uint32_t> &local_candidates) {
  constexpr size_t BatchSize = 62;
  constexpr unsigned SparseThreshold = 16;

  std::array<uint32_t, BatchSize> neighborhoods{};

  size_t first = unit.start_idx;
  size_t remaining = unit.length;

  while (remaining > 0) {
    const size_t count = std::min(BatchSize, remaining);
    const uint64_t valid_mask = (uint64_t{1} << count) - 1;

    // Stage 1: Keep foreground center voxels.
    const uint64_t main_bits = load_bits_at_voxel(img, first);
    uint64_t mask = main_bits & valid_mask;

    if (mask == 0) {
      first += count;
      remaining -= count;
      continue;
    }

    // Stage 2: Keep voxels whose selected border neighbor is background.
    const size_t offset_first =
        static_cast<size_t>(static_cast<int64_t>(first) + b_offset);

    const uint64_t offset_bits = load_bits_at_voxel(img, offset_first);

    mask &= ~offset_bits & valid_mask;

    if (mask == 0) {
      first += count;
      remaining -= count;
      continue;
    }

    // Stage 3: Choose sparse scalar extraction or batch extraction.
    const unsigned candidates_in_batch = std::popcount(mask);

    if (count == BatchSize && candidates_in_batch <= SparseThreshold) {

      // Sparse path: extract neighborhoods only for eligible lanes.
      uint64_t pending = mask;

      while (pending != 0) {
        const unsigned bit = std::countr_zero(pending);
        pending &= pending - 1;

        const uint32_t n = get_neighborhood(img, first + bit, anchors);

        if (!is_endpoint(n) && is_euler_invariant(n) && is_simple_point(n)) {
          local_candidates.emplace_back(static_cast<uint32_t>(first + bit));
        }
      }
    } else {
      // Dense path: compute neighborhoods for the whole batch.
      if (count == BatchSize) {
        const auto words = load_neighborhood_words62(img, first, anchors);

        get_neighborhoods62_bit_plane(words, neighborhoods);
      } else {
        get_neighborhoods_bit_plane_intermediary(
            img, first, count, anchors,
            std::span<uint32_t>(neighborhoods).first(count));
      }

      // Run topology tests only for eligible lanes.
      uint64_t pending = mask;

      while (pending != 0) {
        const unsigned bit = std::countr_zero(pending);
        pending &= pending - 1;

        const uint32_t n = neighborhoods[bit];

        if (!is_endpoint(n) && is_euler_invariant(n) && is_simple_point(n)) {
          local_candidates.emplace_back(static_cast<uint32_t>(first + bit));
        }
      }
    }

    first += count;
    remaining -= count;
  }
}

void skel::find_candidates_bit_work_unit_mutex(
    std::span<const uint64_t> img, const BitWorkUnit &unit,
    const std::array<AnchorConfig, 9> &anchors, int64_t b_offset,
    std::array<uint32_t, 8192> &buffer_candidates,
    size_t &index_candidates_array) {
  constexpr size_t BatchSize = 62;
  constexpr unsigned SparseThreshold = 16;

  std::array<uint32_t, BatchSize> neighborhoods;

  size_t first = unit.start_idx;
  size_t remaining = unit.length;

  index_candidates_array = 0;

  while (remaining > 0) {
    const size_t count = std::min(BatchSize, remaining);
    const uint64_t valid_mask = (uint64_t{1} << count) - 1;

    // Filter 1: center voxel must be foreground.
    const uint64_t main_bits = load_bits_at_voxel(img, first);
    uint64_t mask = main_bits & valid_mask;

    if (mask == 0) {
      first += count;
      remaining -= count;
      continue;
    }

    // Filter 2: selected border neighbor must be background.
    const size_t offset_first =
        static_cast<size_t>(static_cast<int64_t>(first) + b_offset);

    const uint64_t offset_bits = load_bits_at_voxel(img, offset_first);

    mask &= ~offset_bits & valid_mask;

    if (mask == 0) {
      first += count;
      remaining -= count;
      continue;
    }

    const unsigned candidates_in_batch = std::popcount(mask);

    if (count == BatchSize && candidates_in_batch <= SparseThreshold) {
        // Sparse path: extract only eligible voxels.
        uint64_t pending = mask;

        while (pending != 0) {
            const unsigned bit = std::countr_zero(pending);
            pending &= pending - 1;

            const uint32_t n = get_neighborhood(img, first + bit, anchors);

            if (!is_endpoint(n) &&
                is_euler_invariant(n) &&
                is_simple_point(n)) {
                buffer_candidates[index_candidates_array++] =
                    static_cast<uint32_t>(first + bit);
            }
        }
    } else {
        // Dense path: construct neighborhoods for the batch.
        if (count == BatchSize) {
            const auto words = load_neighborhood_words62(img, first, anchors);
            get_neighborhoods62_bit_plane(words, neighborhoods);
        } else {
            get_neighborhoods_bit_plane_intermediary(
                img, first, count, anchors,
                std::span<uint32_t>(neighborhoods).first(count));
        }

        uint64_t pending = mask;

        while (pending != 0) {
            const unsigned bit = std::countr_zero(pending);
            pending &= pending - 1;

            const uint32_t n = neighborhoods[bit];

            if (!is_endpoint(n) &&
                is_euler_invariant(n) &&
                is_simple_point(n)) {
                buffer_candidates[index_candidates_array++] =
                    static_cast<uint32_t>(first + bit);
            }
        }
    }

    first += count;
    remaining -= count;
  }
}

void skel::find_simple_point_candidates_packed_parallel(
    std::span<const uint64_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset) {
  constexpr uint32_t VoxelsPerUnit = 8192;

  const size_t total_units = get_total_bit_work_units<VoxelsPerUnit>(D, H, W);

  if (total_units == 0) {
    candidates.clear();
    return;
  }

  // Each pool worker writes exclusively to its own vector.
  std::vector<std::vector<uint32_t>> worker_candidates(pool.get_thread_count());

  auto loop_future =
      pool.submit_loop(size_t{0}, total_units, [&](size_t unit_id) {
        const size_t worker_id = BS::this_thread::get_index().value();

        const BitWorkUnit unit =
            get_bit_work_unit<VoxelsPerUnit>(unit_id, D, H, W);

        find_candidates_bit_work_unit(img, unit, anchor_configs, b_offset,
                                      worker_candidates[worker_id]);
      });

  loop_future.wait();

  // Merge results and restore deterministic row-major order.
  size_t total_candidates = 0;
  for (const auto &local : worker_candidates) {
    total_candidates += local.size();
  }

  candidates.clear();
  candidates.reserve(total_candidates);

  for (const auto &local : worker_candidates) {
    candidates.insert(candidates.end(), local.begin(), local.end());
  }
}

void skel::find_simple_point_candidates_packed_parallel_mutex(
    std::span<const uint64_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset) {
  constexpr uint32_t VoxelsPerUnit = 8192;
  constexpr size_t LocalCandidateCapacity = VoxelsPerUnit;

  const size_t total_units = get_total_bit_work_units<VoxelsPerUnit>(D, H, W);

  candidates.clear();

  if (total_units == 0)
    return;

  std::mutex candidates_mutex;
  std::vector<WriteBuffer> writebuffer(pool.get_thread_count());

  auto loop_future =
      pool.submit_loop(size_t{0}, total_units, [&](size_t unit_id) {
        // Each worker owns its own write buffer.
        const size_t worker_id = BS::this_thread::get_index().value();

        WriteBuffer &local_write_buffer = writebuffer[worker_id];

        std::array<uint32_t, LocalCandidateCapacity> buffer_candidates;

        size_t index_candidates_array = 0;

        const BitWorkUnit unit =
            get_bit_work_unit<VoxelsPerUnit>(unit_id, D, H, W);

        find_candidates_bit_work_unit_mutex(img, unit, anchor_configs, b_offset,
                                            buffer_candidates,
                                            index_candidates_array);

        // Same mutex/write-buffer logic as the byte implementation.
        if (index_candidates_array == 0)
          return;

        std::span<uint32_t> span_array(buffer_candidates.data(),
                                       index_candidates_array);

        if (local_write_buffer.size + index_candidates_array >=
            WRITE_BUFFER_CAPACITY) {
          {
            std::lock_guard<std::mutex> lock(candidates_mutex);

            candidates.insert(candidates.end(), local_write_buffer.data.begin(),
                              local_write_buffer.data.begin() +
                                  local_write_buffer.size);

            candidates.insert(candidates.end(), span_array.begin(),
                              span_array.end());
          }

          local_write_buffer.size = 0;
        } else {
          std::copy(span_array.begin(), span_array.end(),
                    local_write_buffer.data.begin() + local_write_buffer.size);

          local_write_buffer.size += index_candidates_array;
        }
      });

  loop_future.wait();

  // Flush each worker's residual buffer after all tasks finish.
  for (auto &local_write_buffer : writebuffer) {
    if (local_write_buffer.size == 0)
      continue;

    candidates.insert(candidates.end(), local_write_buffer.data.begin(),
                      local_write_buffer.data.begin() +
                          local_write_buffer.size);
    local_write_buffer.size = 0;
  }
}

void skel::compute_thin_image_multi_threaded_bit(std::span<uint64_t> img,
                                                 const uint32_t D,
                                                 const uint32_t H,
                                                 const uint32_t W) {
  const uint32_t stride_y = W;
  const uint32_t stride_z = W * H;

  const auto anchor_configs = generate_anchor_configs(stride_y, stride_z);

  constexpr std::array<int, 6> borders = {4, 3, 2, 1, 5, 6};

  const std::array<int64_t, 7> border_offsets = {
      0,
      -1,
      1,
      static_cast<int64_t>(stride_y),
      -static_cast<int64_t>(stride_y),
      static_cast<int64_t>(stride_z),
      -static_cast<int64_t>(stride_z)};

  const int num_borders = (D == 3) ? 4 : 6;

  std::vector<uint32_t> candidates;
  candidates.reserve(stride_z);

  int unchanged_borders = 0;

  const std::span<const uint64_t> read_img = img;

  while (unchanged_borders < num_borders) {
    unchanged_borders = 0;

    for (int border : std::span(borders).first(num_borders)) {
      const int64_t b_offset = border_offsets[border];
      candidates.clear();

      // find_simple_point_candidates_packed_parallel(
      //     read_img, candidates, anchor_configs, D, H, W, b_offset);

      find_simple_point_candidates_packed_parallel_mutex(
          read_img, candidates, anchor_configs, D, H, W, b_offset);

      std::sort(std::execution::par_unseq, candidates.begin(),
                candidates.end());
      unchanged_borders += sequential_recheck(img, candidates, anchor_configs,
                                              stride_y, stride_z);
    }
  }
}
