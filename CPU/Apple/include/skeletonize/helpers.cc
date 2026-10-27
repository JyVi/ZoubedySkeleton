#include "helpers.hpp"

int skel::sequential_recheck(std::span<uint8_t> img,
                             std::vector<uint32_t> &candidates,
                             const std::array<AnchorConfig, 9> &anchor_configs,
                             uint32_t stride_y, uint32_t stride_z) {
  bool no_change = true;
  for (uint32_t i : candidates) {
    uint32_t packed = get_neighborhood(img, i, anchor_configs);
    // uint32_t packed = get_neighborhood_fast(img, i, stride_y, stride_z);
    if (is_simple_point(packed)) {
      img[i] = 0;
      no_change = false;
    }
  }
  return no_change ? 1 : 0;
}

uint32_t
skel::get_neighborhood(const std::span<const uint8_t> volume_array,
                       int voxPosition,
                       const std::array<AnchorConfig, 9> &precomputed_configs) {
  uint32_t neighborhood = 0;
  for (const auto &config : precomputed_configs) {
    const uint8_t *computed_anchor =
        volume_array.data() + voxPosition + config.mem_offset;

    // This is like in CUDA, I load 4 uint8_t in one pass rather than just one
    Byte4 chunk;
    // No real deepcopy, it is just necessary to store the memory to my struct
    std::memcpy(&chunk, computed_anchor, sizeof(Byte4));

    // Shift to the EXACT Cython indices
    // Banchless packing at the valid and required place by the LUT computation
    neighborhood |= (static_cast<uint32_t>(chunk.x) << config.bit_x);
    neighborhood |= (static_cast<uint32_t>(chunk.y) << config.bit_y);
    neighborhood |= (static_cast<uint32_t>(chunk.z) << config.bit_z);
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

  int seed = std::countr_zero(shell);
  uint32_t frontier = (1u << seed);

  // mark the seed as visited in the shell
  shell &= ~frontier;

  while (frontier != 0) {
    int current = std::countr_zero(frontier);
    // Dequeue the neighbors
    frontier &= ~(1u << current);
    // frontier &= frontier - 1;

    uint32_t new_neighbors = ADJACENCY_MASKS[current] & shell;

    // Enqueue the neighbors to the frontier
    frontier |= new_neighbors;
    shell &= ~new_neighbors;
  }
  return shell == 0;
}
