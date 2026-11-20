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

#include "bindings.hpp"
#include "skeletonize/lut.hpp"
// #include "skeletonize/perfetto_tracing.hpp"
#include "skeletonize/pipeline.hpp"
#include "skeletonize/pipeline_multi_thread.hpp"
#include "skeletonize/pipeline_multi_thread_bit.hpp"
#include "skeletonize/pipeline_single_thread.hpp"
#include "skeletonize/pipeline_single_thread_bit.hpp"

namespace py = pybind11;

std::tuple<uint32_t, uint32_t, uint32_t, uint8_t *>
verification_checkup(py::array_t<uint8_t> volume) {
  if (volume.ndim() != 3) {
    throw std::invalid_argument("Volume must be a 3D NumPy array (D, H, W)");
  }

  const py::buffer_info info = volume.request();

  if (info.strides[2] != static_cast<py::ssize_t>(sizeof(uint8_t)) ||
      info.strides[1] != info.shape[2] * sizeof(uint8_t) ||
      info.strides[0] != info.shape[1] * info.shape[2] * sizeof(uint8_t)) {
    throw std::invalid_argument(
        "Volume must be C-contiguous. Use np.ascontiguousarray().");
  }

  for (int axis = 0; axis < 3; ++axis) {
    if (info.shape[axis] < 0 || static_cast<uint64_t>(info.shape[axis]) >
                                    std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("Volume dimension exceeds uint32_t");
    }
  }

  const auto D = static_cast<uint32_t>(info.shape[0]);
  const auto H = static_cast<uint32_t>(info.shape[1]);
  const auto W = static_cast<uint32_t>(info.shape[2]);

  const size_t d = D;
  const size_t h = H;
  const size_t w = W;

  if (h != 0 && d > std::numeric_limits<size_t>::max() / h) {
    throw std::overflow_error("Volume dimensions overflow size_t");
  }

  const size_t dh = d * h;

  if (w != 0 && dh > std::numeric_limits<size_t>::max() / w) {
    throw std::overflow_error("Volume dimensions overflow size_t");
  }
  return std::tuple<uint32_t, uint32_t, uint32_t, uint8_t *>(
      D, H, W, static_cast<uint8_t *>(info.ptr));
}

void skeletonize_inplace(py::array_t<uint8_t> volume) {
  const auto [D, W, H, data] = verification_checkup(volume);
  std::span<uint8_t> img_span(data, D * H * W);

  // skel::compute_thin_image(img_span, D, H, W);
  skel::compute_thin_image_multi_threaded(img_span, D, H, W);
  // skel::compute_thin_image_single_threaded(img_span, D, H, W);
}

void skeletonize_inplace_bits_past(py::array_t<uint8_t> volume) {
  const auto [D, W, H, data] = verification_checkup(volume);
  const size_t total_voxels = D * H * W;
  const size_t word_count = total_voxels / 64 + (total_voxels % 64 != 0);

  auto *src = static_cast<const uint8_t *>(data);
  auto *dst = data;

  // Pack: voxel i maps to bit (i % 64) of word (i / 64).
  std::vector<uint64_t> packed(word_count, uint64_t{0});
  const auto pack_start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < total_voxels; ++i) {
    if (src[i] != 0) {
      packed[i >> 6] |= uint64_t{1} << (i & 63);
    }
  }
  const auto pack_end = std::chrono::steady_clock::now();

  const double pack_ms =
      std::chrono::duration<double, std::milli>(pack_end - pack_start).count();

  std::cout << "uint8 -> bit-packed copy: " << pack_ms << " ms\n";

  // The packed implementation expects a span of uint64_t words.
  {
    py::gil_scoped_release release;
    skel::compute_thin_image_multi_threaded_bit(std::span<uint64_t>(packed), D,
                                                H, W);
    // skel::compute_thin_image_single_threaded_bit(std::span<uint64_t>(packed),
    // D,
    //                                              H, W);
  }

  const auto unpack_start = std::chrono::steady_clock::now();
  // Unpack the result back into the original NumPy array.
  for (size_t i = 0; i < total_voxels; ++i) {
    dst[i] = static_cast<uint8_t>((packed[i >> 6] >> (i & 63)) & uint64_t{1});
  }
  const auto unpack_end = std::chrono::steady_clock::now();

  const double unpack_ms =
      std::chrono::duration<double, std::milli>(unpack_end - unpack_start)
          .count();

  std::cout << "bit-packed -> uint8 copy: " << unpack_ms << " ms\n";
}

void skeletonize_inplace_bits(py::array_t<uint8_t> volume) {
  const auto [D, H, W, data] = verification_checkup(volume);

  const size_t total_voxels = D * H * W;
  const size_t word_count = total_voxels / 64 + (total_voxels % 64 != 0);

  if (total_voxels == 0) {
    return;
  }

  const auto pack_start = std::chrono::steady_clock::now();
  {
    py::gil_scoped_release release;
    std::unique_ptr<uint64_t[]> packed(new uint64_t[word_count]);
    size_t i = 0;

    pack_bytes_to_bits_parallel_newscheduler(data, packed.get(), total_voxels);

    const auto pack_end = std::chrono::steady_clock::now();

    std::cout << "uint8 -> bit-packed copy: "
              << std::chrono::duration<double, std::milli>(pack_end -
                                                           pack_start)
                     .count()
              << " ms\n";

    // Run the existing bit-packed skeletonization implementation.
    skel::compute_thin_image_multi_threaded_bit(
        std::span<uint64_t>(packed.get(), word_count), D, H, W);

    // Unpack: one uint64_t -> 64 output voxels.
    const auto unpack_start = std::chrono::steady_clock::now();

    unpack_bits_to_bytes_parallel(packed.get(), data, total_voxels);

    const auto unpack_end = std::chrono::steady_clock::now();

    std::cout << "bit-packed -> uint8 copy: "
              << std::chrono::duration<double, std::milli>(unpack_end -
                                                           unpack_start)
                     .count()
              << " ms\n";
  }
}

static void pack_bytes_to_bits_padded_fast(const uint8_t *src, uint64_t *dst,
                                           size_t D, size_t H, size_t W,
                                           size_t D_pad, size_t H_pad,
                                           size_t W_pad) {
  const size_t H_pad_W_pad = H_pad * W_pad;
  constexpr size_t DepthsPerTask = 16;

  const auto pack_depth = [&](size_t d_begin, size_t d_end) noexcept {
    for (size_t d = d_begin; d < d_end; ++d) {
      size_t L_base_d = (d + 1) * H_pad_W_pad;
      const uint8_t *src_plane = src + d * H * W;

      for (size_t h = 0; h < H; ++h) {
        size_t L_start = L_base_d + (h + 1) * W_pad + 1;
        size_t word_idx = L_start >> 6;
        size_t bit_offset = L_start & 63;

        const uint8_t *src_row = src_plane + h * W;
        size_t full_chunks = W / 64;

        if (bit_offset == 0) {
          for (size_t i = 0; i < full_chunks; ++i) {
            dst[word_idx + i] = pack_64_fast(src_row + i * 64);
          }
        } else {
          size_t shift_right = 64 - bit_offset;
          for (size_t i = 0; i < full_chunks; ++i) {
            uint64_t val = pack_64_fast(src_row + i * 64);
            dst[word_idx + i] |= (val << bit_offset);
            dst[word_idx + i + 1] |= (val >> shift_right);
          }
        }

        size_t tail_size = W % 64;
        if (tail_size != 0) {
          uint64_t val = pack_tail(src_row + full_chunks * 64, tail_size);
          dst[word_idx + full_chunks] |= (val << bit_offset);
          if (bit_offset != 0 && tail_size + bit_offset > 64) {
            dst[word_idx + full_chunks + 1] |= (val >> (64 - bit_offset));
          }
        }
      }
    }
  };

  const size_t threads = std::max<size_t>(1, skel::pool.get_thread_count());
  const size_t task_count = std::min(threads, D / DepthsPerTask);

  if (task_count >= 2) {
    std::vector<std::future<void>> futures;
    futures.reserve(task_count);
    for (size_t t = 0; t < task_count; ++t) {
      size_t begin = D * t / task_count;
      size_t end = D * (t + 1) / task_count;
      futures.emplace_back(
          skel::pool.submit_task([=, &pack_depth] { pack_depth(begin, end); }));
    }
    for (auto &f : futures)
      f.get();
  } else {
    pack_depth(0, D);
  }
}

static void unpack_bits_to_bytes_padded_fast(const uint64_t *src, uint8_t *dst,
                                             size_t D, size_t H, size_t W,
                                             size_t D_pad, size_t H_pad,
                                             size_t W_pad) {
  const size_t H_pad_W_pad = H_pad * W_pad;
  constexpr size_t DepthsPerTask = 16;

  const auto unpack_depth = [&](size_t d_begin, size_t d_end) noexcept {
    for (size_t d = d_begin; d < d_end; ++d) {
      size_t L_base_d = (d + 1) * H_pad_W_pad;
      uint8_t *dst_plane = dst + d * H * W;

      for (size_t h = 0; h < H; ++h) {
        size_t L_start = L_base_d + (h + 1) * W_pad + 1;
        size_t word_idx = L_start >> 6;
        size_t bit_offset = L_start & 63;

        uint8_t *dst_row = dst_plane + h * W;
        size_t full_chunks = W / 64;

        if (bit_offset == 0) {
          for (size_t i = 0; i < full_chunks; ++i) {
            unpack_64_fast(src[word_idx + i], dst_row + i * 64);
          }
        } else {
          size_t shift_left = 64 - bit_offset;
          for (size_t i = 0; i < full_chunks; ++i) {
            uint64_t val = (src[word_idx + i] >> bit_offset) |
                           (src[word_idx + i + 1] << shift_left);
            unpack_64_fast(val, dst_row + i * 64);
          }
        }

        size_t tail_size = W % 64;
        if (tail_size != 0) {
          uint64_t val;
          if (bit_offset == 0) {
            val = src[word_idx + full_chunks];
          } else {
            val = (src[word_idx + full_chunks] >> bit_offset) |
                  (src[word_idx + full_chunks + 1] << (64 - bit_offset));
          }
          for (size_t b = 0; b < tail_size; ++b) {
            dst_row[full_chunks * 64 + b] =
                static_cast<uint8_t>((val >> b) & 1u);
          }
        }
      }
    }
  };

  const size_t threads = std::max<size_t>(1, skel::pool.get_thread_count());
  const size_t task_count = std::min(threads, D / DepthsPerTask);

  if (task_count >= 2) {
    std::vector<std::future<void>> futures;
    futures.reserve(task_count);
    for (size_t t = 0; t < task_count; ++t) {
      size_t begin = D * t / task_count;
      size_t end = D * (t + 1) / task_count;
      futures.emplace_back(skel::pool.submit_task(
          [=, &unpack_depth] { unpack_depth(begin, end); }));
    }
    for (auto &f : futures)
      f.get();
  } else {
    unpack_depth(0, D);
  }
}

py::array_t<uint8_t>
skeletonize_inplace_bits_return(py::array_t<uint8_t> volume) {
  const auto [D, H, W, data] = verification_checkup(volume);

  if (D == 0 || H == 0 || W == 0) {
    return py::array_t<uint8_t>({D, H, W});
  }

  const uint32_t D_pad = D + 2;
  const uint32_t H_pad = H + 2;
  const uint32_t W_pad = W + 2;

  const size_t total_voxels_pad = (size_t)D_pad * H_pad * W_pad;
  const size_t word_count_pad = (total_voxels_pad + 63) / 64;

  std::unique_ptr<uint64_t[]> packed(new uint64_t[word_count_pad]());

  py::array_t<uint8_t> result_volume({D, H, W});
  auto result_buf = result_volume.request();
  uint8_t *result_data = static_cast<uint8_t *>(result_buf.ptr);

  const auto pack_start = std::chrono::steady_clock::now();
  {
    py::gil_scoped_release release;

    pack_bytes_to_bits_padded_fast(data, packed.get(), D, H, W, D_pad, H_pad,
                                   W_pad);

    const auto pack_end = std::chrono::steady_clock::now();
    std::cout << "uint8 -> bit-packed copy (with padding): "
              << std::chrono::duration<double, std::milli>(pack_end -
                                                           pack_start)
                     .count()
              << " ms\n";

    skel::compute_thin_image_multi_threaded_bit(
        std::span<uint64_t>(packed.get(), word_count_pad), D_pad, H_pad, W_pad);

    const auto unpack_start = std::chrono::steady_clock::now();

    unpack_bits_to_bytes_padded_fast(packed.get(), result_data, D, H, W, D_pad,
                                     H_pad, W_pad);

    const auto unpack_end = std::chrono::steady_clock::now();
    std::cout << "bit-packed -> uint8 copy (with unpadding): "
              << std::chrono::duration<double, std::milli>(unpack_end -
                                                           unpack_start)
                     .count()
              << " ms\n";
  }

  return result_volume;
}

PYBIND11_MODULE(_skeletonize_core, m, py::mod_gil_not_used()) {
  m.doc() = "3D skeletonization using Lee94 thinning algorithm";

  // m.def("zoubiskel", &skeletonize_inplace,
  //       "Compute the 3D skeleton of a binary volume", py::arg("volume"));
  m.def("zoubiskel3d", &skeletonize_inplace_bits_return,
        "Compute the 3D skeleton of a binary volume", py::arg("volume"));

  /*
  // Expose the main function
  m.def("skeletonize_raw", &skel::compute_thin_image,
        "Compute the 3D skeleton of a binary volume", py::arg("volume"),
        py::arg("D"), py::arg("H"), py::arg("W"));

  // Expose debug/utility functions (optional)
  m.def("is_simple_point", &skel::is_simple_point,
        "Check if a packed neighborhood represents a simple point",
        py::arg("packed_neighborhood"));

  m.def("is_endpoint", &skel::is_endpoint,
        "Check if a packed neighborhood represents an endpoint",
        py::arg("packed_neighborhood"));

  m.def("is_euler_invariant", &skel::is_euler_invariant,
        "Check if removing a point preserves the Euler characteristic",
        py::arg("packed_neighborhood"));
  */
  /*
  m.def("init_tracing", &skel::init_tracing,
        "Initialize the Perfetto tracer (call once)");
  m.def("start_tracing", &skel::start_tracing,
        "Start recording trace, must be stop and giving an output filename.");
  m.def("stop_tracing", &skel::stop_tracing,
        "Stop recording and flush to disk to the filename in arg",
        py::arg("filename"));
        */
}
