#include "graph_downsizing/logging.hpp"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace downsizing {
namespace {

constexpr double mebibyte = 1024.0 * 1024.0;

struct Resources {
    std::optional<double> cpu_seconds;
    std::optional<double> rss_mib;
    std::optional<double> peak_rss_mib;
};

Resources read_resources() {
    Resources result;
    struct rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
        result.cpu_seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                             static_cast<double>(usage.ru_stime.tv_sec) +
                             (static_cast<double>(usage.ru_utime.tv_usec) +
                              static_cast<double>(usage.ru_stime.tv_usec)) / 1'000'000.0;
#ifdef __APPLE__
        result.peak_rss_mib = static_cast<double>(usage.ru_maxrss) / mebibyte;
#else
        result.peak_rss_mib = static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
    }
#ifdef __linux__
    std::ifstream statm("/proc/self/statm");
    unsigned long long virtual_pages = 0;
    unsigned long long resident_pages = 0;
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size > 0 && statm >> virtual_pages >> resident_pages) {
        result.rss_mib = static_cast<double>(resident_pages) * static_cast<double>(page_size) / mebibyte;
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        result.rss_mib = static_cast<double>(info.resident_size) / mebibyte;
    }
#endif
    return result;
}

std::string metric(const std::optional<double>& value) {
    if (!value) {
        return "n/a";
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << *value;
    return text.str();
}

} // namespace

RunLogger::RunLogger(const std::filesystem::path& file, WorkerPool& pool, std::chrono::seconds interval)
    : pool_(pool), directory_(file.parent_path()), output_(file, std::ios::app) {
    if (!output_) {
        throw std::runtime_error("Cannot open log file: " + file.string());
    }
    if (interval.count() <= 0) {
        throw std::invalid_argument("Log interval must be positive");
    }
    sample();
    monitor_ = std::thread([this, interval] { monitor(interval); });
}

RunLogger::~RunLogger() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (monitor_.joinable()) {
        monitor_.join();
    }
    try {
        sample();
    } catch (const std::exception& error) {
        std::cerr << "Logging error: " << error.what() << '\n';
    }
}

void RunLogger::search(const std::string& graph, double alpha, std::size_t r) {
    std::ostringstream text;
    text << "graph=" << std::quoted(graph) << " phase=search alpha=" << std::setprecision(15) << alpha << " r=" << r;
    std::lock_guard lock(mutex_);
    context_ = text.str();
    progress_ = {};
    searching_ = true;
    write("INFO", "event=search_started");
}

void RunLogger::search_progress(const SolveProgress& progress) {
    std::lock_guard lock(mutex_);
    progress_ = progress;
}

std::string RunLogger::progress_fields() const {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << context_;
    if (!searching_) {
        text << " level_nodes_percent=n/a overall_percent=n/a";
        return text.str();
    }
    text << " k=" << progress_.k << " level_nodes_done=" << progress_.level_done
         << " level_nodes_total=" << progress_.level_total << " level_nodes_percent=";
    if (progress_.level_total != 0) {
        text << 100.0L * static_cast<long double>(progress_.level_done) / static_cast<long double>(progress_.level_total);
    } else {
        text << "n/a";
    }
    text << " overall_nodes_done=" << progress_.overall_done << " overall_nodes_total=" << progress_.overall_total
         << " overall_percent=";
    if (progress_.solved) {
        text << "100.00";
    } else if (progress_.overall_total != 0) {
        text << 100.0L * static_cast<long double>(progress_.overall_done) / static_cast<long double>(progress_.overall_total);
    } else {
        text << "n/a";
    }
    text << " search_status=" << (progress_.solved ? "SOLVED" : "IN_PROGRESS");
    return text.str();
}

void RunLogger::write(const char* level, const std::string& message) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    std::ostringstream line;
    line << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ") << " " << level << " pid=" << ::getpid()
         << " elapsed_s=" << std::fixed << std::setprecision(2) << elapsed << " " << message << " " << progress_fields();
    // One physical line per event, including filenames containing newlines.
    auto rendered = line.str();
    for (auto& character : rendered) {
        if (character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    output_ << rendered << '\n';
    output_.flush();
    std::clog << rendered << std::endl;
    if (!output_) {
        throw std::runtime_error("Cannot write run.log");
    }
}

void RunLogger::info(const std::string& message) {
    std::lock_guard lock(mutex_);
    write("INFO", message);
}

void RunLogger::error(const std::string& message) {
    std::lock_guard lock(mutex_);
    write("ERROR", message);
}

void RunLogger::context(const std::string& graph, const std::string& phase, std::size_t r) {
    std::ostringstream text;
    text << "graph=" << std::quoted(graph) << " phase=" << phase;
    if (r != 0) {
        text << " r=" << r;
    }
    std::lock_guard lock(mutex_);
    context_ = text.str();
    searching_ = false;
    write("INFO", "event=phase_changed");
}

void RunLogger::sample() {
    const auto resources = read_resources();
    const auto now = std::chrono::steady_clock::now();
    std::optional<double> cpu_percent;
    if (resources.cpu_seconds && previous_cpu_ >= 0) {
        const double wall = std::chrono::duration<double>(now - previous_time_).count();
        if (wall > 0) {
            cpu_percent = 100.0 * (*resources.cpu_seconds - previous_cpu_) / wall;
        }
    }
    previous_cpu_ = resources.cpu_seconds.value_or(-1);
    previous_time_ = now;
    const auto pool = pool_.stats();
    std::error_code space_error;
    const auto space = std::filesystem::space(directory_, space_error);
    const std::optional<double> free_gib = space_error ? std::nullopt :
        std::optional<double>(static_cast<double>(space.available) / (1024.0 * mebibyte));
    std::ostringstream text;
    text << "resources cpu_percent=" << metric(cpu_percent)
         << " rss_mib=" << metric(resources.rss_mib)
         << " peak_rss_mib=" << metric(resources.peak_rss_mib)
         << " disk_available_gib=" << metric(free_gib)
         << " workers=" << pool_.size() << " active_tasks=" << pool.active
         << " queued_tasks=" << pool.queued << " finished_tasks=" << pool.finished;
    std::lock_guard lock(mutex_);
    write("INFO", text.str());
}

void RunLogger::monitor(std::chrono::seconds interval) {
    std::unique_lock lock(mutex_);
    while (!wake_.wait_for(lock, interval, [this] { return stopping_; })) {
        lock.unlock();
        try {
            sample();
        } catch (const std::exception& error) {
            std::cerr << "Resource logging stopped: " << error.what() << '\n';
            return;
        }
        lock.lock();
    }
}

} // namespace downsizing
