# nova_pc: the PC layer (settings file, keyboard/mouse state, guest hooks for raw look, Hud input, frame timing...).
add_library(nova_pc STATIC config.cpp input.cpp hook.cpp layer.cpp)
target_link_libraries(nova_pc PUBLIC nova_core nova_cpu nova_loader SDL3::SDL3)
target_include_directories(nova_pc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
