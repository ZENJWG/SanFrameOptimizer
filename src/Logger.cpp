#include "mws/Logger.hpp"
#include <chrono>
#include <iomanip>
#include <Windows.h>

namespace mws {
Logger& Logger::instance() {
  static Logger l;
  return l;
}

bool Logger::open(const std::filesystem::path& p) {
  std::scoped_lock lock(mutex_);
  out_.open(p, std::ios::trunc);
  nextFlushTick_.store(GetTickCount64() + 1000, std::memory_order_relaxed);
  return out_.is_open();
}

void Logger::flush() {
  std::scoped_lock lock(mutex_);
  if (dirty_) {
    out_.flush();
    dirty_ = false;
  }
  nextFlushTick_.store(GetTickCount64() + 1000, std::memory_order_relaxed);
}

void Logger::flushIfDue() {
  if (GetTickCount64() >= nextFlushTick_.load(std::memory_order_relaxed))
    flush();
}

void Logger::info(std::string_view s)  { write("INFO", s); }
void Logger::warn(std::string_view s)  { write("WARN", s); }
void Logger::error(std::string_view s) { write("ERROR", s); }
void Logger::debug(std::string_view s) {
  if (debugEnabled_) write("DEBUG", s);
}

void Logger::write(const char* level, std::string_view s) {
  std::scoped_lock lock(mutex_);
  if (!out_) return;

  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count() % 1000;

  std::tm tm{};
  localtime_s(&tm, &t);

  out_ << std::put_time(&tm, "%F %T")
       << '.' << std::setw(3) << std::setfill('0') << ms
       << " [" << level << "] " << s << '\n';
  dirty_ = true;
  if (level[0] == 'W' || level[0] == 'E' || debugEnabled_ ||
      GetTickCount64() >= nextFlushTick_.load(std::memory_order_relaxed)) {
    out_.flush();
    dirty_ = false;
    nextFlushTick_.store(GetTickCount64() + 1000, std::memory_order_relaxed);
  }
}
}
