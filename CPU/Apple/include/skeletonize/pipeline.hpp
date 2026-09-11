#pragma once

#include <cstdint>
#include <cstring>
#include <span>

struct Byte4 {
  uint8_t x, y, z, overflow_value;
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
constexpr std::array<AnchorConfig, 9> generate_anchor_configs(int stride_y,
                                                              int stride_z) {
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

// Get the neighborhood of a 3d volume voxel
uint32_t get_neighborhood(std::span<uint8_t> volume_array, int voxPosition,
                          std::span<const AnchorConfig, 9> precomputed_configs);
