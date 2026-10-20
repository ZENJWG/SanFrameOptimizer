#pragma once
#include "../runtime/Engine.h"
namespace fxp::render {
struct TriangleArgs { float p[9], uv[6]; uint32_t rgba[12]; };
bool Install(engine::Logger logger);
void Uninstall();
void ClearProject();
bool IsInstalled();
void BeforeReset();
void BeginScope(bool profile = false, bool explosion = false);
void EndScope();
engine::Counters TakeCounters();
#ifdef FXP_RENDER_TEST
bool TestDraw(void* device, const TriangleArgs* triangles, unsigned count, const float* worldMatrix);
void TestRelease();
#endif
}
