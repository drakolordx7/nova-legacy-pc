# Graphics dependencies: SDL3 (window, events, raw mouse) and ANGLE (OpenGL ES over Direct3D 11) from vcpkg.
#   vcpkg port `sdl3`  -> find_package(SDL3 CONFIG) -> SDL3::SDL3
#   vcpkg port `angle` -> find_package(unofficial-angle CONFIG) -> unofficial::angle::libEGL / libGLESv2
# Adapted from Zombiewood's cmake/ZwDependencies.cmake (MIT, (c) 2026 ChrisDutary); SDL2 became SDL3.
find_package(SDL3 CONFIG REQUIRED)
find_package(unofficial-angle CONFIG REQUIRED)

# Full paths of ANGLE's runtime DLLs (libEGL.dll, libGLESv2.dll and the zlib DLL the vcpkg build imports).
set(NOVA_ANGLE_DLLS "")
foreach(_t libEGL libGLESv2)
  set(_loc "")
  foreach(_prop IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION)
    if(NOT _loc)
      get_target_property(_loc unofficial::angle::${_t} ${_prop})
    endif()
  endforeach()
  if(NOT _loc)
    message(FATAL_ERROR "ANGLE: no DLL location for unofficial::angle::${_t}")
  endif()
  list(APPEND NOVA_ANGLE_DLLS "${_loc}")
endforeach()
list(GET NOVA_ANGLE_DLLS 0 _nova_angle_first)
get_filename_component(_nova_angle_bin "${_nova_angle_first}" DIRECTORY)
foreach(_extra z.dll zlib1.dll)
  if(EXISTS "${_nova_angle_bin}/${_extra}")
    list(APPEND NOVA_ANGLE_DLLS "${_nova_angle_bin}/${_extra}")
  endif()
endforeach()
message(STATUS "ANGLE DLLs: ${NOVA_ANGLE_DLLS}")

# Copies ANGLE's DLLs (libEGL, libGLESv2, z) and SDL3.dll next to <target>'s output after every build.
function(nova_copy_gfx_dlls target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NOVA_ANGLE_DLLS} "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Copying ANGLE (libEGL/libGLESv2) and SDL3 next to ${target}")
endfunction()
