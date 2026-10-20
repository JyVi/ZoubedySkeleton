#pragma once

#include "BS_thread_pool.hpp"
#include "lut.hpp"
#include "helpers.hpp"
//#include "skeletonize/perfetto_tracing.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <sys/types.h>
#include <vector>

namespace skel {

// Lee94 skeleton, img is supposed to be padded before.
void compute_thin_image(std::span<uint8_t> img, const uint32_t D,
                        const uint32_t H, const uint32_t W);
} // namespace skel
