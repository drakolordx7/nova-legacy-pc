# Dependencies for Phase 1: dynarmic (A64 frontend + x64 backend, pinned) and Boost headers from vcpkg.
# No SDL / ANGLE yet.
# Adapted from Zombiewood's cmake/ZwDependencies.cmake (MIT, (c) 2026 ChrisDutary).
find_package(Boost REQUIRED)

# mimalloc (vcpkg, "override" feature): replaces malloc/free/new/delete for the whole process (the CRT's allocator is
# redirected at load time by mimalloc.dll + mimalloc-redirect.dll). dynarmic's translator allocates heavily and the
# Windows heap was 15-20% of its time. The dependency is attached to nova.exe in src/CMakeLists.txt.
find_package(mimalloc CONFIG REQUIRED)

# Graphics: SDL3 + ANGLE (cmake/NovaGfxDeps.cmake)
include(NovaGfxDeps)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- dynarmic (azahar-emu fork, A64 frontend, x64 backend), pinned commit ----
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dynarmic
  GIT_REPOSITORY https://github.com/azahar-emu/dynarmic.git
  GIT_TAG        a46601580d5512d324104f985b5f0209dc980ddc
  GIT_SUBMODULES_RECURSE TRUE)

# dynarmic and its externals are compiled optimised even in Debug: an unoptimised JIT makes guest startup
# several times slower. Only /Od /RTC1 become /O2 /Ob2; the CRT stays the debug one so it links with our
# code. NOVA_OPTIMIZE_DEPS_IN_DEBUG=OFF disables this (for debugging dynarmic itself).
option(NOVA_OPTIMIZE_DEPS_IN_DEBUG "Compile dynarmic optimised in the Debug configuration" ON)
set(_nova_saved_c_debug "${CMAKE_C_FLAGS_DEBUG}")
set(_nova_saved_cxx_debug "${CMAKE_CXX_FLAGS_DEBUG}")
if(MSVC AND NOVA_OPTIMIZE_DEPS_IN_DEBUG)
  foreach(_v CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
    string(REGEX REPLACE "/RTC[1csu]+" "" ${_v} "${${_v}}")
    string(REGEX REPLACE "/O[d012xs]" "" ${_v} "${${_v}}")
    string(REGEX REPLACE "/Ob[0-3]" "" ${_v} "${${_v}}")
    set(${_v} "${${_v}} /O2 /Ob2")
  endforeach()
endif()
FetchContent_MakeAvailable(dynarmic)
set(CMAKE_C_FLAGS_DEBUG "${_nova_saved_c_debug}")
set(CMAKE_CXX_FLAGS_DEBUG "${_nova_saved_cxx_debug}")

# Local patch to dynarmic (the same one Zombiewood applies): the Windows exception handler calls the fastmem
# callback after reserving only 8 bytes of stack, but the x64 ABI needs 32 bytes of shadow space (+8 for
# alignment). The callee writes into that space and clobbers the handler's return address, so the first
# fastmem fault crashes. The handler is shared by the A32 and A64 backends, so A64 needs the same fix.
# Idempotent; applied at every configure to the already downloaded tree.
set(_nova_dyn_eh "${dynarmic_SOURCE_DIR}/src/dynarmic/backend/x64/exception_handler_windows.cpp")
if(EXISTS "${_nova_dyn_eh}")
  file(READ "${_nova_dyn_eh}" _nova_dyn_eh_src)
  string(FIND "${_nova_dyn_eh_src}" "code.sub(code.rsp, 8);" _nova_dyn_eh_pos)
  if(NOT _nova_dyn_eh_pos EQUAL -1)
    string(REPLACE "code.sub(code.rsp, 8);" "code.sub(code.rsp, 0x28);  // nova: shadow space" _nova_dyn_eh_src "${_nova_dyn_eh_src}")
    string(REPLACE "code.add(code.rsp, 8);" "code.add(code.rsp, 0x28);  // nova: shadow space" _nova_dyn_eh_src "${_nova_dyn_eh_src}")
    file(WRITE "${_nova_dyn_eh}" "${_nova_dyn_eh_src}")
    message(STATUS "dynarmic: shadow-space patch applied to exception_handler_windows.cpp")
  else()
    string(FIND "${_nova_dyn_eh_src}" "code.sub(code.rsp, 0x28);" _nova_dyn_eh_pos)
    if(_nova_dyn_eh_pos EQUAL -1)
      message(FATAL_ERROR "dynarmic: exception_handler_windows.cpp not recognised; review the fastmem patch")
    endif()
  endif()
else()
  message(FATAL_ERROR "dynarmic: missing ${_nova_dyn_eh}")
endif()
