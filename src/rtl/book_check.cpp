// Compare two book dumps.
//
// The counterpart to `hft_rtl_check`, for the order book rather than
// the decode path.
//
// It reports the first order that differs, and which field of it. A
// checksum would report only that the two differ; the whole point of
// the dump is that it says where.
//
// Usage:
//   hft_book_check <dump_a> <dump_b> [--max-report N]

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Line {
    std::string text;
    std::string comment;
    bool is_comment = false;
};

[[nodiscard]] bool load(const std::string& path, std::vector<Line>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        if (raw.empty()) {
            continue;
        }
        Line line;
        if (raw[0] == '#') {
            line.is_comment = true;
            line.comment = raw;
        } else {
            line.text = raw;
        }
        out.push_back(line);
    }
    return true;
}

/// The field names, by position, for a failure message.
[[nodiscard]] const char* field_name(std::size_t position) {
    static const char* kNames[] = {"index", "side", "id", "price", "size", "state"};
    if (position < sizeof(kNames) / sizeof(kNames[0])) {
        return kNames[position];
    }
    return "field";
}

[[nodiscard]] std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> tokens;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
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
        std::printf("usage: hft_book_check <dump_a> <dump_b> [--max-report N]\n");
        return 2;
    }

    std::vector<Line> a;
    std::vector<Line> b;
    std::string error;
    if (!load(path_a, a, error)) {
        std::printf("%s\n", error.c_str());
        return 2;
    }
    if (!load(path_b, b, error)) {
        std::printf("%s\n", error.c_str());
        return 2;
    }

    std::printf("book comparison\n");
    std::printf("----------------\n");
    std::printf("a                 %s (%zu lines)\n", path_a.c_str(), a.size());
    std::printf("b                 %s (%zu lines)\n", path_b.c_str(), b.size());

    std::size_t differences = 0;
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();

    for (std::size_t i = 0; i < common; ++i) {
        if (a[i].is_comment != b[i].is_comment) {
            ++differences;
            if (differences <= max_report) {
                std::printf("  line %-6zu a comment is not a comment on the other side\n", i);
            }
            continue;
        }
        if (a[i].is_comment) {
            // The checksum line, and the header.
            if (a[i].comment != b[i].comment) {
                ++differences;
                if (differences <= max_report) {
                    std::printf("  line %-6zu %s\n           a: %s\n           b: %s\n", i,
                                "comment differs", a[i].comment.c_str(), b[i].comment.c_str());
                }
            }
            continue;
        }
        if (a[i].text == b[i].text) {
            continue;
        }
        ++differences;
        if (differences > max_report) {
            continue;
        }
        const std::vector<std::string> ta = split(a[i].text);
        const std::vector<std::string> tb = split(b[i].text);
        const std::size_t fields = ta.size() < tb.size() ? ta.size() : tb.size();
        std::string what = "the order differs";
        for (std::size_t f = 0; f < fields; ++f) {
            if (ta[f] != tb[f]) {
                what = std::string("field ") + field_name(f) + " differs";
                break;
            }
        }
        std::printf("  line %-6zu %s\n           a: %s\n           b: %s\n", i, what.c_str(),
                    a[i].text.c_str(), b[i].text.c_str());
    }

    if (a.size() != b.size()) {
        ++differences;
        std::printf("  line count differs: a has %zu, b has %zu\n", a.size(), b.size());
    }

    std::printf("differences       %zu\n", differences);
    if (differences == 0) {
        std::printf("\nIDENTICAL: the two books agree on every order and every field.\n");
        return 0;
    }
    std::printf("\nDIFFERENT: the two books do not agree.\n");
    return 1;
}
