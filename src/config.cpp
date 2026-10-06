#include "graph_downsizing/config.hpp"

#include "graph_downsizing/alpha.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace downsizing {
namespace {

using Json = nlohmann::json;

void check_keys(const Json& object, std::initializer_list<std::string_view> allowed, std::string_view name) {
    if (!object.is_object()) {
        throw std::invalid_argument(std::string(name) + " must be a JSON object");
    }
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            // Do not repeat user-controlled keys or values: they could contain credentials.
            throw std::invalid_argument("Unknown field in " + std::string(name));
        }
    }
}

std::string string_field(const Json& object, std::string_view key, std::string fallback) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return fallback;
    }
    if (!it->is_string() || it->get_ref<const std::string&>().empty()) {
        throw std::invalid_argument(std::string(key) + " must be a nonempty string");
    }
    return it->get<std::string>();
}

bool bool_field(const Json& object, std::string_view key, bool fallback) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return fallback;
    }
    if (!it->is_boolean()) {
        throw std::invalid_argument(std::string(key) + " must be a boolean");
    }
    return it->get<bool>();
}

std::uint64_t positive_field(const Json& object, std::string_view key, std::uint64_t fallback,
                             std::uint64_t maximum) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return fallback;
    }
    if (!it->is_number_unsigned() || it->get<std::uint64_t>() == 0 || it->get<std::uint64_t>() > maximum) {
        throw std::invalid_argument(std::string(key) + " must be a positive integer within the supported range");
    }
    return it->get<std::uint64_t>();
}

std::filesystem::path resolve_path(const std::filesystem::path& base, const std::string& value) {
    if (value.find('\0') != std::string::npos) {
        throw std::invalid_argument("Paths must not contain NUL characters");
    }
    return (base / std::filesystem::path(value)).lexically_normal();
}

bool valid_repository_url(const std::string& url) {
    constexpr std::string_view prefix = "https://github.com/";
    if (!url.starts_with(prefix)) {
        return false;
    }
    const auto rest = std::string_view(url).substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash == rest.size() - 1) {
        return false;
    }
    return std::all_of(rest.begin(), rest.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_' ||
               character == '.' || character == '/';
    }) && rest.find('/', slash + 1) == std::string_view::npos && rest.find("..") == std::string_view::npos;
}

bool valid_branch(const std::string& branch) {
    if (branch == "@" || branch.front() == '/' || branch.back() == '/' || branch.back() == '.' ||
        branch.front() == '-' || branch.find("..") != std::string::npos ||
        branch.find("@{") != std::string::npos || branch.find("//") != std::string::npos) {
        return false;
    }
    for (const char byte : branch) {
        const auto character = static_cast<unsigned char>(byte);
        if (character <= 32 || character == 127 || std::string_view("~^:?*[\\").find(static_cast<char>(character)) != std::string_view::npos) {
            return false;
        }
    }
    std::size_t start = 0;
    while (start < branch.size()) {
        auto end = branch.find('/', start);
        if (end == std::string::npos) {
            end = branch.size();
        }
        const auto component = std::string_view(branch).substr(start, end - start);
        if (component.front() == '.' || component.ends_with(".lock")) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace

Config read_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open configuration file: " + path.string());
    }
    // Parsing exceptions can include source excerpts containing a token. Suppress them.
    bool duplicate_key = false;
    std::vector<std::set<std::string>> object_keys;
    const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            object_keys.pop_back();
        } else if (event == Json::parse_event_t::key) {
            if (!object_keys.back().insert(parsed.get<std::string>()).second) {
                duplicate_key = true;
            }
        }
        return true;
    };
    const auto document = Json::parse(input, callback, false);
    if (document.is_discarded()) {
        throw std::invalid_argument("Configuration is not valid JSON");
    }
    if (duplicate_key) {
        throw std::invalid_argument("Duplicate field in configuration");
    }
    if (input.bad()) {
        throw std::runtime_error("Failed to read configuration file");
    }
    check_keys(document, {"inputs", "alpha_step", "threads", "combination_chunk_size", "working_directory",
                          "checkpoint_interval_seconds", "github"}, "configuration");

    Config config{};
    const auto base = std::filesystem::absolute(path).parent_path().lexically_normal();
    if (document.contains("alpha_step")) {
        if (!document["alpha_step"].is_number()) {
            throw std::invalid_argument("alpha_step must be a number in (0, 1]");
        }
        config.alpha_step = document["alpha_step"].get<double>();
    }
    (void)AlphaSchedule(config.alpha_step, 1);
    config.threads = static_cast<std::size_t>(positive_field(document, "threads",
        std::max(1U, std::thread::hardware_concurrency()), std::numeric_limits<std::size_t>::max()));
    config.combination_chunk_size = static_cast<std::size_t>(positive_field(document, "combination_chunk_size",
        256, std::numeric_limits<std::size_t>::max()));
    config.working_directory = resolve_path(base, string_field(document, "working_directory", "runs"));
    constexpr auto max_seconds = static_cast<std::uint64_t>(std::chrono::seconds::max().count());
    config.checkpoint_interval = std::chrono::seconds(static_cast<std::chrono::seconds::rep>(
        positive_field(document, "checkpoint_interval_seconds", 60, max_seconds)));

    const auto files = document.find("inputs");
    if (files == document.end() || !files->is_array() || files->empty()) {
        throw std::invalid_argument("inputs must be a nonempty array of file paths");
    }
    std::set<std::filesystem::path> unique_inputs;
    for (const auto& file : *files) {
        if (!file.is_string() || file.get_ref<const std::string&>().empty()) {
            throw std::invalid_argument("Every input path must be a nonempty string");
        }
        auto resolved = resolve_path(base, file.get<std::string>());
        if (!unique_inputs.insert(resolved).second) {
            throw std::invalid_argument("inputs must not contain duplicate paths");
        }
        config.inputs.push_back(std::move(resolved));
    }

    const auto github = document.find("github");
    if (github != document.end()) {
        check_keys(*github, {"enabled", "repository_url", "branch", "username", "token", "token_env",
                             "upload_interval_seconds"}, "github configuration");
        config.github.enabled = bool_field(*github, "enabled", false);
        config.github.repository_url = string_field(*github, "repository_url", "");
        config.github.branch = string_field(*github, "branch", "runs");
        config.github.username = string_field(*github, "username", "x-access-token");
        if (github->contains("token") && github->contains("token_env")) {
            throw std::invalid_argument("Specify only one of github.token and github.token_env");
        }
        config.github.token = string_field(*github, "token", "");
        config.github.token_env = string_field(*github, "token_env", config.github.token.empty()
            ? "GRAPH_DOWNSIZING_GITHUB_TOKEN" : "");
        config.github.upload_interval = std::chrono::seconds(static_cast<std::chrono::seconds::rep>(
            positive_field(*github, "upload_interval_seconds", 300, max_seconds)));
        if ((config.github.enabled || !config.github.repository_url.empty()) && !valid_repository_url(config.github.repository_url)) {
            throw std::invalid_argument("github.repository_url must be https://github.com/OWNER/REPOSITORY without credentials");
        }
        if (!valid_branch(config.github.branch)) {
            throw std::invalid_argument("github.branch is not a valid Git branch name");
        }
    }
    return config;
}

std::filesystem::path solution_path(const Config& config, std::size_t input_index) {
    const auto& input = config.inputs.at(input_index);
    return config.working_directory / ("graph_" + std::to_string(input_index)) / (input.stem().string() + "_sol.txt");
}

} // namespace downsizing
