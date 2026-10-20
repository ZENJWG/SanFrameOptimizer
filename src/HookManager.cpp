#include "mws/HookManager.hpp"
#include "mws/Logger.hpp"
#include <MinHook.h>
namespace mws {
bool HookManager::initialize() {
  const auto st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
    Logger::instance().error("MinHook initialization failed.");
    return false;
  }
  Logger::instance().info("MinHook initialized.");
  return true;
}
void HookManager::shutdown() { MH_Uninitialize(); }
}
