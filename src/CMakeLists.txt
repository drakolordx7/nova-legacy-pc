add_subdirectory(core)
add_subdirectory(cpu)
add_subdirectory(loader)
add_subdirectory(hle)
add_subdirectory(jni)
add_subdirectory(pc)
add_subdirectory(game)
add_subdirectory(gfx)
add_subdirectory(audio)
add_subdirectory(android)

add_executable(nova main.cpp mimalloc_anchor.cpp)
# mimalloc first so its DLL is the first import (see mimalloc_anchor.cpp).
target_link_libraries(nova PRIVATE mimalloc nova_libc nova_game nova_gfx nova_audio nova_android nova_jni SDL3::SDL3)

# mimalloc.dll / mimalloc-redirect.dll next to nova.exe (vcpkg's applocal step normally does it; this makes it certain).
add_custom_command(TARGET nova POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:mimalloc>" "$<TARGET_FILE_DIR:nova>"
  COMMAND ${CMAKE_COMMAND} -DSRC_DIR=$<TARGET_FILE_DIR:mimalloc> -DDST_DIR=$<TARGET_FILE_DIR:nova>
          -P ${CMAKE_SOURCE_DIR}/cmake/CopyMimallocRedirect.cmake
  COMMENT "Copying mimalloc DLLs next to nova")
