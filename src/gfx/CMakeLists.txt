# nova_gfx: SDL3 window, ANGLE (D3D11) display/contexts, the framebuffer-0 redirect, and the guest-facing HLE for
# libEGL, libGLESv2 and libandroid's ANativeWindow_* (src/android/native_window.cpp is compiled in here because it needs
# the window API). Public entry points: gfx/gfx.h.
add_library(nova_gfx STATIC
  gfx.cpp
  hle_egl.cpp
  hle_gles.cpp
  texdecode.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../android/native_window.cpp)
target_link_libraries(nova_gfx PUBLIC nova_cpu nova_libc SDL3::SDL3 unofficial::angle::libEGL unofficial::angle::libGLESv2)
target_include_directories(nova_gfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# nova.exe is defined in src/CMakeLists.txt after this directory: attach the DLL copy at the end of that directory's
# processing so SDL3.dll and ANGLE's libEGL/libGLESv2 end up next to it.
function(nova_gfx_attach_dlls_to_nova)
  if(TARGET nova)
    nova_copy_gfx_dlls(nova)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." CALL nova_gfx_attach_dlls_to_nova)
