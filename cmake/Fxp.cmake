# Standalone FXP dependencies.
# FX runtime uses offset-checked GTA SA ABI declarations in src/fxp/GtaFxAbi.h.
# Do not fetch Plugin-SDK: its bundled SafetyHook headers collide with the
# separately pinned SafetyHook used by SanFrame. No Premake or Plugin.lib.
#
FetchContent_Declare(safetyhook
  URL "https://github.com/cursey/safetyhook/archive/${SAFETYHOOK_REV}.tar.gz"
  URL_HASH "SHA256=${SAFETYHOOK_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(safetyhook)

# Embed shaders as generated C++ headers; no runtime HLSL compilation.
foreach(STAGE IN ITEMS VS PS)
  string(TOLOWER "${STAGE}" PROFILE)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SanFrameFxp${STAGE}.hpp"
    COMMAND "${SANFRAME_FXC}" /nologo /O3 /T "${PROFILE}_3_0"
      /E "${STAGE}" /Vn "SanFrameFxp${STAGE}"
      /Fh "${CMAKE_CURRENT_BINARY_DIR}/SanFrameFxp${STAGE}.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/FxpTriangles.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/FxpTriangles.hlsl"
    VERBATIM)
endforeach()

add_library(sanframe_fxp STATIC
  src/fxp/runtime/Engine.cpp
  src/fxp/runtime/OperatorPlans.cpp
  src/fxp/renderer/RenderStream.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/SanFrameFxpVS.hpp"
  "${CMAKE_CURRENT_BINARY_DIR}/SanFrameFxpPS.hpp")
target_include_directories(sanframe_fxp PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_definitions(sanframe_fxp PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(sanframe_fxp PRIVATE /W4 /permissive- /EHsc)
if(TARGET safetyhook::safetyhook)
  target_link_libraries(sanframe_fxp PUBLIC safetyhook::safetyhook)
elseif(TARGET safetyhook)
  target_link_libraries(sanframe_fxp PUBLIC safetyhook)
else()
  message(FATAL_ERROR "Pinned SafetyHook dependency did not create a CMake target.")
endif()
target_link_libraries(sanframe_fxp PUBLIC d3d9)
