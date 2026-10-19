// Compare two decode traces.
//
// The tool is deliberately ignorant of what the two traces ARE. It
// compares them positionally and reports the first difference, and
// nothing else. It does not know which side is the C++ engine and which
// side is the RTL model, because a comparator that knew would be a
// comparator that could be asked to be lenient about one of them.
//
// Usage:
//   hft_rtl_check <trace_a> <trace_b> [--max-report N]
//
// Exits 0 when the traces are identical, 1 when they differ, 2 on an
// I/O or parse error.
//
// ---- Why the report is positional ------------------------------------
//
// Field 3 against field 3, and so on. The field names the report prints
// come from the C++ field table and are a convenience for a human; they
// are not part of the comparison and are not read from the trace.
//
// The consequence is that an implementation which extracts a DIFFERENT
// set of fields is caught, not accommodated: the field counts differ and
// the comparison fails. An implementation that extracts the same fields
// but reads them from the wrong offsets produces different values and
// also fails. What is not caught is an implementation that agrees with
// the C++ engine on every extracted value while having a different
// opinion about the bytes it skipped -- which is not a thing a decoder
// can do, because skipping the wrong bytes changes the values.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "rtl/trace.hpp"

namespace {

[[nodiscard]] bool load_trace(const std::string& path,
                              std::vector<hft::rtl::TraceRecord>& records,
                              std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    hft::rtl::TraceRecord record;
    std::string line_error;
    while (hft::rtl::read_record(in, record, line_error)) {
        std::string invalid;
        if (!hft::rtl::validate_record(record, invalid)) {
            error = path + ": record " + std::to_string(record.index) + " is invalid: " + invalid;
            return false;
        }
        records.push_back(record);
    }
    if (!line_error.empty()) {
        error = path + ": " + line_error;
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path_a;
    std::string path_b;
    std::size_t max_report = 20;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-report") {
            if (i + 1 >= argc) {
                std::printf("--max-report needs a value\n");
                return 2;
            }
            max_report = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (!arg.empty() && arg[0] == '-') {
            std::printf("unknown option %s\n", arg.c_str());
            return 2;
        } else if (path_a.empty()) {
            path_a = arg;
        } else if (path_b.empty()) {
            path_b = arg;
        } else {
            std::printf("unexpected argument %s\n", arg.c_str());
            return 2;
        }
    }

    if (path_a.empty() || path_b.empty()) {
        std::printf("usage: hft_rtl_check <trace_a> <trace_b> [--max-report N]\n");
        return 2;
    }

    std::vector<hft::rtl::TraceRecord> a;
    std::vector<hft::rtl::TraceRecord> b;
    std::string error;
    if (!load_trace(path_a, a, error)) {
        std::printf("%s\n", error.c_str());
        return 2;
    }
    if (!load_trace(path_b, b, error)) {
        std::printf("%s\n", error.c_str());
        return 2;
    }

    std::vector<hft::rtl::TraceDifference> differences;
    const bool identical = hft::rtl::compare_traces(a, b, differences, max_report);

    std::printf("trace comparison\n");
    std::printf("----------------\n");
    std::printf("a                 %s (%zu records)\n", path_a.c_str(), a.size());
    std::printf("b                 %s (%zu records)\n", path_b.c_str(), b.size());
    std::printf("differences       %zu\n", differences.size());

    if (identical) {
        std::printf("\nIDENTICAL: the two traces agree on every record and every field.\n");
        return 0;
    }

    std::printf("\nDIFFERENT:\n");
    for (const hft::rtl::TraceDifference& d : differences) {
        std::printf("  record %-8zu %-18s a=%-20s b=%s\n", d.index, d.what.c_str(),
                    d.expected.c_str(), d.actual.c_str());
    }
    if (differences.size() >= max_report) {
        std::printf("  ... further differences not shown (max-report %zu)\n", max_report);
    }
    return 1;
}
