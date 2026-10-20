#pragma once
#include "mws/Config.hpp"
namespace mws::EnginePass {
void install(const Config& config);
void uninstall();
// Called at existing frame boundaries; reports activity separately from FPS.
void onPresent();
void captureDiagnostics();
#ifdef SANFRAME_ENGINE_PASS_TEST
bool installReference(bool alpha,bool shadow);
void beginShadowReference();
void endShadowReference();
#endif
}
