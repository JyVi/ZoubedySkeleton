#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <pybind11/cast.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "skeletonize/lut.hpp"
//#include "skeletonize/perfetto_tracing.hpp"
#include "skeletonize/pipeline.hpp"
#include "skeletonize/pipeline_multi_thread.hpp"
#include "skeletonize/pipeline_multi_thread_bit.hpp"
#include "skeletonize/pipeline_single_thread.hpp"
#include "skeletonize/pipeline_single_thread_bit.hpp"

namespace py = pybind11;

inline uint64_t pack_tail(const uint8_t *src, size_t count) {
  uint64_t result = 0;

  for (size_t i = 0; i < count; ++i) {
    result |= uint64_t(src[i] != 0) << i;
  }

  return result;
}

static inline void unpack_64_fast(uint64_t word, uint8_t *dst) noexcept {
#pragma GCC unroll 64
  for (unsigned b = 0; b < 64; ++b) {
    dst[b] = static_cast<uint8_t>((word >> b) & 1u);
  }
}

#include <cstdint>

#if defined(__aarch64__)
#include <arm_neon.h>

static inline uint16_t pack_16_neon(const uint8_t *src) {
  const uint8x16_t v = vld1q_u8(src);

  // 0xFF for nonzero input bytes; 0 otherwise.
  const uint8x16_t nonzero = vmvnq_u8(vceqq_u8(v, vdupq_n_u8(0)));

  // Convert each comparison result to 0 or 1.
  const uint8x16_t bits = vshrq_n_u8(nonzero, 7);

  // Weights for packing eight 0/1 lanes into one byte.
  const uint8x8_t weights = {1, 2, 4, 8, 16, 32, 64, 128};

  const uint8_t lo = vaddv_u8(vmul_u8(vget_low_u8(bits), weights));

  const uint8_t hi = vaddv_u8(vmul_u8(vget_high_u8(bits), weights));

  return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
}

static inline uint64_t pack_64_neon(const uint8_t *src) {
  const uint64_t a = pack_16_neon(src);
  const uint64_t b = pack_16_neon(src + 16);
  const uint64_t c = pack_16_neon(src + 32);
  const uint64_t d = pack_16_neon(src + 48);

  return a | (b << 16) | (c << 32) | (d << 48);
}

#endif

#include <cstdint>
#include <cstring>

#if defined(__aarch64__)

// Requires every input byte to be exactly 0 or 1.
// Packs src[i] into output bit i.
// Target: little-endian ARM64, such as Apple M4.
static inline uint64_t pack_64_fast(const uint8_t *src) noexcept {
  constexpr uint64_t Magic = 0x0102040810204080ULL;

  uint64_t x0, x1, x2, x3, x4, x5, x6, x7;

  // Each memcpy should compile to an efficient 64-bit load with -O3.
  std::memcpy(&x0, src + 0, 8);
  std::memcpy(&x1, src + 8, 8);
  std::memcpy(&x2, src + 16, 8);
  std::memcpy(&x3, src + 24, 8);
  std::memcpy(&x4, src + 32, 8);
  std::memcpy(&x5, src + 40, 8);
  std::memcpy(&x6, src + 48, 8);
  std::memcpy(&x7, src + 56, 8);

  const uint64_t b0 = (x0 * Magic) >> 56;
  const uint64_t b1 = (x1 * Magic) >> 56;
  const uint64_t b2 = (x2 * Magic) >> 56;
  const uint64_t b3 = (x3 * Magic) >> 56;
  const uint64_t b4 = (x4 * Magic) >> 56;
  const uint64_t b5 = (x5 * Magic) >> 56;
  const uint64_t b6 = (x6 * Magic) >> 56;
  const uint64_t b7 = (x7 * Magic) >> 56;

  return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24) | (b4 << 32) | (b5 << 40) |
         (b6 << 48) | (b7 << 56);
}

#endif

static void pack_bytes_to_bits_parallel_old_past(const uint8_t *src,
                                                 uint64_t *dst,
                                                 size_t total_voxels) {
  const size_t full_words = total_voxels / 64;
  const size_t tail_size = total_voxels % 64;

  // Each iteration packs 64 input bytes into one uint64_t.
  const auto pack_range = [src, dst](size_t begin, size_t end) noexcept {
    for (size_t word = begin; word < end; ++word) {
      dst[word] = pack_64_fast(src + word * 64);
    }
  };

  // 16384 words correspond to 1 MiB of input.
  // constexpr size_t MinWordsPerBlock = 1u << 14;
  constexpr size_t MinWordsPerBlock = 1u << 18;

  const size_t thread_count =
      std::max<size_t>(1, skel::pool.get_thread_count());

  // Avoid parallel scheduling overhead for small volumes.
  const size_t block_count =
      std::min(thread_count, full_words / MinWordsPerBlock);

  if (block_count >= 2) {
    auto completion = skel::pool.submit_blocks(size_t{0}, full_words,
                                               pack_range, block_count);

    completion.wait();
  } else {
    pack_range(0, full_words);
  }

  // Only the last incomplete word needs special handling.
  if (tail_size != 0) {
    dst[full_words] = pack_tail(src + full_words * 64, tail_size);
  }
}

static void pack_bytes_to_bits_parallel(const uint8_t *src, uint64_t *dst,
                                        size_t total_voxels) {
  const size_t full_words = total_voxels / 64;
  const size_t tail_size = total_voxels % 64;

  constexpr size_t WordsPerTask = size_t{1} << 18; // 16 MiB input

  const auto pack_range = [src, dst](size_t begin, size_t end) noexcept {
    size_t word = begin;

    // Four independent 64-byte input blocks per iteration.
    const size_t unrolled_end = begin + ((end - begin) & ~size_t{7});

    for (; word < unrolled_end; word += 8) {
      const uint8_t *p = src + word * 64;

      dst[word + 0] = pack_64_fast(p);
      dst[word + 1] = pack_64_fast(p + 64);
      dst[word + 2] = pack_64_fast(p + 128);
      dst[word + 3] = pack_64_fast(p + 192);
      dst[word + 4] = pack_64_fast(p + 256);
      dst[word + 5] = pack_64_fast(p + 320);
      dst[word + 6] = pack_64_fast(p + 384);
      dst[word + 7] = pack_64_fast(p + 448);
    }

    for (; word < end; ++word) {
      dst[word] = pack_64_fast(src + word * 64);
    }
  };

  const size_t thread_count =
      std::max<size_t>(1, skel::pool.get_thread_count());

  const size_t task_count = std::min(thread_count, full_words / WordsPerTask);

  if (task_count >= 2) {
    auto completion =
        skel::pool.submit_blocks(size_t{0}, full_words, pack_range, task_count);
    completion.wait();
  } else {
    pack_range(0, full_words);
  }

  if (tail_size != 0) {
    dst[full_words] = pack_tail(src + full_words * 64, tail_size);
  }
}

static void unpack_bits_to_bytes_parallel(const uint64_t *src, uint8_t *dst,
                                          size_t total_voxels) {
  const size_t full_words = total_voxels / 64;
  const size_t tail_size = total_voxels % 64;

  // Each task writes about 16 MiB of output.
  constexpr size_t WordsPerTask = size_t{1} << 18;

  const auto unpack_range = [src, dst](size_t begin, size_t end) noexcept {
    size_t word = begin;
    const size_t unrolled_end = begin + ((end - begin) & ~size_t{7});

    for (; word < unrolled_end; word += 8) {
      unpack_64_fast(src[word + 0], dst + (word + 0) * 64);
      unpack_64_fast(src[word + 1], dst + (word + 1) * 64);
      unpack_64_fast(src[word + 2], dst + (word + 2) * 64);
      unpack_64_fast(src[word + 3], dst + (word + 3) * 64);
      unpack_64_fast(src[word + 4], dst + (word + 4) * 64);
      unpack_64_fast(src[word + 5], dst + (word + 5) * 64);
      unpack_64_fast(src[word + 6], dst + (word + 6) * 64);
      unpack_64_fast(src[word + 7], dst + (word + 7) * 64);
    }

    for (; word < end; ++word) {
      unpack_64_fast(src[word], dst + word * 64);
    }
  };

  const size_t thread_count =
      std::max<size_t>(1, skel::pool.get_thread_count());

  const size_t task_count = std::min(thread_count, full_words / WordsPerTask);

  if (task_count >= 2) {
    auto completion = skel::pool.submit_blocks(size_t{0}, full_words,
                                               unpack_range, task_count);
    completion.wait();
  } else {
    unpack_range(0, full_words);
  }

  // The only partial word is the final word.
  if (tail_size != 0) {
    const uint64_t word = src[full_words];

    for (size_t b = 0; b < tail_size; ++b) {
      dst[full_words * 64 + b] = static_cast<uint8_t>((word >> b) & 1u);
    }
  }
}

static void pack_bytes_to_bits_parallel_newscheduler(const uint8_t *src,
                                                     uint64_t *dst,
                                                     size_t total_voxels) {
  const size_t full_words = total_voxels / 64;
  const size_t tail_size = total_voxels % 64;

  constexpr size_t MinWordsPerTask = size_t{1} << 18;

  const auto pack_range = [src, dst](size_t begin, size_t end) noexcept {
    /*
    size_t word = begin;

    // Unroll by four.
    const size_t unrolled_end = begin + ((end - begin) & ~size_t{3});

    for (; word < unrolled_end; word += 4) {
      const uint8_t *p = src + word * 64;
      dst[word + 0] = pack_64_fast(p);
      dst[word + 1] = pack_64_fast(p + 64);
      dst[word + 2] = pack_64_fast(p + 128);
      dst[word + 3] = pack_64_fast(p + 192);
    }

    for (; word < end; ++word) {
      dst[word] = pack_64_fast(src + word * 64);
    }
    */
    size_t word = begin;
    const size_t unrolled_end = begin + ((end - begin) & ~size_t{7});

    for (; word < unrolled_end; word += 8) {
      pack_64_fast(src + (word + 0) * 64);
      pack_64_fast(src + (word + 1) * 64);
      pack_64_fast(src + (word + 2) * 64);
      pack_64_fast(src + (word + 3) * 64);
      pack_64_fast(src + (word + 4) * 64);
      pack_64_fast(src + (word + 5) * 64);
      pack_64_fast(src + (word + 6) * 64);
      pack_64_fast(src + (word + 7) * 64);
    }

    for (; word < end; ++word) {
      pack_64_fast(src + word * 64);
    }
  };

  const size_t threads = std::max<size_t>(1, skel::pool.get_thread_count());

  const size_t useful_tasks =
      (full_words + MinWordsPerTask - 1) / MinWordsPerTask;

  const size_t task_count = std::min(threads, useful_tasks);

  if (task_count >= 2) {
    std::vector<std::future<void>> futures;
    futures.reserve(task_count);

    for (size_t t = 0; t < task_count; ++t) {
      const size_t begin = full_words * t / task_count;
      const size_t end = full_words * (t + 1) / task_count;

      futures.emplace_back(
          skel::pool.submit_task([=, &pack_range] { pack_range(begin, end); }));
    }

    for (auto &f : futures) {
      f.get();
    }
  } else {
    pack_range(0, full_words);
  }

  if (tail_size != 0) {
    dst[full_words] = pack_tail(src + full_words * 64, tail_size);
  }
}

std::tuple<uint32_t, uint32_t, uint32_t, uint8_t *>
verification_checkup(py::array_t<uint8_t> volume);

void skeletonize_inplace(py::array_t<uint8_t> volume);

void skeletonize_inplace_bits_past(py::array_t<uint8_t> volume);

void skeletonize_inplace_bits(py::array_t<uint8_t> volume);

#if defined(__AVX2__)
    #include <immintrin.h>
#elif defined(__x86_64__) || defined(_M_X64)
    #include <emmintrin.h>
#endif
static inline uint64_t pack_64_avx(const uint8_t* src) noexcept {
#if defined(__AVX2__)

    // AVX2: pack 32 bytes into 32 bits, twice.
    const __m256i zero = _mm256_setzero_si256();

    const __m256i v0 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(src));

    const __m256i v1 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(src + 32));

    const uint32_t lo = ~static_cast<uint32_t>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, zero)));

    const uint32_t hi = ~static_cast<uint32_t>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, zero)));

    return uint64_t{lo} | (uint64_t{hi} << 32);

#elif defined(__x86_64__) || defined(_M_X64)

    // SSE2: baseline x86-64, pack 16 bytes per operation.
    const __m128i zero = _mm_setzero_si128();

    const __m128i v0 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(src +  0));
    const __m128i v1 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(src + 16));
    const __m128i v2 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(src + 32));
    const __m128i v3 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(src + 48));

    const uint16_t b0 = static_cast<uint16_t>(
        ~_mm_movemask_epi8(_mm_cmpeq_epi8(v0, zero)));
    const uint16_t b1 = static_cast<uint16_t>(
        ~_mm_movemask_epi8(_mm_cmpeq_epi8(v1, zero)));
    const uint16_t b2 = static_cast<uint16_t>(
        ~_mm_movemask_epi8(_mm_cmpeq_epi8(v2, zero)));
    const uint16_t b3 = static_cast<uint16_t>(
        ~_mm_movemask_epi8(_mm_cmpeq_epi8(v3, zero)));

    return uint64_t{b0}
         | (uint64_t{b1} << 16)
         | (uint64_t{b2} << 32)
         | (uint64_t{b3} << 48);

#else

    // Portable SWAR fallback.
    // Requires little-endian and input bytes exactly 0 or 1.
    constexpr uint64_t Magic = 0x0102040810204080ULL;

    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;

    std::memcpy(&x0, src +  0, 8);
    std::memcpy(&x1, src +  8, 8);
    std::memcpy(&x2, src + 16, 8);
    std::memcpy(&x3, src + 24, 8);
    std::memcpy(&x4, src + 32, 8);
    std::memcpy(&x5, src + 40, 8);
    std::memcpy(&x6, src + 48, 8);
    std::memcpy(&x7, src + 56, 8);

    return ((x0 * Magic) >> 56)
         | (((x1 * Magic) >> 56) <<  8)
         | (((x2 * Magic) >> 56) << 16)
         | (((x3 * Magic) >> 56) << 24)
         | (((x4 * Magic) >> 56) << 32)
         | (((x5 * Magic) >> 56) << 40)
         | (((x6 * Magic) >> 56) << 48)
         | (((x7 * Magic) >> 56) << 56);

#endif
}
