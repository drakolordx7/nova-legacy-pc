# Smoke test: hidden SDL3 window + ANGLE context, driven from an AArch64 guest blob through the EGL/GLES HLE thunks.
nova_guest_blob(gfx ${CMAKE_CURRENT_SOURCE_DIR}/guest.c 0x40000000)
add_executable(nova_test_gfx gfx_test.cpp)
add_dependencies(nova_test_gfx gfx_blob)
target_include_directories(nova_test_gfx PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gfx)
target_link_libraries(nova_test_gfx PRIVATE nova_gfx)
nova_copy_gfx_dlls(nova_test_gfx)
add_test(NAME gfx COMMAND nova_test_gfx)
