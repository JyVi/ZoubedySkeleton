#include "pipeline_single_thread.hpp"

void skel::find_simple_point_candidates(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset) {

  uint32_t stride_y = W;
  uint32_t stride_z = W * H;
  // Padded img, so from 1 to size-1
  for (uint32_t d : std::views::iota(1u, D - 1)) {
    uint32_t d_offset = d * stride_z;

    for (uint32_t h : std::views::iota(1u, H - 1)) {
      // Create a range to iterate over.
      uint32_t row_start = d_offset + (h * stride_y) + 1;
      uint32_t row_end = row_start + (W - 2);

      for (uint32_t i : std::views::iota(row_start, row_end)) {

        if (img[i] != 1)
          continue;
        if (img[i + b_offset] != 0)
          continue;

        uint32_t packed = get_neighborhood(img, i, anchor_configs);

        if (is_endpoint(packed))
          continue;
        if (!is_euler_invariant(packed))
          continue;
        if (!is_simple_point(packed))
          continue;

        candidates.emplace_back(i);
      }
    }
  }
}

void skel::find_simple_point_candidates_Byte4(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset) {
  uint32_t stride_y = W;
  uint32_t stride_z = W * H;

  uint32_t chunk_skip = 0;
  uint32_t offset_chunk_skip = 0;

  for (uint32_t d : std::views::iota(1u, D - 1)) {
    uint32_t d_offset = d * stride_z;

    for (uint32_t h : std::views::iota(1u, H - 1)) {
      // Create a range to iterate over.
      uint32_t row_start = d_offset + (h * stride_y) + 1;
      uint32_t row_end = row_start + (W - 2);
      uint32_t row_chunks = (row_end - row_start) / 4;

      for (uint32_t i : std::views::iota(0u, row_chunks)) {
        uint32_t chunk_idx = row_start + i * 4;
        Byte4 chunk;
        Byte4 offset_chunk;
        std::memcpy(&chunk, &img[chunk_idx], sizeof(Byte4));
        std::memcpy(&offset_chunk, &img[chunk_idx + b_offset], sizeof(Byte4));

        // fast 4 bytes skip
        if (std::bit_cast<uint32_t>(chunk) == 0)
          continue;
        if (std::bit_cast<uint32_t>(offset_chunk) == 0x01010101)
          continue;

        if (chunk.x == 1 && offset_chunk.x == 0) {
          uint32_t packed =
              get_neighborhood(img, chunk_idx + 0, anchor_configs);
          if (!is_endpoint(packed) && is_euler_invariant(packed) &&
              is_simple_point(packed))
            candidates.emplace_back(chunk_idx + 0);
        }
        if (chunk.y == 1 && offset_chunk.y == 0) {
          uint32_t packed =
              get_neighborhood(img, chunk_idx + 1, anchor_configs);
          if (!is_endpoint(packed) && is_euler_invariant(packed) &&
              is_simple_point(packed))
            candidates.emplace_back(chunk_idx + 1);
        }
        if (chunk.z == 1 && offset_chunk.z == 0) {
          uint32_t packed =
              get_neighborhood(img, chunk_idx + 2, anchor_configs);
          if (!is_endpoint(packed) && is_euler_invariant(packed) &&
              is_simple_point(packed))
            candidates.emplace_back(chunk_idx + 2);
        }
        if (chunk.overflow_value == 1 && offset_chunk.overflow_value == 0) {
          uint32_t packed =
              get_neighborhood(img, chunk_idx + 3, anchor_configs);
          if (!is_endpoint(packed) && is_euler_invariant(packed) &&
              is_simple_point(packed))
            candidates.emplace_back(chunk_idx + 3);
        }
      }
      uint32_t tail_start = row_start + (row_chunks * 4);
      for (uint32_t i : std::views::iota(tail_start, row_end)) {
        if (img[i] != 1)
          continue;
        if (img[i + b_offset] != 0)
          continue;

        uint32_t packed = get_neighborhood(img, i, anchor_configs);

        if (is_endpoint(packed))
          continue;
        if (!is_euler_invariant(packed))
          continue;
        if (!is_simple_point(packed))
          continue;

        candidates.emplace_back(i);
      }
    }
  }
  // std::sort(candidates.begin(), candidates.end());
}

void skel::compute_thin_image_single_threaded(std::span<uint8_t> img,
                                              const uint32_t D,
                                              const uint32_t H,
                                              const uint32_t W) {
  uint32_t stride_y = W;
  uint32_t stride_z = W * H;
  uint32_t total_voxels = D * H * W;
  const auto anchor_configs = generate_anchor_configs(stride_y, stride_z);
  // This order is to be kept because of the skimage compatibility and itself to
  // ImageJ
  constexpr std::array<int, 6> borders = {4, 3, 2, 1, 5, 6};
  // constexpr std::array<int, 6> borders = {1, 2, 4, 3, 6, 5};
  const std::array<int64_t, 7> border_offsets = {
      0,                               // 0: unused
      -1,                              // 1: X-1 (North)
      1,                               // 2: X+1 (South)
      static_cast<int64_t>(stride_y),  // 3: Y+1 (East)
      -static_cast<int64_t>(stride_y), // 4: Y-1 (West)
      static_cast<int64_t>(stride_z),  // 5: Z+1 (Up)
      -static_cast<int64_t>(stride_z)  // 6: Z-1 (Bottom)
  };
  const int num_borders = (D == 3) ? 4 : 6;
  std::vector<uint32_t> candidates;
  candidates.reserve(stride_z);
  int unchanged_borders = 0;
  constexpr uint8_t Byte4PerUnit = 4;

  while (unchanged_borders < num_borders) {
    unchanged_borders = 0;

    for (auto border : std::span(borders).first(num_borders)) {
      int64_t b_offset = border_offsets[border];
      candidates.clear();

      // Pass 1
      /*
      find_simple_point_candidates(img, candidates, anchor_configs, D, H, W,
                                   b_offset);
      find_simple_point_candidates_Byte4(img, candidates, anchor_configs, D, H,
                                         W, b_offset);
        */
      find_simple_point_candidates_Iterative_Byte4<Byte4PerUnit>(
          img, candidates, anchor_configs, D, H, W, b_offset);
      // Pass 2
      unchanged_borders += sequential_recheck(img, candidates, anchor_configs,
                                              stride_y, stride_z);
    }
  }
}
