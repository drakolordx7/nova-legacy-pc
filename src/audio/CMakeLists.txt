# nova_audio: OpenSL ES HLE (slCreateEngine and the engine/output-mix/player/buffer-queue interfaces) over SDL3 streams.
find_package(SDL3 CONFIG REQUIRED)
add_library(nova_audio STATIC opensl.cpp sink.cpp)
target_link_libraries(nova_audio PUBLIC nova_cpu nova_libc PRIVATE SDL3::SDL3)
target_include_directories(nova_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
