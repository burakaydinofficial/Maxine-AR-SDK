cmake_minimum_required(VERSION 3.12)
project(nvar_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------------
# Bridge shared library
#
# The bridge does NOT link against nvARPose or NVCVImage at build time.
# Instead, it loads them at runtime via dlopen/LoadLibrary (lazy loading).
# This means the build succeeds even without the SDK installed.
# --------------------------------------------------------------------------

add_library(nvar_bridge SHARED
    src/nvar_bridge.cpp
)

target_include_directories(nvar_bridge
    PUBLIC  include
    PRIVATE ${CMAKE_SOURCE_DIR}/nvar/include
)

# --------------------------------------------------------------------------
# Platform-specific settings
# --------------------------------------------------------------------------

if(WIN32)
    # NVAR_BRIDGE_EXPORT enables __declspec(dllexport) on public symbols
    target_compile_definitions(nvar_bridge PRIVATE NVAR_BRIDGE_EXPORT)
else()
    # Hide all symbols by default, only export those marked with NVAR_API
    target_compile_options(nvar_bridge PRIVATE -fvisibility=hidden)
    target_compile_definitions(nvar_bridge PRIVATE NVAR_BRIDGE_EXPORT)
    # Need -ldl for dlopen/dlsym
    target_link_libraries(nvar_bridge PRIVATE ${CMAKE_DL_LIBS})
endif()

# Suppress warnings from SDK headers
target_compile_definitions(nvar_bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
