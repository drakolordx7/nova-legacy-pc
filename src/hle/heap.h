// Guest heap behind malloc/free: size-class free lists over chunks of the Heap zone; large blocks map directly.
// Every block has a 16-byte header {capacity, original pointer} so free/realloc/malloc_usable_size and aligned
// allocations work from the user pointer alone. Thread-safe.
#pragma once
#include "cpu/guest_memory.h"

namespace nova::hle::heap {
gaddr Alloc(gsize size);                      // 16-byte aligned, contents undefined; 0 on failure
gaddr AllocAligned(gsize align, gsize size);  // align power of two
gaddr Calloc(gsize n, gsize size);
gaddr Realloc(gaddr p, gsize size);
void Free(gaddr p);
gsize UsableSize(gaddr p);
}  // namespace nova::hle::heap
