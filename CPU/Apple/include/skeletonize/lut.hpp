#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <utility>

namespace skel {
constexpr std::array<int8_t, 256> generate_Euler_LUT() {
  std::array<int8_t, 256> LUT = {
      1,  -1, -1, 1,  -3, -1, -1, 1,  -1, 1,  1, -1, 3,  1,  1, -1, -3, -1, 3,
      1,  1,  -1, 3,  1,  -1, 1,  1,  -1, 3,  1, 1,  -1, -3, 3, -1, 1,  1,  3,
      -1, 1,  -1, 1,  1,  -1, 3,  1,  1,  -1, 1, 3,  3,  1,  5, 3,  3,  1,  -1,
      1,  1,  -1, 3,  1,  1,  -1, -7, -1, -1, 1, -3, -1, -1, 1, -1, 1,  1,  -1,
      3,  1,  1,  -1, -3, -1, 3,  1,  1,  -1, 3, 1,  -1, 1,  1, -1, 3,  1,  1,
      -1, -3, 3,  -1, 1,  1,  3,  -1, 1,  -1, 1, 1,  -1, 3,  1, 1,  -1, 1,  3,
      3,  1,  5,  3,  3,  1,  -1, 1,  1,  -1, 3, 1,  1,  -1};
  std::ranges::for_each(std::views::iota(0, 128) | std::views::reverse,
                        [&LUT](int i) {
                          LUT[2 * i + 1] = LUT[i];
                          LUT[2 * i] = 0;
                        });

  return LUT;
}

constexpr auto EULER_LUT = generate_Euler_LUT();
// The 7 neighbor indices (out of 27) for each of the 8 overlapping octants.
constexpr std::array<std::array<uint8_t, 7>, 8> OCTANT_INDICES_27 = {{
    {2, 1, 11, 10, 5, 4, 14},     // Octant 0
    {0, 9, 3, 12, 1, 10, 4},      // Octant 1
    {8, 7, 17, 16, 5, 4, 14},     // Octant 2
    {6, 15, 7, 16, 3, 12, 4},     // Octant 3
    {20, 23, 19, 22, 11, 14, 10}, // Octant 4
    {18, 21, 9, 12, 19, 22, 10},  // Octant 5
    {26, 23, 17, 14, 25, 22, 16}, // Octant 6
    {24, 25, 15, 16, 21, 22, 12}  // Octant 7
}};

constexpr std::array<uint8_t, 7> BIT_SHIFT_ARRAY = {
    1u << 7, 1u << 6, 1u << 5, 1u << 4, 1u << 3, 1u << 2, 1u << 1,
};

constexpr auto pair_view(const std::array<uint8_t, 7> &raw_octant) {
  return std::views::iota(0, 7) |
         std::views::transform(
             [&raw_octant](int j) -> std::pair<uint8_t, uint8_t> {
               return {raw_octant[j], BIT_SHIFT_ARRAY[j]};
             });
}

constexpr auto lambda_acumulation(const uint32_t packed_neighborhood) {
    return [packed_neighborhood](uint8_t element, const std::pair<uint8_t, uint8_t>& pair_view_element) -> uint8_t {
        if  (packed_neighborhood & (1u << pair_view_element.first))
            element |= pair_view_element.second;
        return element;
    };
}

constexpr auto accumulation(const uint32_t packed_neighborhood) {
    return [packed_neighborhood](const std::array<uint8_t, 7> &raw_octant) {
        return std::accumulate(
            pair_view(raw_octant).begin(),
            pair_view(raw_octant).end(),
            uint8_t{0},
            lambda_acumulation(packed_neighborhood)
        );
    };
}

constexpr auto inner_accumulation(const uint32_t packed_neighborhood) {
    return OCTANT_INDICES_27 | std::views::transform(
        [packed_neighborhood](const std::array<uint8_t, 7>& raw_octant) -> int8_t {
            uint8_t LUT_index = accumulation(packed_neighborhood)(raw_octant);
            LUT_index |= 1u;
            return EULER_LUT[LUT_index];
        }
    );
}

constexpr bool is_euler_invariant(uint32_t packed_neighborhood) {
  auto octant_values_view = inner_accumulation(packed_neighborhood);
  int8_t euler_sum = std::accumulate(
      octant_values_view.begin(),
      octant_values_view.end(),
      int8_t{0},
      std::plus<int8_t>{}
  );
  return euler_sum == 0;
}
} // namespace skel
