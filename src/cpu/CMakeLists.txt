# nova_cpu: guest memory window, import registry/stubs, A64 CPU + call_guest + threads, debug helpers.
add_library(nova_cpu STATIC guest_memory.cpp hle.cpp cpu.cpp)
target_link_libraries(nova_cpu PUBLIC nova_core dynarmic)
target_include_directories(nova_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
