#include "pipeline.hpp"
#include "LUT.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <ranges>
#include <span>
#include <sys/types.h>

uint32_t skel::get_neighborhood(
    const std::span<const uint8_t> volume_array, int voxPosition,
    const std::array<AnchorConfig, 9> &precomputed_configs) {

  uint32_t neighborhood = 0;
  for (const auto &config : precomputed_configs) {
    const uint8_t *computed_anchor =
        volume_array.data() + voxPosition + config.mem_offset;

    // This is like in CUDA, I load 4 uint8_t in one pass rather than just one
    Byte4 chunk;
    // No real deepcopy, it is just necessary to store the memory to my struct
    std::memcpy(&chunk, computed_anchor, sizeof(Byte4));

    // Banchless packing at the valid and required place by the LUT computation
    neighborhood |= (chunk.x ? 1u : 0u) << config.bit_x;
    neighborhood |= (chunk.y ? 1u : 0u) << config.bit_y;
    neighborhood |= (chunk.z ? 1u : 0u) << config.bit_z;
  }
  return neighborhood;
}

bool skel::is_endpoint(const uint32_t packed_neighborhood) {
  return std::popcount(packed_neighborhood) == 2;
}

bool skel::is_simple_point(const uint32_t packed_neighborhood) {
  uint32_t shell = packed_neighborhood & ~(1u << 13);

  // no neighbors so simple point
  if (shell == 0)
    return true;

  int seed = std::countl_zero(shell);
  uint32_t frontier = (1u << seed);

  // mark the seed as visited in the shell
  shell &= ~frontier;

  while (frontier != 0) {
    int current = std::countl_zero(frontier);
    // Dequeue the neighbors
    frontier &= ~(1u << current);

    uint32_t new_neighbors = ADJACENCY_MASKS[current] & shell;

    // Enqueue the neighbors to the frontier
    frontier |= new_neighbors;
    shell &= ~new_neighbors;
  }
  return shell == 0;
}

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

int skel::sequential_recheck(
    std::span<uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs) {
  bool no_change = true;
  for (uint32_t i : candidates) {
    uint32_t packed = get_neighborhood(img, i, anchor_configs);

    if (is_simple_point(packed)) {
      img[i] = 0;
      no_change = false;
    }
  }
  return no_change ? 0 : 1;
}

void skel::compute_thin_image(std::span<uint8_t> img, const uint32_t D,
                              const uint32_t H, const uint32_t W) {
  uint32_t stride_y = W;
  uint32_t stride_z = W * H;
  uint32_t total_voxels = D * H * W;
  const auto anchor_configs = generate_anchor_configs(stride_y, stride_z);
  constexpr std::array<int, 6> borders = {4, 3, 2, 1, 5, 6};
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
  candidates.reserve(total_voxels / 10);
  int unchanged_borders = 0;

  while (unchanged_borders < num_borders) {
    unchanged_borders = 0;

    for (auto border : std::span(borders).first(num_borders)) {
      int64_t b_offset = border_offsets[border];
      candidates.clear();
      // Pass 1
      find_simple_point_candidates(img, candidates, anchor_configs, D, H, W,
                                   b_offset);
      // Pass 2
      unchanged_borders += sequential_recheck(img, candidates, anchor_configs);
    }
  }
}
