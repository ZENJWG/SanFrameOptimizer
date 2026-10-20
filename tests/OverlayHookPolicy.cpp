#include "mws/OverlayHookPolicy.hpp"
#include <cassert>
int main() {
  // DistantStreaming always wins over the extra EndScene hook.
  assert(!mws::allowEndSceneOverlayHook(true, false));
  assert(!mws::allowEndSceneOverlayHook(true, true));
  // Manual override works for renamed or late-loading ASIs.
  assert(!mws::allowEndSceneOverlayHook(false, true));
  // Unrelated setups retain the old behavior by default.
  assert(mws::allowEndSceneOverlayHook(false, false));
}
