#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <sys/types.h>
#include <vector>

#include "helpers.hpp"
#include "lut.hpp"

namespace skel {

void find_simple_point_candidates(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset);

// Version of simple point detetion with loading more data at once
void find_simple_point_candidates_Byte4(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset);

template <size_t Byte4PerUnit = 4> // 1 = 4 bytes, 4 = 16 bytes, etc.
void find_simple_point_candidates_Iterative_Byte4(
    std::span<const uint8_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchor_configs, const uint32_t D,
    const uint32_t H, const uint32_t W, const int64_t b_offset) {

  uint32_t stride_y = W;
  uint32_t stride_z = W * H;
  constexpr uint32_t bytes_per_unit = Byte4PerUnit * 4;

  // Standard for loops guarantee zero overhead and perfect compiler
  // optimization
  for (uint32_t d = 1; d < D - 1; ++d) {
    uint32_t d_offset = d * stride_z;

    for (uint32_t h = 1; h < H - 1; ++h) {
      uint32_t row_start = d_offset + (h * stride_y) + 1;
      uint32_t bytes_per_row = W - 2;

      // DIVISION HAPPENS ONCE PER ROW, NOT PER CHUNK!
      uint32_t units_per_row = bytes_per_row / bytes_per_unit;
      uint32_t bytes_processed = units_per_row * bytes_per_unit;
      uint32_t tail_bytes = bytes_per_row - bytes_processed;

      // ── 1. PROCESS FULL UNITS ──
      for (uint32_t i = 0; i < units_per_row; ++i) {
        uint32_t unit_idx = row_start + i * bytes_per_unit;

        // Wide load: Compiler optimizes this to a single SIMD register load
        std::array<uint8_t, bytes_per_unit> main_bytes;
        std::array<uint8_t, bytes_per_unit> offset_bytes;
        std::memcpy(main_bytes.data(), &img[unit_idx], bytes_per_unit);
        std::memcpy(offset_bytes.data(), &img[unit_idx + b_offset],
                    bytes_per_unit);

        // Unrolled processing by 4 bytes
        uint32_t j = 0;
        for (; j + 3 < bytes_per_unit; j += 4) {
          uint32_t main_32, offset_32;
          std::memcpy(&main_32, &main_bytes[j], 4);
          std::memcpy(&offset_32, &offset_bytes[j], 4);

          // FAST 4-BYTE SKIP
          if (main_32 == 0)
            continue;
          if (offset_32 == 0x01010101)
            continue;

          // Byte 0
          if (main_bytes[j] == 1 && offset_bytes[j] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j);
          }
          // Byte 1
          if (main_bytes[j + 1] == 1 && offset_bytes[j + 1] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j + 1, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j + 1);
          }
          // Byte 2
          if (main_bytes[j + 2] == 1 && offset_bytes[j + 2] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j + 2, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j + 2);
          }
          // Byte 3
          if (main_bytes[j + 3] == 1 && offset_bytes[j + 3] == 0) {
            uint32_t packed =
                get_neighborhood(img, unit_idx + j + 3, anchor_configs);
            if (!is_endpoint(packed) && is_euler_invariant(packed) &&
                is_simple_point(packed))
              candidates.emplace_back(unit_idx + j + 3);
          }
        }
      }

      // ── 2. PROCESS TAIL (at most bytes_per_unit - 1 bytes) ──
      uint32_t tail_start = row_start + bytes_processed;
      for (uint32_t i = 0; i < tail_bytes; ++i) {
        uint32_t idx = tail_start + i;
        if (img[idx] == 1 && img[idx + b_offset] == 0) {
          uint32_t packed = get_neighborhood(img, idx, anchor_configs);
          if (!is_endpoint(packed) && is_euler_invariant(packed) &&
              is_simple_point(packed)) {
            candidates.emplace_back(idx);
          }
        }
      }
    }
  }
}

void compute_thin_image_single_threaded(std::span<uint8_t> img,
                                        const uint32_t D, const uint32_t H,
                                        const uint32_t W);

}; // namespace skel
