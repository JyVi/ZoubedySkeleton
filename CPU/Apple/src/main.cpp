#include <algorithm>
#include <iostream>
#include <string>
//#include "include/skeletonize/lut.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <utility>


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


int main(int argc, char** argv)
{
    const auto EULER_LUT = generate_Euler_LUT();
    std::cout << "hello world" << std::endl;
    std::cout << "my first cpp program after too much time" << std::endl;

    for (auto& elem : EULER_LUT)
        std::cout << std::to_string(elem) << " ";
    std::cout << std::endl;

    return 0;
}
