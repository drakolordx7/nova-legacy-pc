// Free, instant, offline in-app purchases (docs/iap.md). The Java side (jni IAPLib) answers the catalogue and the
// purchase; these patches remove the native "no network" gates and start the feed load that the dead servers never
// triggered. Call before any guest thread starts.
#pragma once
#include "loader/elf64.h"

namespace nova::game {

// Returns the number of functions patched.
int ApplyIapPatches(const loader::Module& module);

}  // namespace nova::game
