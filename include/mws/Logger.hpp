#pragma once
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <atomic>
#include <cstdint>

namespace mws {
class Logger {
public:
  static Logger& instance();

  bool open(const std::filesystem::path& path);
  void info(std::string_view s);
  void warn(std::string_view s);
  void error(std::string_view s);
  void debug(std::string_view s);
  void flush();
  void flushIfDue();

  // Debug mode and warnings/errors retain immediate crash diagnostics.
  void setDebugEnabled(bool enabled) noexcept { debugEnabled_ = enabled; }
  bool debugEnabled() const noexcept { return debugEnabled_; }

private:
  void write(const char* level, std::string_view s);

  std::ofstream out_;
  std::mutex mutex_;
  std::atomic<bool> debugEnabled_{false};
  std::atomic<std::uint64_t> nextFlushTick_{0};
  bool dirty_{false};
};
}
