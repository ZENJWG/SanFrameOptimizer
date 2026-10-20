#pragma once
#include "Engine.h"
namespace fxp::operators {
bool Install(bool cache, bool plans, engine::Logger logger);
void Uninstall();
void BeginFrame();
void ClearProject();
struct Stats { uint64_t cacheCalls{}, cacheHits{}, planCalls{}, planHits{}, fallbacks{}; };
Stats TakeStats();
}
