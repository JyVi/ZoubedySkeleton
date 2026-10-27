#pragma once

#include "helpers_bit.hpp"
#include "pipeline_multi_thread.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

constexpr size_t NeighborhoodBatchSize = 62;
constexpr size_t NeighborhoodRowCount = 9;

namespace skel {

void find_simple_point_candidates_packed(
    std::span<const uint64_t> img, std::vector<uint32_t> &candidates,
    const std::array<AnchorConfig, 9> &anchors, uint32_t D, uint32_t H,
    uint32_t W, int64_t b_offset);
void compute_thin_image_single_threaded_bit(std::span<uint64_t> img,
                                            const uint32_t D, const uint32_t H,
                                            const uint32_t W);

} // namespace skel
