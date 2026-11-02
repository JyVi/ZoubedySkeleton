#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <utility>
#include <execution>

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

constexpr uint8_t octant_lut_index(
    uint32_t packed_neighborhood,
    const std::array<uint8_t, 7>& octant) noexcept {

    uint8_t index = 1;

    for (unsigned j = 0; j < 7; ++j) {
        const uint8_t bit = static_cast<uint8_t>(
            (packed_neighborhood >> octant[j]) & 1u
        );

        index |= static_cast<uint8_t>(bit << (7 - j));
    }

    return index;
}

constexpr bool is_euler_invariant_fast(
    uint32_t packed_neighborhood) noexcept {

    int euler_sum = 0;

    for (const auto& octant : OCTANT_INDICES_27) {
        const uint8_t index =
            octant_lut_index(packed_neighborhood, octant);

        euler_sum += EULER_LUT[index];
    }

    return euler_sum == 0;
}

constexpr auto make_euler_lut_7() {
    std::array<int8_t, 128> lut{};

    for (unsigned i = 0; i < 128; ++i)
        lut[i] = EULER_LUT[(i << 1) | 1u];

    return lut;
}

alignas(64) inline constexpr auto EULER_LUT_7 = make_euler_lut_7();

inline constexpr int EULER_MIN = [] {
    int result = EULER_LUT_7[0];

    for (const int8_t value : EULER_LUT_7)
        result = std::min(result, static_cast<int>(value));

    return result;
}();

inline constexpr int EULER_MAX = [] {
    int result = EULER_LUT_7[0];

    for (const int8_t value : EULER_LUT_7)
        result = std::max(result, static_cast<int>(value));

    return result;
}();

template <std::size_t Octant>
inline int euler_octant_term(uint32_t n) noexcept {
    constexpr auto bits = OCTANT_INDICES_27[Octant];

    const unsigned index =
        (((n >> bits[0]) & 1u) << 6) |
        (((n >> bits[1]) & 1u) << 5) |
        (((n >> bits[2]) & 1u) << 4) |
        (((n >> bits[3]) & 1u) << 3) |
        (((n >> bits[4]) & 1u) << 2) |
        (((n >> bits[5]) & 1u) << 1) |
        (((n >> bits[6]) & 1u) << 0);

    return EULER_LUT_7[index];
}

constexpr inline bool is_euler_invariant_stacked(uint32_t n) noexcept {
    const int e0 = euler_octant_term<0>(n);
    const int e1 = euler_octant_term<1>(n);
    const int e2 = euler_octant_term<2>(n);
    const int e3 = euler_octant_term<3>(n);
    const int e4 = euler_octant_term<4>(n);
    const int e5 = euler_octant_term<5>(n);
    const int e6 = euler_octant_term<6>(n);
    const int e7 = euler_octant_term<7>(n);

    return (e0 + e1 + e2 + e3 + e4 + e5 + e6 + e7) == 0;
}

template <std::size_t Octant>
constexpr int euler_octant_term_bis(uint32_t n) noexcept {
    constexpr auto bits = OCTANT_INDICES_27[Octant];

    constexpr unsigned shifts[7] = {6, 5, 4, 3, 2, 1, 0};

    unsigned index = 0;

    for (unsigned i = 0; i < 7; ++i) {
        index |= ((n >> bits[i]) & 1u) << shifts[i];
    }

    return EULER_LUT_7[index];
}

template <std::size_t A, std::size_t B>
struct EulerPairLUT {
    static constexpr unsigned TableBits = 14;
    static constexpr unsigned TableSize = 1u << TableBits;

    struct Spec {
        std::array<uint8_t, TableBits> positions{};
        unsigned count = 0;
    };

    static consteval Spec make_spec() {
        Spec spec{};

        auto add_position = [&](uint8_t position) {
            for (unsigned i = 0; i < spec.count; ++i) {
                if (spec.positions[i] == position)
                    return;
            }

            spec.positions[spec.count++] = position;
        };

        for (uint8_t p : OCTANT_INDICES_27[A])
            add_position(p);

        for (uint8_t p : OCTANT_INDICES_27[B])
            add_position(p);

        return spec;
    }

    inline static constexpr Spec spec = make_spec();

    static constexpr unsigned make_index(uint32_t n) noexcept {
        unsigned index = 0;

        for (unsigned i = 0; i < spec.count; ++i) {
            index |= ((n >> spec.positions[i]) & 1u) << i;
        }

        return index;
    }

    static constexpr uint32_t reconstruct(unsigned index) noexcept {
        uint32_t n = 0;

        for (unsigned i = 0; i < spec.count; ++i) {
            n |= ((index >> i) & 1u) << spec.positions[i];
        }

        return n;
    }

    static constexpr int octant_value(uint32_t n,
                                      std::size_t octant) noexcept {
        unsigned index = 0;

        for (unsigned i = 0; i < 7; ++i) {
            index |= ((n >> OCTANT_INDICES_27[octant][i]) & 1u)
                     << (6 - i);
        }

        return EULER_LUT_7[index];
    }

    static consteval auto make_table() {
        std::array<int8_t, TableSize> table{};

        for (unsigned i = 0; i < (1u << spec.count); ++i) {
            const uint32_t n = reconstruct(i);

            table[i] = static_cast<int8_t>(
                octant_value(n, A) + octant_value(n, B));
        }

        return table;
    }

    inline static constexpr auto table = make_table();

    static constexpr int lookup(uint32_t n) noexcept {
        return table[make_index(n)];
    }
};

using EulerPair01 = EulerPairLUT<0, 1>;

inline bool is_euler_invariant_C(uint32_t n) noexcept {
    const int s01 = EulerPair01::lookup(n);

    const int e2 = euler_octant_term<2>(n);
    const int e3 = euler_octant_term<3>(n);
    const int e4 = euler_octant_term<4>(n);
    const int e5 = euler_octant_term<5>(n);
    const int e6 = euler_octant_term<6>(n);
    const int e7 = euler_octant_term<7>(n);

    return (s01 + e2 + e3 + e4 + e5 + e6 + e7) == 0;
}

using EulerPair01 = EulerPairLUT<0, 1>;
using EulerPair23 = EulerPairLUT<2, 3>;
using EulerPair45 = EulerPairLUT<4, 5>;
using EulerPair67 = EulerPairLUT<6, 7>;

inline bool is_euler_invariant_D(uint32_t n) noexcept {
    const int s01 = EulerPair01::lookup(n);
    const int s23 = EulerPair23::lookup(n);
    const int s45 = EulerPair45::lookup(n);
    const int s67 = EulerPair67::lookup(n);

    return (s01 + s23 + s45 + s67) == 0;
}

} // namespace skel
