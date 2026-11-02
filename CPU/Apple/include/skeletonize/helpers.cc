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

bool skel::is_simple_point_whole_frontier(uint32_t packed_neighborhood) {
    uint32_t remaining = packed_neighborhood & ~(1u << 13);

    if (remaining == 0)
        return true;

    uint32_t frontier = remaining & -remaining;
    remaining &= ~frontier;

    while (frontier != 0) {
        uint32_t next = 0;

        uint32_t current = frontier;

        while (current != 0) {
            const unsigned bit = std::countr_zero(current);
            current &= current - 1;

            next |= ADJACENCY_MASKS[bit];
        }

        next &= remaining;

        if (next == 0)
            return remaining == 0;

        remaining &= ~next;
        frontier = next;
    }

    return remaining == 0;
}

bool skel::is_simple_point_whole_frontier_precomp(uint32_t packed_neighborhood)
{
    uint32_t remaining = packed_neighborhood & ~(1u << 13);

    if (remaining == 0)
        return true;

    uint32_t frontier = remaining & -remaining;
    remaining ^= frontier;

    while (remaining != 0) {
        const uint32_t adjacent =
            ADJ_LUT_0[frontier & 0x7Fu] |
            ADJ_LUT_1[(frontier >> 7) & 0x3Fu] |
            ADJ_LUT_2[(frontier >> 14) & 0x3Fu] |
            ADJ_LUT_3[(frontier >> 20) & 0x7Fu];

        const uint32_t next = adjacent & remaining;

        if (next == 0)
            return false;

        remaining ^= next;
        frontier = next;
    }

    return true;
}
