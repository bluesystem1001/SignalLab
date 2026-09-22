#pragma once

#include <cstddef>

namespace signallab::parser {

// Source offsets count UTF-8 bytes. Lines and columns are one-based; columns
// count Unicode code points so diagnostics match what a user sees.
struct SourceLocation {
    std::size_t offset{0};
    std::size_t line{1};
    std::size_t column{1};

    [[nodiscard]] bool operator==(const SourceLocation&) const = default;
};

// Source spans are half-open: begin is included and end is excluded.
struct SourceSpan {
    SourceLocation begin{};
    SourceLocation end{};

    [[nodiscard]] bool operator==(const SourceSpan&) const = default;
};

}  // namespace signallab::parser
