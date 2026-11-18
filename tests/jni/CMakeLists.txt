# Fake-JNI test: an AArch64 guest (tests/jni/guest.c) calls through the host's JNIEnv / JavaVM tables.
# Included from tests/CMakeLists.txt inside the "guest blobs available" branch (nova_guest_blob is defined there).
nova_guest_blob(jni ${CMAKE_CURRENT_SOURCE_DIR}/guest.c 0x40000000)
add_executable(nova_test_jni jni_test.cpp)
add_dependencies(nova_test_jni jni_blob)
target_include_directories(nova_test_jni PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/jni)
target_link_libraries(nova_test_jni PRIVATE nova_jni)
add_test(NAME jni COMMAND nova_test_jni)
