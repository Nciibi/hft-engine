#include "hft/util/affinity.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

namespace hft::util {
namespace {

/// The logical processors that make up one physical core. A single-core
/// machine and a machine with SMT disabled produce groups of one; the
/// distinction is what the benchmark needs and is why the groups are
/// discovered rather than inferred from the processor count.
using CoreGroup = std::vector<std::size_t>;

#if defined(_WIN32)

[[nodiscard]] CoreGroup group_from_mask(const KAFFINITY mask) noexcept {
    CoreGroup group;
    for (std::size_t bit = 0; bit < 64; ++bit) {
        if ((mask & (static_cast<KAFFINITY>(1) << bit)) != 0) {
            group.push_back(bit);
        }
    }
    return group;
}

/// Windows reports physical cores directly: `RelationProcessorCore`
/// records carry one processor mask per core, and both hyperthreads of
/// an SMT pair appear in the same mask. That is exactly the grouping
/// needed, and it is an OS query rather than an inference.
[[nodiscard]] std::vector<CoreGroup> detect_topology() {
    DWORD bytes = 0;
    ::GetLogicalProcessorInformation(nullptr, &bytes);
    if (bytes == 0) {
        return {};
    }

    // Allocated as the record type rather than as bytes so the buffer is
    // correctly aligned for the structures written into it.
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> records(
        (static_cast<std::size_t>(bytes) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)) + 1u);
    bytes = static_cast<DWORD>(records.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (::GetLogicalProcessorInformation(records.data(), &bytes) == FALSE) {
        return {};
    }

    // `GetLogicalProcessorInformation` returns an array of
    // SYSTEM_LOGICAL_PROCESSOR_INFORMATION, one entry per relationship,
    // each the same size -- so the stream is indexed uniformly and there
    // is no record-length arithmetic to get wrong.
    //
    // The MSVC SDK's version of this struct carries a `Size` member and
    // the MinGW one does not, but `Size` is documented to equal
    // sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) in both, so stepping
    // one entry at a time is correct on either toolchain. Only
    // `ProcessorMask` is read, which both layouts agree on.
    const std::size_t count = static_cast<std::size_t>(bytes) /
                              sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    std::vector<CoreGroup> groups;
    groups.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        if (records[index].Relationship != RelationProcessorCore) {
            continue;
        }
        CoreGroup group = group_from_mask(records[index].ProcessorMask);
        if (!group.empty()) {
            groups.push_back(std::move(group));
        }
    }

    std::sort(groups.begin(), groups.end(),
              [](const CoreGroup& a, const CoreGroup& b) { return a.front() < b.front(); });
    return groups;
}

[[nodiscard]] std::size_t count_logical() noexcept {
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwNumberOfProcessors);
}

bool pin(std::size_t logical) noexcept {
    if (logical >= 64) {
        return false;
    }
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << logical;
    return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0;
}

[[nodiscard]] std::size_t current_cpu() noexcept {
    return static_cast<std::size_t>(::GetCurrentProcessorNumber());
}

#else  // POSIX

/// Linux exposes topology through sysfs. `core_id` alone is ambiguous on
/// a multi-socket machine, so the package is read too: two sockets can
/// both have a core 0, and treating them as one core would put two
/// benchmark threads on two different CPUs while reporting them as
/// siblings.
[[nodiscard]] bool read_topology_file(std::size_t logical, const char* leaf,
                                      long* out) noexcept {
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%zu/topology/%s", logical, leaf);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        return false;
    }
    long value = -1;
    const int matched = std::fscanf(f, "%ld", &value);
    std::fclose(f);
    if (matched != 1) {
        return false;
    }
    *out = value;
    return true;
}

[[nodiscard]] std::vector<CoreGroup> detect_topology() {
    const std::size_t count = count_logical();
    std::vector<CoreGroup> groups;

    for (std::size_t logical = 0; logical < count; ++logical) {
        long package = -1;
        long core = -1;
        if (!read_topology_file(logical, "physical_package_id", &package) ||
            !read_topology_file(logical, "core_id", &core)) {
            // sysfs is not mounted, or this is a container with a
            // synthetic topology. Fall back to treating every logical
            // processor as its own core: the benchmark then reports that
            // it could not tell, which is better than a wrong sibling.
            CoreGroup solo{logical};
            groups.push_back(std::move(solo));
            continue;
        }

        bool placed = false;
        for (CoreGroup& group : groups) {
            long other_package = -1;
            long other_core = -1;
            const std::size_t head = group.front();
            if (read_topology_file(head, "physical_package_id", &other_package) &&
                read_topology_file(head, "core_id", &other_core) && other_package == package &&
                other_core == core) {
                group.push_back(logical);
                placed = true;
                break;
            }
        }
        if (!placed) {
            CoreGroup fresh{logical};
            groups.push_back(std::move(fresh));
        }
    }

    std::sort(groups.begin(), groups.end(),
              [](const CoreGroup& a, const CoreGroup& b) { return a.front() < b.front(); });
    return groups;
}

[[nodiscard]] std::size_t count_logical() noexcept {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<std::size_t>(n) : 1u;
}

bool pin(std::size_t logical) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (logical >= static_cast<std::size_t>(CPU_SETSIZE)) {
        return false;
    }
    CPU_SET(static_cast<int>(logical), &set);
    return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

[[nodiscard]] std::size_t current_cpu() noexcept {
    // `sched_getcpu` would be tidier but needs _GNU_SOURCE, which is not
    // worth a feature-test macro in a header used by both platforms.
    // The lowest bit of the affinity mask is the same answer for a
    // pinned thread, which is the only case that matters here.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) != 0) {
        return 0;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set)) {
            return static_cast<std::size_t>(cpu);
        }
    }
    return 0;
}

#endif

[[nodiscard]] const std::vector<CoreGroup>& topology() {
    static const std::vector<CoreGroup> groups = detect_topology();
    return groups;
}

/// Index into `topology()` for a logical processor, or its size when the
/// processor is not in any group (which means topology detection failed).
[[nodiscard]] std::size_t group_of(std::size_t logical) noexcept {
    const auto& groups = topology();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        for (const std::size_t member : groups[i]) {
            if (member == logical) {
                return i;
            }
        }
    }
    return groups.size();
}

/// The logical processor in `group` that is not `logical` itself.
///
/// Not `group[1]`: a thread placed on the upper half of a two-way SMT
/// core would otherwise be reported as sharing its core with itself.
[[nodiscard]] std::size_t other_in(const CoreGroup& group, std::size_t logical) noexcept {
    for (const std::size_t member : group) {
        if (member != logical) {
            return member;
        }
    }
    return logical;
}

}  // namespace

std::size_t logical_processor_count() noexcept { return count_logical(); }

std::size_t physical_core_count() noexcept {
    const auto& groups = topology();
    // An empty topology means detection failed. Reporting the logical
    // count is then the honest answer: this process can use that many
    // processors and has no evidence they are separate cores.
    return groups.empty() ? count_logical() : groups.size();
}

std::size_t smt_sibling(std::size_t logical) noexcept {
    const auto& groups = topology();
    const std::size_t index = group_of(logical);
    if (index >= groups.size()) {
        return logical;
    }
    const CoreGroup& group = groups[index];
    // A group of one has no sibling, and returning `logical` makes that
    // case indistinguishable from "the sibling is me", which is exactly
    // right: a thread cannot share a core with itself.
    return group.size() < 2 ? logical : group[1];
}

bool shares_physical_core(std::size_t a, std::size_t b) noexcept {
    if (a == b) {
        return false;
    }
    const std::size_t index = group_of(a);
    return index < topology().size() && index == group_of(b);
}

bool pin_current_thread(std::size_t logical) noexcept { return pin(logical); }

std::size_t current_logical_processor() noexcept { return current_cpu(); }

std::string describe_topology() {
    const auto& groups = topology();
    if (groups.empty()) {
        return "topology unknown (" + std::to_string(count_logical()) +
               " logical processors, physical cores not detected)";
    }

    std::size_t with_smt = 0;
    for (const CoreGroup& group : groups) {
        if (group.size() > 1) {
            ++with_smt;
        }
    }

    std::string out = std::to_string(groups.size()) + " physical cores / " +
                      std::to_string(count_logical()) + " logical processors";
    if (with_smt == 0) {
        out += ", SMT not detected";
    } else {
        out += ", " + std::to_string(with_smt) + " core(s) with SMT";
    }
    return out;
}

std::string describe_affinity(const char* role) {
    const std::size_t logical = current_logical_processor();
    const std::size_t total = count_logical();

    std::string out = role;
    out += ": ";
    if (logical >= total) {
        out += "placement not detected";
        return out;
    }

    out += "logical " + std::to_string(logical);
    const std::size_t index = group_of(logical);
    const auto& groups = topology();
    if (index < groups.size()) {
        out += " (physical core " + std::to_string(index) + " of " + std::to_string(groups.size());
        if (groups[index].size() > 1) {
            out += ", shares with logical " + std::to_string(groups[index][1]);
        } else {
            out += ", no SMT sibling";
        }
        out += ")";
    } else {
        out += " (physical core unknown)";
    }
    return out;
}

}  // namespace hft::util