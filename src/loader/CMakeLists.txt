add_library(nova_loader STATIC elf64.cpp)
target_link_libraries(nova_loader PUBLIC nova_cpu)
target_include_directories(nova_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
