#pragma once

#include <boost/multiprecision/cpp_int.hpp>
#include <cstddef>
#include <vector>

namespace downsizing {
using BigInt = boost::multiprecision::cpp_int;

[[nodiscard]] BigInt combination_count(std::size_t n, std::size_t r);
// Rank and unrank use the same lexicographic order as next_combination.
[[nodiscard]] BigInt combination_rank(const std::vector<std::size_t>& indices, std::size_t n);
[[nodiscard]] std::vector<std::size_t> combination_at(std::size_t n, std::size_t r, BigInt rank);
// Indices must describe a valid, increasing combination; false leaves the last combination unchanged.
bool next_combination(std::vector<std::size_t>& indices, std::size_t n);

} // namespace downsizing
