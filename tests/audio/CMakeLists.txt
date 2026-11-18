# OpenSL ES test: needs nova_guest_blob() from tests/CMakeLists.txt (clang + Python), so it is added from inside that block.
nova_guest_blob(audio ${CMAKE_CURRENT_SOURCE_DIR}/guest.c 0x40000000)
add_executable(nova_test_audio audio_test.cpp)
add_dependencies(nova_test_audio audio_blob)
target_include_directories(nova_test_audio PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/audio)
target_link_libraries(nova_test_audio PRIVATE nova_audio)
add_test(NAME audio COMMAND nova_test_audio)

# AAssetManager over the real APK (skips itself when the APK is not there).
add_executable(nova_test_android_assets android_assets_test.cpp)
target_link_libraries(nova_test_android_assets PRIVATE nova_android)
add_test(NAME android_assets COMMAND nova_test_android_assets)
