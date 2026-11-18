// Makes nova.exe import mimalloc.dll first, before anything allocates: with the DLL override, mimalloc-redirect only
// takes over the CRT allocator when the DLL is loaded early, and the loader only loads it if the exe references it.
// Dynamic initialisers of this TU run in the CRT startup, before main(); the call also keeps the import alive.
#include <mimalloc.h>

namespace {
const int g_mimalloc_version = mi_version();
}
extern "C" int nova_mimalloc_version() { return g_mimalloc_version; }
