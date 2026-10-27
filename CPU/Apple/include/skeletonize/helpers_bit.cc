#include "helpers_bit.hpp"
#include "perfetto_tracing.hpp"

void skel::get_neighborhoods62_bit_plane(const std::array<uint64_t, 9> &w,
                                         std::array<uint32_t, 62> &out) {

  std::array<uint32_t, 32> lo{};
  std::array<uint32_t, 32> hi{};
  //uint32_t hi[32]{};

  for (unsigned r = 0; r < 9; ++r) {
    for (unsigned d = 0; d < 3; ++d) {
      const unsigned b = 3 * r + d;
      lo[31 - b] = static_cast<uint32_t>(w[r] >> d);
      hi[31 - b] = static_cast<uint32_t>(w[r] >> (32 + d));
    }
  }

  transpose32(lo);
  transpose32(hi);

  for (unsigned i = 0; i < 32; ++i)
    out[i] = lo[31 - i];

  for (unsigned i = 0; i < 30; ++i)
    out[32 + i] = hi[31 - i];
}

void skel::get_neighborhoods62_bit(const std::array<uint64_t, 9> &w,
                                   std::array<uint32_t, 62> &out) {
  for (unsigned i = 0; i < 62; ++i) {
    uint32_t n = 0;

    for (unsigned r = 0; r < 9; ++r)
      n |= uint32_t((w[r] >> i) & 7u) << (3 * r);

    out[i] = n;
  }
}


int skel::sequential_recheck(std::span<uint64_t> img,
                             std::vector<uint32_t> &candidates,
                             const std::array<AnchorConfig, 9> &anchor_configs,
                             uint32_t stride_y, uint32_t stride_z) {
  bool no_change = true;
  const std::span<const uint64_t> read_img = img;

  for (uint32_t i : candidates) {
    const uint32_t neighborhood = get_neighborhood(read_img, i, anchor_configs);

    if (is_simple_point(neighborhood)) {
      img[i >> 6] &= ~(uint64_t{1} << (i & 63));
      no_change = false;
    }
  }

  return no_change ? 1 : 0;
}
