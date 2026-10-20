#pragma once
#include "mws/Config.hpp"

namespace mws {

class D3D9Hook {
public:
  bool install(const Config& config);
  void uninstall();
  static void captureDiagnostics();

private:
  bool installed_{false};
};

} // namespace mws
