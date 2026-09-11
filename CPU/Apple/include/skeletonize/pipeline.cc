#include "pipeline.hpp"

uint32_t get_neighborhood(std::span<uint8_t> volume_array, int voxPosition,
                 std::span<const AnchorConfig, 9> precomputed_configs) {
    uint32_t neighborhood = 0;
    for (const auto& config: precomputed_configs) {
        const uint8_t* computed_anchor = volume_array.data() + voxPosition + config.mem_offset;
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
