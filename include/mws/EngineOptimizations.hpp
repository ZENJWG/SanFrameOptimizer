#pragma once
#include "mws/Config.hpp"
#include <cstdint>

namespace mws::EngineOptimizations {
void install(const Config& config);
void uninstall();
void onPresent();
bool registerEntity(void* entity, std::uint32_t flags);
void unregisterEntity(void* entity);
void invalidateEntity(void* entity);
void forceVisible(void* entity, std::uint32_t frames);
}
