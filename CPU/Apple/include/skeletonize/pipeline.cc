#include "pipeline.hpp"
#include <bit>
#include <cstdint>
#include <ios>

uint32_t skel::get_neighborhood(std::span<uint8_t> volume_array, int voxPosition,
                    std::span<const AnchorConfig, 9> precomputed_configs) {
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

bool skel::is_endpoint(uint32_t packed_neighborhood) {
  return std::popcount(packed_neighborhood) == 2;
}

bool skel::is_simple_point(uint32_t packed_neighborhood) {
    uint32_t shell = packed_neighborhood & ~(1u << 13);

    // no neighbors so simple point
    if (shell == 0)
        return true;

    int seed = std::countl_zero(shell);
    uint32_t frontier = (1u << seed);

    // mark the seed as visited
    seed &= ~frontier;

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
