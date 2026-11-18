add_library(nova_core STATIC log.cpp)
target_include_directories(nova_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
