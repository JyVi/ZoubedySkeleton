#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace skel {
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

// Returns true if the voxel is an endpoint (exactly 1 neighbor + the center
// voxel)
bool is_endpoint(uint32_t packed_neighborhood);

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
} // namespace skel
