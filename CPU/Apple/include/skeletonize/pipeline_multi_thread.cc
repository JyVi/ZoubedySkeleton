#include "pipeline_multi_thread.hpp"

void skel::compute_thin_image_multi_threaded(std::span<uint8_t> img, const uint32_t D,
                              const uint32_t H, const uint32_t W)
{
  uint32_t stride_y = W;
  uint32_t stride_z = W * H;
  uint32_t total_voxels = D * H * W;
  const auto anchor_configs = generate_anchor_configs(stride_y, stride_z);
  constexpr std::array<int, 6> borders = {4, 3, 2, 1, 5, 6};
  // constexpr std::array<int, 6> borders = {1, 2, 4, 3, 6, 5};
  const std::array<int64_t, 7> border_offsets = {
      0,                               // 0: unused
      -1,                              // 1: X-1 (North)
      1,                               // 2: X+1 (South)
      static_cast<int64_t>(stride_y),  // 3: Y+1 (East)
      -static_cast<int64_t>(stride_y), // 4: Y-1 (West)
      static_cast<int64_t>(stride_z),  // 5: Z+1 (Up)
      -static_cast<int64_t>(stride_z)  // 6: Z-1 (Bottom)
  };
  const int num_borders = (D == 3) ? 4 : 6;
  std::vector<uint32_t> candidates;
  candidates.reserve(stride_z);
  int unchanged_borders = 0;
  constexpr uint8_t Byte4PerUnit = 4;
  constexpr uint32_t bytes_per_unit = Byte4PerUnit * 32;//32;

  while (unchanged_borders < num_borders) {
    unchanged_borders = 0;

    for (auto border : std::span(borders).first(num_borders)) {
      int64_t b_offset = border_offsets[border];
      candidates.clear();
      //
      // Pass 1
      find_simple_point_candidates_Parallel_Byte4<Byte4PerUnit, bytes_per_unit>(img, candidates, anchor_configs, D, H,
          W, b_offset);
      // Pass 2
      std::sort(std::execution::par_unseq, candidates.begin(), candidates.end());
      unchanged_borders += sequential_recheck(img, candidates, anchor_configs, stride_y, stride_z);
    }
  }
}
