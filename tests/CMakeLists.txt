# Guest code for the tests is built from C with clang (--target=aarch64-linux-android) by tools/build_guest.py.
# If the LLVM tools are not found, the guest-based tests are skipped with a warning.
find_package(Python3 COMPONENTS Interpreter)
find_program(NOVA_LLVM_CLANG NAMES clang clang.exe
  HINTS "C:/Program Files/LLVM/bin" NO_DEFAULT_PATH)
if(NOT NOVA_LLVM_CLANG)
  find_program(NOVA_LLVM_CLANG NAMES clang clang.exe)
endif()

if(Python3_FOUND AND NOVA_LLVM_CLANG)
  get_filename_component(NOVA_LLVM_BIN "${NOVA_LLVM_CLANG}" DIRECTORY)

  # build_guest_blob(<name> <src.c> <base>) -> ${CMAKE_CURRENT_BINARY_DIR}/<name>/guest_blob.h
  function(nova_guest_blob name src base)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}/guest_blob.h")
    add_custom_command(OUTPUT "${out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/${name}"
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/build_guest.py"
              --llvm-bin "${NOVA_LLVM_BIN}" --src "${src}" --out "${out}" --base ${base}
      DEPENDS "${src}" "${CMAKE_SOURCE_DIR}/tools/build_guest.py"
      COMMENT "Building AArch64 guest blob ${name}")
    add_custom_target(${name}_blob DEPENDS "${out}")
  endfunction()

  nova_guest_blob(spike ${CMAKE_CURRENT_SOURCE_DIR}/spike/guest.c 0x40000000)
  add_executable(nova_test_spike spike/spike_test.cpp)
  add_dependencies(nova_test_spike spike_blob)
  target_include_directories(nova_test_spike PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/spike)
  target_link_libraries(nova_test_spike PRIVATE nova_cpu)
  add_test(NAME spike COMMAND nova_test_spike)

  nova_guest_blob(abi ${CMAKE_CURRENT_SOURCE_DIR}/abi/guest.c 0x40000000)
  add_executable(nova_test_abi abi/abi_test.cpp)
  add_dependencies(nova_test_abi abi_blob)
  target_include_directories(nova_test_abi PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/abi)
  target_link_libraries(nova_test_abi PRIVATE nova_cpu)
  add_test(NAME abi COMMAND nova_test_abi)

  add_subdirectory(jni)
  add_subdirectory(gfx)
  add_subdirectory(audio)
  nova_guest_blob(libc ${CMAKE_CURRENT_SOURCE_DIR}/libc/guest.c 0x40000000)
  add_executable(nova_test_libc libc/libc_test.cpp)
  add_dependencies(nova_test_libc libc_blob)
  target_include_directories(nova_test_libc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libc ${CMAKE_CURRENT_SOURCE_DIR}/libc)
  target_link_libraries(nova_test_libc PRIVATE nova_libc)
  add_test(NAME libc COMMAND nova_test_libc)
else()
  message(WARNING "Python3 or LLVM clang not found: guest-based tests (spike) are skipped")
endif()
