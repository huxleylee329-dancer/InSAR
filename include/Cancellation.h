#pragma once

// Callbacks cross DLL boundaries, so they deliberately use only C-style
// function pointers and caller-owned opaque context. They may be invoked by
// OpenMP worker threads and must not access Qt or other thread-local state.
// Implementations must not throw exceptions through these callbacks. The
// project is built as C++14, where noexcept cannot appear in a typedef.
using IsCancelledCallback = bool (__stdcall *)(void* context);
using InSARProgressCallback = void (__stdcall *)(void* context, int progress,
                                                  const char* message);

// Long-running APIs use these values. A cancelled operation must not begin a
// new output operation after observing kOperationCancelled.
constexpr int kOperationSucceeded = 0;
constexpr int kOperationFailed = -1;
constexpr int kOperationCancelled = -2;

// Each cancellable API documents its polling boundary in its implementation.
// A single third-party or HDF5 call is allowed to finish, but the next read,
// write, export, or processing stage must first test IsCancelledCallback.
//
// Current polling contracts:
// - PSI candidate/mask and result filtering: 512-1024 pixels or points.
// - PSI network: each mask row, Delaunay insertion, and generated edge.
// - PSI phase/time series: 1024 samples, 64 periodogram candidates, or one
//   BFS node.
// - SBAS: one image pair, block, OpenMP row chunk, HDF5 operation (or its
//   tightly coupled metadata batch), or export.
// - BM3D: one reference-patch row; DBC: one box-size iteration; ONNX: one
//   inference call or one batch image.
