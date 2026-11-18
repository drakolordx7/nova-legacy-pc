# nova_android: libandroid imports that are not about the window (assets over the APK zip, looper, sensors).
add_library(nova_android STATIC android.cpp assets.cpp sensors.cpp zip.cpp)
target_link_libraries(nova_android PUBLIC nova_cpu nova_libc)
target_include_directories(nova_android PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
