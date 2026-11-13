#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace wireview::host {

// Capture existing Host diagnostics for one connection. Completed lines are
// flushed immediately, including when the controller later terminates Host.
class SessionPerformanceLog {
public:
    explicit SessionPerformanceLog(
        const std::filesystem::path& directory,
        std::uint32_t process_id,
        std::ostream& output = std::cout,
        std::ostream& errors = std::cerr)
        : output_(output), errors_(errors),
          output_buffer_(*this, output.rdbuf(), "host"),
          error_buffer_(*this, errors.rdbuf(), "error") {
        std::filesystem::create_directories(directory);
        static std::atomic_uint64_t next_connection{0U};
        path_ = directory / ("performance-" + timestamp(true) + "-" +
            std::to_string(process_id) + "-" +
            std::to_string(next_connection.fetch_add(1U)) + ".log");
        file_.open(path_, std::ios::out | std::ios::app);
        if (!file_) {
            throw std::runtime_error("Cannot open WireView performance log");
        }
        output_.rdbuf(&output_buffer_);
        errors_.rdbuf(&error_buffer_);
    }

    ~SessionPerformanceLog() {
        output_.rdbuf(output_buffer_.original());
        errors_.rdbuf(error_buffer_.original());
        for (const auto& [key, text] : pending_) {
            if (!text.empty()) {
                write_line(key.second, text);
            }
        }
        write_line("session", std::uncaught_exceptions() > 0
            ? "Connection diagnostics ended during exception unwinding"
            : "Connection diagnostics ended");
    }

    SessionPerformanceLog(const SessionPerformanceLog&) = delete;
    SessionPerformanceLog& operator=(const SessionPerformanceLog&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    class Buffer final : public std::streambuf {
    public:
        Buffer(SessionPerformanceLog& owner, std::streambuf* original, const char* channel)
            : owner_(owner), original_(original), channel_(channel) {}
        [[nodiscard]] std::streambuf* original() const noexcept { return original_; }

    protected:
        std::streamsize xsputn(const char* text, std::streamsize count) override {
            std::lock_guard lock(owner_.mutex_);
            const auto written = original_->sputn(text, count);
            auto& pending = owner_.pending_[{std::this_thread::get_id(), channel_}];
            for (std::streamsize index = 0; index < count; ++index) {
                if (text[index] == '\n') {
                    owner_.write_line(channel_, pending);
                    pending.clear();
                } else if (text[index] != '\r') {
                    pending += text[index];
                }
            }
            return written;
        }

        int_type overflow(int_type value) override {
            if (traits_type::eq_int_type(value, traits_type::eof())) {
                return traits_type::not_eof(value);
            }
            const char character = traits_type::to_char_type(value);
            return xsputn(&character, 1) == 1 ? value : traits_type::eof();
        }

        int sync() override {
            std::lock_guard lock(owner_.mutex_);
            return original_->pubsync();
        }

    private:
        SessionPerformanceLog& owner_;
        std::streambuf* original_;
        const char* channel_;
    };

    static std::string timestamp(bool filename) {
        const auto now = std::chrono::system_clock::now();
        const auto seconds = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_s(&local, &seconds);
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        std::ostringstream text;
        text << std::put_time(&local, filename ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S")
             << (filename ? '-' : '.') << std::setfill('0') << std::setw(3) << millis;
        return text.str();
    }

    void write_line(const char* channel, const std::string& text) {
        file_ << timestamp(false) << " | " << channel << " | " << text << '\n';
        file_.flush();
        if (!file_ && !reported_write_error_) {
            reported_write_error_ = true;
            constexpr char message[] = "WireView performance log write failed\n";
            error_buffer_.original()->sputn(message, sizeof(message) - 1U);
            error_buffer_.original()->pubsync();
        }
    }

    std::ostream& output_;
    std::ostream& errors_;
    Buffer output_buffer_;
    Buffer error_buffer_;
    std::filesystem::path path_;
    std::ofstream file_;
    std::mutex mutex_;
    std::map<std::pair<std::thread::id, const char*>, std::string> pending_;
    bool reported_write_error_{false};
};

} // namespace wireview::host
