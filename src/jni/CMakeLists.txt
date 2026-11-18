# nova_jni: the fake Java side (JavaVM/JNIEnv for the A64 guest, object model, emulated classes).
add_library(nova_jni STATIC runtime.cpp env.cpp classes_android.cpp classes_game.cpp classes_iap.cpp jni.cpp)
target_link_libraries(nova_jni PUBLIC nova_libc nova_loader nova_cpu bcrypt)
target_include_directories(nova_jni PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
