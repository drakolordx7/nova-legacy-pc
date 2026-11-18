# nova_libc: bionic LP64 HLE. RegisterMinimal() is the Phase 1 core (heap, basic string/printf, liblog, dl_iterate_phdr);
# RegisterAll() adds the full layer: pthread, VFS + stdio + POSIX files, libm, string/wchar/time, runtime (setjmp, signals,
# system properties, qsort...). Later registrations override the minimal handlers of the same name.
add_library(nova_libc STATIC
  heap.cpp format.cpp libc.cpp libc_extra.cpp
  register.cpp pthread.cpp runtime.cpp vfs.cpp posix.cpp stdio.cpp string.cpp wchar.cpp time.cpp math.cpp)
target_link_libraries(nova_libc PUBLIC nova_loader synchronization bcrypt)
target_include_directories(nova_libc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
