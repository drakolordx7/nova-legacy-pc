# nova_game: boot driver (Java start-up without Java), input / lifecycle forwarding, anti-tamper patches.
add_library(nova_game STATIC game.cpp patches.cpp iap.cpp)
target_link_libraries(nova_game PUBLIC nova_jni nova_loader nova_cpu nova_pc)
target_include_directories(nova_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
