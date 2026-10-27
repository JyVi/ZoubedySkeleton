#include "pipeline_single_thread_bit.hpp"
#include "perfetto_tracing.hpp"
#include "skeletonize/helpers_bit.hpp"
#include "skeletonize/lut.hpp"

#include <bit>
#include <cstdint>
#include <execution>
#include <span>
#include <vector>

void skel::find_simple_point_candidates_packed(
    std::span<const uint64_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchors, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset) {
  const size_t stride_y = W;
  const size_t stride_z = size_t(W) * H;

  std::array<uint32_t, 62> neighborhoods{};

  // Process a batch of up to 62 voxels.
  const auto process_batch = [&](uint64_t first, uint64_t count) {
    // Bits 0..count-1 represent consecutive center voxels.
    const uint64_t valid_mask = (uint64_t{1} << count) - 1;

    // const uint64_t main_bits = load_bits_at_voxel(img, first);
    const uint64_t offset_first = first + b_offset;
    // const uint64_t offset_bits = load_bits_at_voxel(img, offset_first);
    const uint64_t main_bits = load_bits_at_voxel(img, first, count);

    const uint64_t offset_bits = load_bits_at_voxel(img, offset_first, count);

    // A candidate must be foreground at the center and background
    // at the selected border offset.
    uint64_t mask = main_bits & ~offset_bits & valid_mask;

    // Fast reject: no possible candidates in this batch.
    if (mask == 0)
      return;

    // Generate neighborhoods for this batch.
    if (count == 62) {
      const auto words = load_neighborhood_words62(img, first, anchors);
      get_neighborhoods62_bit_plane(words, neighborhoods);
    } else {
      get_neighborhoods_bit_plane_intermediary(
          img, first, count, anchors,
          std::span<uint32_t>(neighborhoods).first(count));
    }

    // Run the topology tests only for candidate voxels.
    while (mask != 0) {
      const unsigned bit = std::countr_zero(mask);
      mask &= mask - 1;

      const size_t idx = first + bit;
      const uint32_t n = neighborhoods[bit];

      if (!is_endpoint(n) && is_euler_invariant(n) && is_simple_point(n)) {
        candidates.emplace_back(static_cast<uint32_t>(idx));
      }
    }
  };

  for (uint32_t d = 1; d + 1 < D; ++d) {
    const size_t d_offset = size_t(d) * stride_z;

    for (uint32_t h = 1; h + 1 < H; ++h) {
      size_t first = d_offset + size_t(h) * stride_y + 1;
      size_t remaining = W - 2;

      while (remaining >= 62) {
        process_batch(first, 62);
        first += 62;
        remaining -= 62;
      }

      if (remaining > 0)
        process_batch(first, remaining);
    }
  }
}

void skel::compute_thin_image_single_threaded_bit(std::span<uint64_t> img,
                                                  const uint32_t D,
                                                  const uint32_t H,
                                                  const uint32_t W) {
  const uint32_t stride_y = W;
  const uint32_t stride_z = W * H;

  const auto anchor_configs = generate_anchor_configs(stride_y, stride_z);

  constexpr std::array<int, 6> borders = {4, 3, 2, 1, 5, 6};

  const std::array<int64_t, 7> border_offsets = {
      0,
      -1,
      1,
      static_cast<int64_t>(stride_y),
      -static_cast<int64_t>(stride_y),
      static_cast<int64_t>(stride_z),
      -static_cast<int64_t>(stride_z)};

  const int num_borders = (D == 3) ? 4 : 6;

  std::vector<uint32_t> candidates;
  candidates.reserve(stride_z);

  int unchanged_borders = 0;

  const std::span<const uint64_t> read_img = img;

  while (unchanged_borders < num_borders) {
    unchanged_borders = 0;

    for (int border : std::span(borders).first(num_borders)) {
      const int64_t b_offset = border_offsets[border];
      candidates.clear();

      find_simple_point_candidates_packed(read_img, candidates, anchor_configs,
                                          D, H, W, b_offset);

      unchanged_borders += sequential_recheck(img, candidates, anchor_configs,
                                              stride_y, stride_z);
    }
  }
}
