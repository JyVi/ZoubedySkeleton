#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <span>

#include "helpers.hpp"
#include "skeletonize/perfetto_tracing.hpp"

namespace skel {

static inline void transpose32(std::array<uint32_t, 32>& a) {
#pragma GCC unroll 32
  for (unsigned j = 16, m = 0x0000ffffu; j; j >>= 1) {
    for (unsigned k = 0; k < 32; k = (k + j + 1) & ~j) {
      uint32_t t = (a[k] ^ (a[k + j] >> j)) & m;
      a[k] ^= t;
      a[k + j] ^= t << j;
    }
    if (j > 1)
      m ^= m << (j >> 1);
  }
}

void get_neighborhoods62_bit_plane(const std::array<uint64_t, 9> &w,
                                   std::array<uint32_t, 62> &out);

void get_neighborhoods62_bit(const std::array<uint64_t, 9> &w,
                             std::array<uint32_t, 62> &out);

inline uint32_t extract_triplet(std::span<const uint64_t> packed,
                                size_t voxel_index) {
  const size_t wi = voxel_index >> 6;
  const unsigned shift = voxel_index & 63;

  uint64_t bits = packed[wi] >> shift;

  if (shift > 61)
    bits |= packed[wi + 1] << (64 - shift);

  return static_cast<uint32_t>(bits & 7u);
}

inline uint32_t get_neighborhood(std::span<const uint64_t> packed,
                                 size_t voxel_index,
                                 const std::array<AnchorConfig, 9> &anchors) {
  uint32_t n = 0;

  const auto add = [&](const AnchorConfig &a) {
    const auto base =
        static_cast<size_t>(static_cast<int64_t>(voxel_index) + a.mem_offset);

    n |= extract_triplet(packed, base) << a.bit_x;
  };

  add(anchors[0]);
  add(anchors[1]);
  add(anchors[2]);
  add(anchors[3]);
  add(anchors[4]);
  add(anchors[5]);
  add(anchors[6]);
  add(anchors[7]);
  add(anchors[8]);

  return n;
}

inline void get_neighborhoods_bit_plane_intermediary(
    std::span<const uint64_t> packed, size_t first_voxel, size_t count,
    const std::array<AnchorConfig, 9> &anchors, std::span<uint32_t> out) {
  for (size_t i = 0; i < count; ++i) {
    uint32_t n = 0;

    for (const auto &a : anchors) {
      const size_t base = static_cast<size_t>(
          static_cast<int64_t>(first_voxel + i) + a.mem_offset);

      const size_t wi = base >> 6;
      const unsigned shift = base & 63;

      uint64_t bits = packed[wi] >> shift;

      if (shift > 61)
        bits |= packed[wi + 1] << (64 - shift);

      n |= static_cast<uint32_t>(bits & 7u) << a.bit_x;
    }

    out[i] = n;
  }
}

inline uint64_t load_bits_at_voxel(std::span<const uint64_t> img,
                                   uint64_t voxel_index) {
  const size_t word_index = voxel_index >> 6;
  const unsigned bit_offset = static_cast<unsigned>(voxel_index & 63);

  uint64_t bits = img[word_index] >> bit_offset;

  if (bit_offset != 0 && word_index + 1 < img.size()) {
    bits |= img[word_index + 1] << (64 - bit_offset);
  }

  return bits;
}

inline uint64_t load_bits_at_voxel(std::span<const uint64_t> img,
                                   uint64_t bit_index, unsigned count) {
  const size_t wi = bit_index >> 6;
  const unsigned shift = static_cast<unsigned>(bit_index & 63);

  uint64_t value = img[wi] >> shift;

  if (shift != 0 && shift + count > 64 && wi + 1 < img.size()) {
    value |= img[wi + 1] << (64 - shift);
  }

  return value;
}

inline bool get_bit(std::span<const uint64_t> img, size_t voxel_index) {
  return (img[voxel_index >> 6] >> (voxel_index & 63)) & 1u;
}

inline uint64_t load64_at_bit(std::span<const uint64_t> img, size_t bit_index) {
  const size_t wi = bit_index >> 6;
  const unsigned shift = bit_index & 63;

  uint64_t value = img[wi] >> shift;

  if (shift != 0 && wi + 1 < img.size())
    value |= img[wi + 1] << (64 - shift);

  return value;
}

inline std::array<uint64_t, 9>
load_neighborhood_words62_rolled(std::span<const uint64_t> img,
                                 size_t first_voxel,
                                 const std::array<AnchorConfig, 9> &anchors) {
  std::array<uint64_t, 9> words{};

  for (size_t r = 0; r < 9; ++r) {
    const auto base = static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                          anchors[r].mem_offset);

    words[r] = load64_at_bit(img, base);
  }

  return words;
}

inline std::array<uint64_t, 9>
load_neighborhood_words62(std::span<const uint64_t> img, size_t first_voxel,
                          const std::array<AnchorConfig, 9> &a) {
  std::array<uint64_t, 9> words{};

  words[0] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[0].mem_offset));
  words[1] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[1].mem_offset));
  words[2] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[2].mem_offset));
  words[3] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[3].mem_offset));
  words[4] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[4].mem_offset));
  words[5] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[5].mem_offset));
  words[6] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[6].mem_offset));
  words[7] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[7].mem_offset));
  words[8] =
      load64_at_bit(img, static_cast<size_t>(static_cast<int64_t>(first_voxel) +
                                             a[8].mem_offset));

  return words;
}

int sequential_recheck_bit(std::span<uint64_t> img,
                       std::vector<uint64_t> &candidates,
                       const std::array<AnchorConfig, 9> &anchor_configs,
                       uint32_t stride_y, uint32_t stride_z);

} // namespace skel
