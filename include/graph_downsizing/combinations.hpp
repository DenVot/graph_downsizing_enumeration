#pragma once

#include <boost/multiprecision/cpp_int.hpp>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace downsizing {
using BigInt = boost::multiprecision::cpp_int;

[[nodiscard]] BigInt combination_count(std::size_t n, std::size_t r);
[[nodiscard]] std::vector<std::size_t> unrank_combination(std::size_t n, std::size_t r, BigInt rank);
// Indices must describe a valid, increasing combination; false leaves the last combination unchanged.
bool next_combination(std::vector<std::size_t>& indices, std::size_t n);

struct CombinationRange {
    std::size_t node;
    BigInt begin;
    BigInt end; // exclusive
};

// Lazy, round-robin allocation among nodes. Caller synchronizes next(). No range queue is stored.
class CombinationRanges {
public:
    CombinationRanges(const std::vector<std::size_t>& node_sizes, std::size_t r, std::size_t chunk_size);
    [[nodiscard]] std::optional<CombinationRange> next();
private:
    struct Cursor { BigInt next = 0; BigInt end; };
    std::vector<Cursor> cursors_;
    std::deque<std::size_t> active_;
    std::size_t chunk_size_;
};
} // namespace downsizing
