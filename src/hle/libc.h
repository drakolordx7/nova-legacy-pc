// bionic LP64 libc/libm/libdl/liblog HLE. RegisterAll() is the complete layer (pthread, stdio on a VFS, POSIX files,
// libm, string/wchar/time, setjmp, signals, system properties...); RegisterMinimal() is the Phase 1 subset that was
// enough to run a library's constructors. Call one of them before loading the module. The data root of the VFS is
// set with nova::vfs::SetRoot() (hle/vfs.h).
#pragma once

namespace nova::hle::libc {
void RegisterAll();
void RegisterMinimal();
void RegisterExtra();  // called by RegisterMinimal
}
