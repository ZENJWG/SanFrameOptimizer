#pragma once
#include <Windows.h>

namespace mws {
void setPluginModule(HMODULE module) noexcept;

class Plugin {
public:
  static Plugin& instance();
  bool initialize();
  void shutdown();
private:
  bool initialized_{false};
};
}
