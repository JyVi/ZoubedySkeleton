#include "skeletonize/pipeline.hpp"
#include "skeletonize/helpers.hpp"
#include "skeletonize/lut.hpp"
//#include "skeletonize/perfetto_tracing.hpp"
#include "skeletonize/pipeline_multi_thread.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <execution>
#include <iostream>
#include <ranges>
#include <span>
#include <sys/types.h>
#include <vector>

void skel::compute_thin_image(std::span<uint8_t> img, const uint32_t D,
                              const uint32_t H, const uint32_t W) {
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
  constexpr uint32_t bytes_per_unit = Byte4PerUnit * 32; // 32;

  // std::vector<uint32_t> seq_candidates;
  while (unchanged_borders < num_borders) {
    unchanged_borders = 0;

    for (auto border : std::span(borders).first(num_borders)) {
      int64_t b_offset = border_offsets[border];
      candidates.clear();
      // seq_candidates.clear();

      // Pass 1
      // find_simple_point_candidates_Byte4(img, candidates, anchor_configs, D,
      // H,
      //                                   W, b_offset);
      // find_simple_point_candidates_Byte4(img, seq_candidates, anchor_configs,
      // D, H, W, b_offset);
      // find_simple_point_candidates_Iteratite_preParallel_Byte4<Byte4PerUnit,
      // bytes_per_unit>(img, candidates, anchor_configs, D, H,
      //    W, b_offset);
      find_simple_point_candidates_Parallel_Byte4<Byte4PerUnit, bytes_per_unit>(
          img, candidates, anchor_configs, D, H, W, b_offset);
      // find_simple_point_candidates_Byte4_prepara(img, candidates,
      // anchor_configs, D, H,
      //                                           W, b_offset);
      // find_simple_point_candidates_Iterative_Byte4(img, candidates,
      // anchor_configs, D, H,
      //                                           W, b_offset);
      // Pass 2
      std::sort(std::execution::par_unseq, candidates.begin(),
                candidates.end());
      // std::sort(candidates.begin(), candidates.end());
      /*
      std::sort(seq_candidates.begin(), seq_candidates.end());
      if (candidates.size() != seq_candidates.size()) {
          std::cerr << "FATAL: Candidate count mismatch! Seq: " <<
      candidates.size()
                    << " vs Par: " << seq_candidates.size() << "\n";
      }

      // Find the EXACT first mismatch
      size_t min_size = std::min(candidates.size(), seq_candidates.size());
      for (size_t i = 0; i < min_size; ++i) {
          if (candidates[i] != seq_candidates[i]) {
              // Helper to convert flat index back to 3D coordinates
              // Helper to convert flat index back to 3D coordinates
              auto to_3d = [H, W](uint32_t idx) {
                  uint32_t d = idx / (H * W);
                  uint32_t rem = idx % (H * W);
                  uint32_t h = rem / W;
                  uint32_t w = rem % W;
                  return std::make_tuple(d, h, w);
              };

              // Helper to find which Work Unit contains a specific flat index
              auto find_owning_unit = [D, H, W, bytes_per_unit](uint32_t
      target_idx) -> std::tuple<uint32_t, uint32_t, uint32_t, uint32_t> {
                  uint32_t stride_y = W;
                  uint32_t stride_z = W * H;
                  uint32_t bytes_per_row = W - 2;
                  uint32_t units_per_row = (bytes_per_row + bytes_per_unit - 1)
      / bytes_per_unit;

                  // Extract 3D coords
                  uint32_t d = target_idx / stride_z;
                  uint32_t rem_z = target_idx % stride_z;
                  uint32_t h = rem_z / stride_y;
                  uint32_t w = rem_z % stride_y;

                  // Check if it's in the valid padded region (1 to size-2)
                  if (d < 1 || d >= D - 1 || h < 1 || h >= H - 1 || w < 1 || w
      >= W - 1) { return {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}; //
      Invalid/Padding
                  }

                  // Map back to "logical" row/col inside the valid region
                  uint32_t logical_h = h - 1;
                  uint32_t logical_w = w - 1;

                  uint32_t unit_in_row = logical_w / bytes_per_unit;
                  uint32_t row_index_in_slice = logical_h; // Each H is a row in
      our unit mapping

                  // The global row_index combines D and H
                  uint32_t global_row_index = (d - 1) * (H - 2) + logical_h;

                  uint32_t unit_index = global_row_index * units_per_row +
      unit_in_row;

                  return {unit_index, d, h, w};
              };

              auto [sd, sh, sw] = to_3d(candidates[i]);
              auto [pd, ph, pw] = to_3d(seq_candidates[i]);

              // Identify the blocks
              auto [seq_unit, seq_d, seq_h, seq_w] =
      find_owning_unit(candidates[i]); auto [par_unit, par_d, par_h, par_w] =
      find_owning_unit(seq_candidates[i]);

              std::cerr << "FIRST LOGICAL MISMATCH at vector index " << i <<
      "!\n";

              std::cerr << "--- SEQUENTIAL FOUND ---\n";
              std::cerr << "  Flat Index: " << candidates[i] << "\n";
              std::cerr << "  Coords (d,h,w): (" << sd << ", " << sh << ", " <<
      sw << ")\n"; std::cerr << "  Owning Unit Index: " << seq_unit << "\n"; if
      (seq_unit != UINT32_MAX) { std::cerr << "  Unit Location: Slice " << seq_d
      << ", Row " << seq_h << ", Col-Chunk " << (seq_w - 1) / bytes_per_unit <<
      "\n";
              }

              std::cerr << "--- PARALLEL FOUND ---\n";
              std::cerr << "  Flat Index: " << seq_candidates[i] << "\n";
              std::cerr << "  Coords (d,h,w): (" << pd << ", " << ph << ", " <<
      pw << ")\n"; std::cerr << "  Owning Unit Index: " << par_unit << "\n"; if
      (par_unit != UINT32_MAX) { std::cerr << "  Unit Location: Slice " << par_d
      << ", Row " << par_h << ", Col-Chunk " << (par_w - 1) / bytes_per_unit <<
      "\n";
              }

              std::cerr << "----------------------\n";
              break;
          }
      }
      */
      unchanged_borders += sequential_recheck(img, candidates, anchor_configs,
                                              stride_y, stride_z);
    }
  }
}
