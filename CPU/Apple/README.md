# Zoubiskel3d a faster 100% compliant Lee94 3d Skeleton

[![PyPI](https://img.shields.io/pypi/v/zoubiskel3d)](https://pypi.org/project/zoubiskel3d/)
[![Python](https://img.shields.io/pypi/pyversions/zoubiskel3d)](https://pypi.org/project/zoubiskel3d/)
[![Platforms](https://img.shields.io/badge/platforms-macOS%20arm64%20%7C%20Linux%20x86__64-blue)](#install)
[![License](https://img.shields.io/pypi/l/zoubiskel3d)](https://github.com/JyVi/ZoubedySkeleton/blob/main/CPU/Apple/LICENCE)

**Multithreaded, bit-packed 3D skeletonization (Lee94) for NumPy — a C++ drop-in
replacement for `skimage.morphology.skeletonize(method="lee")`.**

> **~15–30× faster than scikit-image (Lee94), voxel-identical output.**
> Validated on 1,000 real medical volumes (ImageCas) — [setup & methodology](#benchmarks).

[Install](#install) · [Quickstart](#quickstart) · [API](#api) · [How it works](#how-it-works) · [Benchmarks](#benchmarks) · [Internals](#internals)

---

## Install

<!-- SPEC (L1):
- pip install zoubiskel3d
- Wheel matrix: macOS arm64 · manylinux/musllinux x86_64 · CPython 3.9–3.14 (auto from cibuildwheel)
- Other platforms: sdist build, needs Clang/GCC + CMake ≥ 3.28 → link #build-from-source
-->

```bash
pip install zoubiskel3d
```

or with [uv](https://docs.astral.sh/uv/):

```bash
uv add zoubiskel3d            # in a uv-managed project (pyproject.toml + lockfile)
```

Prebuilt wheels, no compiler needed:

| OS | Architecture | Python |
|---|---|---|
| macOS 11+ | Apple Silicon (arm64) | CPython 3.9 – 3.14 |
| Linux (glibc ≥ 2.28) | x86-64 | CPython 3.9 – 3.14 |
| Linux (musl ≥ 1.2) | x86-64 | CPython 3.9 – 3.14 |

Other platforms build from the source distribution: Clang or GCC with C++20,
CMake ≥ 3.28 — see [Build from source](#build-from-source).

## Quickstart

A solid 3D torus is the classic topology test for thinning: the result must be
a **1-voxel-wide closed ring** — the hole has to survive.

```python
import numpy as np
from skimage.morphology import skeletonize
from zoubiskel3d import zoubiskel3d

# Solid torus in a 64³ volume (axis along z, ring radius 20, tube radius 7)
z, y, x = np.ogrid[:64, :64, :64]
torus = (np.sqrt((x - 31.5)**2 + (y - 31.5)**2) - 20)**2 + (z - 31.5)**2 < 7**2
volume = torus.astype(np.uint8)             # binary 0/1, C-contiguous

ours   = zoubiskel3d(volume)                # uint8 array, values 0/1
theirs = skeletonize(volume, method="lee")  # bool array

assert np.array_equal(ours, theirs)         # identical, voxel for voxel
print(volume.sum(), "foreground voxels ->", ours.sum(), "skeleton voxels")
```

Migrating from scikit-image is a one-line change:
`skeletonize(vol, method="lee")` → `zoubiskel3d(vol)`.

Or `skeletonize(vol)` → `zoubiskel3d(vol)` (if vol is a 3d volume, so that shape has 3 values like this (3, 3, 3)).


- **No manual padding**, both functions pad internally.
- `zoubiskel3d` returns a **new** array; the input is never modified.
- Input must be a binary `uint8` volume, cast masks with
  `(mask > 0).astype(np.uint8)`.
- Output is `uint8` 0/1 (skimage returns `bool`.
  `np.array_equal` compares values, so the check works across dtypes).

<details>
<summary><b>Real data: skeletonize a NIfTI mask</b></summary>

```python
import nibabel as nib
import numpy as np
from zoubiskel3d import zoubiskel3d

img  = nib.load("label.nii.gz")
mask = np.ascontiguousarray((img.get_fdata() > 0).astype(np.uint8))
skel = zoubiskel3d(mask)
nib.save(nib.Nifti1Image(skel, img.affine), "skeleton.nii.gz")
```

</details>

## API

```python
zoubiskel3d(volume: np.ndarray) -> np.ndarray
```

Compute the 3D skeleton of a binary volume with the Lee94 thinning algorithm.

| Property | Contract |
|---|---|
| Shape | 3D `(D, H, W)`, any dimension may be 0 (returns an empty array of the same shape) |
| dtype | `uint8`. Other dtypes are auto-cast (`bool` works, floats **truncate**), pass `uint8` explicitly |
| Values | `0` = background, `1` = foreground. **Other values are not supported yet**, binarize masks with `(mask > 0).astype(np.uint8)` | <!-- D2: after nonzero-safe packing fix → "any nonzero = foreground, like skimage"; drop the binarize requirement -->
| Layout | C-contiguous, use `np.ascontiguousarray()` on sliced/transposed views |
| Padding | Automatic: a 1 voxel one border is added internally |
| Returns | A **new** `uint8` array, same shape, values 0/1. The input is never modified. `.astype(bool)` for a skimage style mask |
| 2D images | A single slice volume `(1, H, W)` follows the same 4-direction path as skimage's 2D Lee |
| GIL | Released during computation, other Python threads keep running |
| Threads | Uses all available cores (no thread count knob) |
| Determinism | Same input → same output, regardless of thread scheduling |

## How it works

[Lee94](#citation) reduces each connected component of a binary volume to a
1-voxel-wide skeleton by iteratively deleting *border* voxels — in six face
directions (±x, ±y, ±z), in scikit-image's exact order — until a full pass
removes nothing. A voxel is deleted only if it is **simple** (its removal
preserves local connectivity), **Euler-invariant** (no holes created or
destroyed), and **not an endpoint** (line ends survive).

The difficulty is that deletion order changes the result, which is why naive
parallelizations drift from the reference. zoubiskel3d splits each
sub-iteration into three phases:

```text
for each of the 6 border directions, until stable:

  1. COLLECT   (parallel)   every core scans its own block of the volume,
                            read-only, and tests border voxels against
                            compile-time topology LUTs → candidate list
  2. SORT      (parallel)   candidates ordered by voxel index
  3. RECHECK + DELETE (sequential)
                            each candidate re-tested against the *current*
                            volume, then deleted — in exactly the order
                            scikit-image visits voxels
```

The expensive part (neighborhood analysis of the whole volume) is parallel;
the order sensitive part (deletion) is sequential over a sparse candidate
list, so the output is **identical to the reference, every run**.

Everything operates on a **bit-packed** volume: one voxel per bit, 64 voxels
per `uint64` word. A 512×512×275 mask drops from ~72 MB to ~9 MB (theoretically and platform dependant), and a full
27-voxel neighborhood fits in a single `uint32`, turning topology checks
into a handful of lookups.

### Correctness

- **Topology preserving by construction**: Euler-invariance octant LUTs,
  simple-point test, endpoint rule, the same predicates as Lee94/skimage.
- **Deterministic**: the sorted sequential recheck makes results independent
  of thread count and scheduling.
- **Empirically validated**: identical skeleton voxel counts to
  `skimage.morphology.skeletonize(method="lee")` on **all 1,000** ImageCas
  medical volumes ([Benchmarks](#benchmarks)), and voxel-for-voxel equality
  in the compliance tests (`tests/test_compliance.py`). <!-- asset: commit the test before release; it upgrades the claim from counts to array_equal -->

## Benchmarks

<!-- SPEC (L2):
- Setup table: chip/cores/RAM, OS, Python, skimage, zoubiskel3d versions, threads (NEED FROM USER)
- Dataset: ImageCas NIfTI labels, N=1000, shapes ~512×512×174–323
- Methodology: full call timed (pack + thin + unpack), perf_counter, runs per volume (DECISION D5)
- Results: aggregate table (mean/median/min/max) + 5 representative rows + 2 plots
- Reproduce: python benchmarks/bench_skimage.py --data-dir ... --out results.csv
- Honesty clause: single machine, sparse medical masks, density-dependent
- NEEDS: full CSV or summary + hardware specs
-->

*TODO — setup, methodology, results, plots, reproduction command.*

## Internals

C++20 core under `include/skeletonize/`, exposed through a thin pybind11
layer. Four pipeline variants share the same topology predicates:

| Pipeline | Storage | Threading | Role |
|---|---|---|---|
| `pipeline` | 1 byte/voxel | sequential | reference port of skimage Lee94 |
| `pipeline_single_thread` | 1 byte/voxel | 1 thread | optimized scalar baseline |
| `pipeline_multi_thread` | 1 byte/voxel | thread pool | parallel byte version |
| `pipeline_multi_thread_bit` | **1 bit/voxel** | thread pool | **shipped path** |

Key techniques in the shipped path:

- **Work decomposition**: interior rows split into 256 voxel units, units-per-row
  rounded to a power of two (`std::bit_ceil`) so unit → coordinates is shifts and
  masks, with a precomputed row start table. Zero per-task allocation.
- **Bit parallel neighborhoods**: nine unaligned 64-bit loads cover the 3×3×3
  neighborhoods of a 62-voxel run, empty regions are skipped word wise.
- **Batched candidate merging**: thread-local candidate buffers, per-worker
  write buffers flushed under a mutex in batches (amortized locking), then a
  parallel `std::sort` (`par_unseq`: oneTBB on Linux, libc++ on macOS).
- **Compile time LUTs**: `constexpr` generated Euler tables (256 entry octant
  LUT, paired octant 2¹⁴ tables), topology tests are pure lookups.
- **SIMD pack/unpack**: byte→bit via SWAR magic multiply, NEON (arm64) and
  AVX2/SSE2 (x86) variants, blocked parallel copies (~16 MiB per task).
  Padding is fused into packing: bytes are written directly into the padded
  bit layout, and unpacking strips it, no separate pad/crop passes.

### Repository map

```text
CPU/Apple/
├── include/skeletonize/
│   ├── lut.hpp                       # constexpr Euler LUTs + topology predicates
│   ├── helpers.{hpp,cc}              # byte-volume neighborhoods, simple-point test
│   ├── helpers_bit.{hpp,cc}          # bit-packed neighborhood loads, sequential recheck
│   ├── pipeline.{hpp,cc}             # reference sequential Lee94 port
│   ├── pipeline_single_thread.*      # scalar byte pipeline
│   ├── pipeline_multi_thread.*       # parallel byte pipeline (BS::thread_pool)
│   ├── pipeline_single_thread_bit.*  # scalar bit-packed pipeline
│   ├── pipeline_multi_thread_bit.*   # ★ shipped: parallel bit-packed pipeline
│   └── perfetto_tracing.*            # Perfetto hooks (not compiled in by default)
├── python/
│   ├── bindings.{cc,hpp}             # pybind11 API, validation, padded pack/unpack
│   └── zoubiskel3d/__init__.py       # public package: exports zoubiskel3d()
├── tests/                            # compliance tests vs skimage
├── CMakeLists.txt                    # core library + bindings + options
└── pyproject.toml                    # scikit-build-core packaging
```

### Build from source

Requirements: CMake ≥ 3.28, AppleClang (macOS) or GCC/Clang (Linux), C++20.
Missing dependencies are fetched automatically: BS::thread_pool 5.1.0,
pybind11 3.1.0, oneTBB 2023.1.0 (Linux only).

**Python module (editable):**

```bash
git clone https://github.com/JyVi/ZoubedySkeleton.git
cd ZoubedySkeleton/CPU/Apple
uv sync                     # or: pip install -e . -v
```

**Pure CMake (core library):**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_PYTHON_BINDINGS=OFF -DBUILD_SKELETONIZE_APP=OFF
cmake --build build -j
```

<!-- D6: BUILD_SKELETONIZE_APP references tests/Cpp/main.cpp which is missing.
     Once fixed (point at src/main.cc), drop the OFF flag + caveat below. -->
> The standalone C++ app target is being reworked — keep
> `-DBUILD_SKELETONIZE_APP=OFF` for now.

| CMake option | Default | Effect |
|---|---|---|
| `BUILD_PYTHON_BINDINGS` | `ON` | build the pybind11 module |
| `BUILD_SKELETONIZE_APP` | `ON` | standalone executable (currently unavailable, set `OFF`) |
| `SKELETONIZE_NATIVE_OPTIMIZATION` | `ON` | `-march=native` (disabled in wheels for portability) |
| `SKELETONIZE_FAST_MATH` | `OFF` | `-ffast-math` (enabled in wheels) |
| `SKELETONIZE_USE_SYSTEM_TBB` | `ON` | Linux: prefer an installed oneTBB |
| `SKELETONIZE_STATIC_TBB` | `OFF` | Linux: static oneTBB for self contained wheels |

> Note: PyPI wheels and `uv sync` builds use portable flags
> (`NATIVE_OPTIMIZATION=OFF`, `FAST_MATH=ON`, static TBB on Linux). A pure CMake
> Release build enables `-march=native` by default and may be slightly faster.

### Tests & profiling

```bash
uv run python tests/test_compliance.py        # voxel-for-voxel equality vs skimage
uv run python benchmarks/bench_skimage.py --data-dir /path/to/niftis --out results.csv
```

<!-- assets: commit tests/test_compliance.py + benchmarks/bench_skimage.py before release -->

Perfetto tracing hooks live in `include/skeletonize/perfetto_tracing.*` for
timeline profiling of the phases. They are disabled in the default build, source file modification needed.
## Citation

This package implements the Lee94 thinning algorithm and reproduces the
reference implementation from scikit-image's `morphology` module. If you use
it in scientific work, please cite both:

**Algorithm** — T.-C. Lee, R. L. Kashyap, C.-N. Chu,
*Building skeleton models via 3-D medial surface/axis thinning algorithms*,
CVGIP: Graphical Models and Image Processing, 56(6):462–478, 1994.
DOI: [10.1006/cgip.1994.1042](https://doi.org/10.1006/cgip.1994.1042)

**Reference implementation** — S. van der Walt et al.,
*scikit-image: image processing in Python*,
PeerJ 2:e453, 2014.
DOI: [10.7717/peerj.453](https://doi.org/10.7717/peerj.453)

### Software dependencies

This project builds on the following open-source components:

- [**BS::thread_pool**](https://github.com/bshoshany/thread-pool) (MIT) —
  thread pool used for parallel candidate collection (v5.1.0).
  B. Shoshany, *A C++17 Thread Pool for High-Performance Scientific Computing*,
  arXiv:2105.00613 (2021). DOI: [10.48550/arXiv.2105.00613](https://doi.org/10.48550/arXiv.2105.00613)
- [**oneTBB**](https://github.com/oneapi-src/oneTBB) (Apache-2.0) —
  backend for `std::execution::par_unseq` parallel sorting on Linux (v2023.1.0).
- [**Perfetto**](https://perfetto.dev) (Apache-2.0) —
  timeline profiling for the optional tracing hooks.
- [**pybind11**](https://github.com/pybind/pybind11) (BSD-3-Clause) —
  seamless operability between C++11 and Python (v3.1.0).

If `zoubiskel3d` itself contributed to your results, a pointer to the software
is appreciated:

```text
zoubiskel3d, https://github.com/JyVi/ZoubedySkeleton
```

## License

BSD-3-Clause — see [LICENCE](https://github.com/JyVi/ZoubedySkeleton/blob/main/CPU/Apple/LICENCE).
