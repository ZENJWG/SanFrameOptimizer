cmake_minimum_required(VERSION 4.2)
project(SanFrameOptimizer VERSION 1.4.3 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Windows is required.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "GTA SA Classic is 32-bit. Configure with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(CTest)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/deps.lock" SANFRAME_DEPENDENCIES)
foreach(ENTRY IN LISTS SANFRAME_DEPENDENCIES)
  if(ENTRY MATCHES "^([A-Z0-9_]+)=([A-Za-z0-9._-]+)$")
    set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS deps.lock)
FetchContent_Declare(
  minhook
  URL "https://github.com/TsudaKageyu/minhook/archive/refs/tags/${MINHOOK_TAG}.tar.gz"
  URL_HASH "SHA256=${MINHOOK_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(imgui
  URL "https://github.com/ocornut/imgui/archive/refs/tags/${IMGUI_TAG}.tar.gz"
  URL_HASH "SHA256=${IMGUI_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(minhook imgui)

add_library(imgui_optimizer STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp)
target_include_directories(imgui_optimizer PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_optimizer PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

option(SANFRAME_EMBED_LOCAL_FONT "Embed a locally supplied BankGothic font" ON)
set(LOCAL_FONT "${CMAKE_CURRENT_SOURCE_DIR}/resources/BankGothic-Bold.ttf")
set(GENERATED_RC "${CMAKE_CURRENT_BINARY_DIR}/SanFrameOptimizer.generated.rc")
set(RC_CONTENT "#include \"resource.h\"\n")
if(SANFRAME_EMBED_LOCAL_FONT AND EXISTS "${LOCAL_FONT}")
  file(TO_CMAKE_PATH "${LOCAL_FONT}" LOCAL_FONT_RC)
  string(APPEND RC_CONTENT "IDR_BANKGOTHIC_BOLD_TTF RCDATA \"${LOCAL_FONT_RC}\"\n")
  set(SANFRAME_HAS_EMBEDDED_FONT TRUE)
  message(STATUS "OSD font: embedding local BankGothic Bold")
else()
  set(SANFRAME_HAS_EMBEDDED_FONT FALSE)
  message(STATUS "OSD font: scalable ImGui fallback (local font absent or embedding disabled)")
endif()
file(GENERATE OUTPUT "${GENERATED_RC}" CONTENT "${RC_CONTENT}")




# Compile embedded D3D9 shaders as part of build.bat, without a runtime compiler.
find_program(SANFRAME_FXC fxc.exe HINTS
  "$ENV{WindowsSdkVerBinPath}/x86"
  "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x86"
  REQUIRED)
foreach(STAGE IN ITEMS VS PS)
  string(TOLOWER "${STAGE}" PROFILE)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SanFrameInstance${STAGE}.hpp"
    COMMAND "${SANFRAME_FXC}" /nologo /O3 /T "${PROFILE}_3_0" /E "${STAGE}"
      /Vn "SanFrameInstance${STAGE}" /Fh "${CMAKE_CURRENT_BINARY_DIR}/SanFrameInstance${STAGE}.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/StaticPropInstancing.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/StaticPropInstancing.hlsl"
    VERBATIM)
endforeach()

add_custom_target(instance_shaders DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/SanFrameInstanceVS.hpp"
  "${CMAKE_CURRENT_BINARY_DIR}/SanFrameInstancePS.hpp")
include(cmake/Fxp.cmake)

add_library(SanFrameOptimizer SHARED
  src/dllmain.cpp
  src/Logger.cpp
  src/Diagnostics.cpp
  src/Config.cpp
  src/HookManager.cpp
  src/Plugin.cpp
  src/FxpOptimizer.cpp
  src/DxvkTuner.cpp
  src/DxvkConfigFile.cpp
  src/FramePacer.cpp
  src/D3D9Hook.cpp
  src/NativeVulkan.cpp
  src/Overlay.cpp
  src/StreamingMemoryTelemetry.cpp
  src/UsageTelemetry.cpp
  src/GpuMemoryTelemetry.cpp
  src/Cache64MemoryTelemetry.cpp
  src/DrawTelemetry.cpp
  ${GENERATED_RC}
  SanFrameOptimizer.def
  src/OptimizationRuntime.cpp
  src/OptimizationReport.cpp
  src/D3D9StateCache.cpp
  src/SkygfxCompatibility.cpp
  src/RenderWareBatcher.cpp
  src/EnginePass.cpp
  src/EngineOptimizations.cpp
  src/RenderSubmission.cpp
  src/PassProfiler.cpp
  src/SavoCompatibility.cpp
  src/GtaSa10US.cpp
  src/ExecutableInfo.cpp
)
add_dependencies(SanFrameOptimizer instance_shaders)
target_include_directories(SanFrameOptimizer PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(SanFrameOptimizer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SanFrameOptimizer PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(SanFrameOptimizer PRIVATE minhook imgui_optimizer sanframe_fxp d3d9 user32 avrt winmm version psapi pdh dxgi ole32 oleaut32 wbemuuid)
set_property(TARGET SanFrameOptimizer imgui_optimizer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set_target_properties(SanFrameOptimizer PROPERTIES OUTPUT_NAME "SanFrameOptimizer" SUFFIX ".asi")
add_custom_command(TARGET SanFrameOptimizer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/config/SanFrameOptimizer.ini"
    "$<TARGET_FILE_DIR:SanFrameOptimizer>/SanFrameOptimizer.ini"
)

if(BUILD_TESTING)
  add_executable(overlay_hook_policy_test tests/OverlayHookPolicy.cpp)
  target_include_directories(overlay_hook_policy_test PRIVATE include)
  target_compile_features(overlay_hook_policy_test PRIVATE cxx_std_23)
  add_test(NAME overlay_hook_policy COMMAND overlay_hook_policy_test)

  add_executable(upload_ring_test tests/UploadRing.cpp)
  target_include_directories(upload_ring_test PRIVATE include)
  target_compile_features(upload_ring_test PRIVATE cxx_std_23)
  add_test(NAME upload_ring COMMAND upload_ring_test)

  add_executable(engine_pass_algorithms tests/EnginePassAlgorithms.cpp)
  target_include_directories(engine_pass_algorithms PRIVATE include)
  target_compile_features(engine_pass_algorithms PRIVATE cxx_std_23)
  add_test(NAME engine_pass_algorithms COMMAND engine_pass_algorithms)
  add_executable(streaming_memory_accounting_test tests/StreamingMemoryAccounting.cpp)
  target_include_directories(streaming_memory_accounting_test PRIVATE include)
  target_compile_features(streaming_memory_accounting_test PRIVATE cxx_std_23)
  add_test(NAME streaming_memory_accounting COMMAND streaming_memory_accounting_test)
  add_executable(overlay_font_test tests/OverlayFont.cpp)
  target_include_directories(overlay_font_test PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(overlay_font_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(overlay_font_test PRIVATE imgui_optimizer)
  add_dependencies(overlay_font_test SanFrameOptimizer)
  add_test(NAME overlay_font COMMAND overlay_font_test
    $<TARGET_FILE:SanFrameOptimizer> "${SANFRAME_HAS_EMBEDDED_FONT}" "${LOCAL_FONT}")
  add_executable(renderer_detection_test
    tests/RendererDetection.cpp src/DxvkTuner.cpp src/DxvkConfigFile.cpp
    src/Config.cpp src/Logger.cpp src/Diagnostics.cpp)
  target_include_directories(renderer_detection_test PRIVATE include)
  target_compile_definitions(renderer_detection_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(renderer_detection_test PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(renderer_detection_test PRIVATE version)
  add_library(refx_asi_test_fixture SHARED tests/ModFixture.cpp)
  set_target_properties(refx_asi_test_fixture PROPERTIES OUTPUT_NAME "ReFXDXVK"
    PREFIX "" SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
  add_dependencies(renderer_detection_test refx_asi_test_fixture)
  add_test(NAME renderer_detection COMMAND renderer_detection_test
    $<TARGET_FILE:refx_asi_test_fixture>)
  add_executable(dxvk_ownership_test tests/DxvkOwnership.cpp src/DxvkConfigFile.cpp)
  target_include_directories(dxvk_ownership_test PRIVATE include)
  target_compile_definitions(dxvk_ownership_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(dxvk_ownership_test PRIVATE /W4 /permissive- /EHsc)
  add_test(NAME dxvk_ownership COMMAND dxvk_ownership_test)

  add_library(skygfx_test_fixture SHARED tests/ModFixture.cpp)
  set_target_properties(skygfx_test_fixture PROPERTIES OUTPUT_NAME "skygfx_sanframe_test" SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
  add_library(proper_shaders_test_fixture SHARED tests/ModFixture.cpp)
  set_target_properties(proper_shaders_test_fixture PROPERTIES OUTPUT_NAME "ProperShaders" SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
  add_executable(skygfx_constants_test tests/SkygfxConstants.cpp
    src/D3D9StateCache.cpp src/SkygfxCompatibility.cpp src/DrawTelemetry.cpp src/Logger.cpp src/Diagnostics.cpp)
  target_include_directories(skygfx_constants_test PRIVATE include)
  target_compile_definitions(skygfx_constants_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(skygfx_constants_test PRIVATE /W4 /permissive- /EHsc)
  add_dependencies(skygfx_constants_test skygfx_test_fixture proper_shaders_test_fixture)
  add_test(NAME skygfx_constants COMMAND skygfx_constants_test
    $<TARGET_FILE:skygfx_test_fixture> $<TARGET_FILE:proper_shaders_test_fixture>)

  add_executable(submission_safety_test tests/SubmissionSafety.cpp
    src/D3D9StateCache.cpp src/SkygfxCompatibility.cpp src/DrawTelemetry.cpp
    src/Logger.cpp src/Diagnostics.cpp)
  add_dependencies(submission_safety_test instance_shaders)
  target_include_directories(submission_safety_test PRIVATE include ${CMAKE_CURRENT_BINARY_DIR})
  target_compile_definitions(submission_safety_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(submission_safety_test PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(submission_safety_test PRIVATE d3d9)
  add_test(NAME submission_safety COMMAND submission_safety_test)
endif()
