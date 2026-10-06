#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace downsizing {

struct GitHubConfig {
    bool enabled = false;
    std::string repository_url;
    std::string branch = "runs";
    std::string username = "x-access-token";
    std::string token;
    std::string token_env = "GRAPH_DOWNSIZING_GITHUB_TOKEN";
    std::chrono::seconds upload_interval{300};
};

struct Config {
    std::vector<std::filesystem::path> inputs;
    double alpha_step = 0.1;
    std::size_t threads = 1;
    std::size_t combination_chunk_size = 256;
    std::filesystem::path working_directory;
    std::chrono::seconds checkpoint_interval{60};
    GitHubConfig github;
};

// Relative paths are resolved against the config file's directory, not the process cwd.
[[nodiscard]] Config read_config(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path solution_path(const Config& config, std::size_t input_index);

} // namespace downsizing
