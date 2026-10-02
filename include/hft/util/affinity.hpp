// Thread pinning and CPU topology.
//
// Why this file exists: a two-thread benchmark whose threads land on the
// same physical core, or on two SMT siblings sharing one core's
// execution resources, is not measuring the thing it claims to measure.
// It is measuring the core. On this repository's development machine --
// a Ryzen 5 1600, six physical cores presented as twelve logical
// processors -- picking "core 0" and "core 1" naively is fine, but
// picking "core 0" and "core 6" is two hyperthreads on ONE core, and the
// resulting number would be roughly half of what the hardware can do
// while looking entirely plausible.
//
// So the topology is discovered at runtime rather than assumed, and the
// benchmark prints what it actually achieved. A benchmark that cannot
// tell you where its threads ran is not a measurement.
//
// This is best-effort by design. Every function reports failure rather
// than throwing, pinning is allowed to fail on a locked-down host, and
// the caller is expected to carry on and say so. A benchmark that
// refuses to run without root is not a benchmark anyone will run.
//
// Nothing here is required for correctness. The SPSC ring is correct
// regardless of placement; pinning only affects what the numbers mean.

#pragma once

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>

namespace hft::util {

/// Logical processors visible to this process.
[[nodiscard]] std::size_t logical_processor_count() noexcept;

/// Physical cores visible to this process, counting an SMT pair once.
[[nodiscard]] std::size_t physical_core_count() noexcept;

/// The other logical processor on the same physical core as `logical`,
/// or `logical` itself if it has no sibling.
///
/// The value is looked up rather than computed as `logical + cores`,
/// because that identity only holds for a specific enumeration order and
/// guessing it is exactly the mistake described at the top of this file.
[[nodiscard]] std::size_t smt_sibling(std::size_t logical) noexcept;

/// True when `a` and `b` are two logical processors on one physical core.
[[nodiscard]] bool shares_physical_core(std::size_t a, std::size_t b) noexcept;

/// First logical processor that is NOT on the same physical core as
/// `logical`.
///
/// This is what a two-thread benchmark wants for its second thread. The
/// obvious alternatives are both wrong on at least one common machine:
/// `logical + 1` lands on an SMT sibling under an interleaved
/// enumeration, and `logical + physical_cores` assumes the
/// first-half/second-half layout. Asking is cheap and is correct on
/// both layouts.
///
/// Returns `logical` when there is no other physical core, so the caller
/// can detect a single-core host rather than quietly measuring two
/// threads on one core.
[[nodiscard]] std::size_t other_core(std::size_t logical) noexcept;

/// Pin the CALLING thread to a single logical processor.
///
/// A thread is pinned by mutating its own affinity mask, so this must
/// be called from the thread being placed -- which is why the benchmark
/// calls it as the first statement inside each thread rather than from
/// main before spawning.
///
/// Returns false if the OS refused, which is normal on a host without
/// the privileges to do so.
bool pin_current_thread(std::size_t logical) noexcept;

/// The logical processor this thread is currently pinned to, or
/// `logical_processor_count()` when it is not pinned.
[[nodiscard]] std::size_t current_logical_processor() noexcept;

/// One human-readable line describing the topology and this thread's
/// placement, for the benchmark to print verbatim.
///
/// The point is that the reader of a benchmark result can see the
/// placement without trusting it. A string is not a machine-readable
/// topology, and it is not meant to be: it is the audit trail.
///
/// Allocates, and is therefore not `noexcept`. A benchmark that cannot
/// describe its own environment is not producing a publishable number,
/// so this is on the path where a failure should be visible.
[[nodiscard]] std::string describe_affinity(const char* role);

/// Topology summary, e.g. "6 physical cores / 12 logical processors".
[[nodiscard]] std::string describe_topology();

}  // namespace hft::util