#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace mws {
struct ExecutableInfo {
  HMODULE module{};
  void* base{};
  std::size_t imageSize{};
  std::uint32_t timeDateStamp{};
  bool validPe{};

  static ExecutableInfo current();
  void log() const;
};
}
