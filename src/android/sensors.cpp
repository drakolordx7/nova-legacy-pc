// ALooper and the sensor API. The PC has no sensors: the manager exists, getDefaultSensor returns NULL for every
// type, and AndroidSensorManager::AndroidInitSensors takes its "no sensor" path (it checks every pointer).
#include <mutex>

#include "android/android.h"
#include "android/internal.h"
#include "cpu/hle_abi.h"
#include "hle/heap.h"

namespace nova::android {
namespace {
constexpr std::int64_t kEinval = -22;

gaddr Dummy(gaddr& slot) {
  static std::mutex mu;
  std::lock_guard lk(mu);
  if (!slot) {
    slot = hle::heap::Alloc(16);
    if (slot) {
      mem::W64(slot, 0);
      mem::W64(slot + 8, 0);
    }
  }
  return slot;
}
gaddr g_looper = 0, g_manager = 0;

std::uint64_t H_looperForThread() { return Dummy(g_looper); }
std::uint64_t H_looperPrepare(std::int32_t /*opts*/) { return Dummy(g_looper); }

std::uint64_t H_managerGetInstance() { return Dummy(g_manager); }
std::uint64_t H_getDefaultSensor(std::uint64_t /*mgr*/, std::int32_t /*type*/) { return 0; }
std::uint64_t H_createEventQueue(std::uint64_t /*mgr*/, std::uint64_t /*looper*/, std::int32_t /*ident*/, std::uint64_t /*cb*/,
                                 std::uint64_t /*data*/) {
  return 0;
}
std::int32_t H_queueEnable(std::uint64_t /*queue*/, std::uint64_t /*sensor*/) { return static_cast<std::int32_t>(kEinval); }
std::int32_t H_queueSetRate(std::uint64_t /*queue*/, std::uint64_t /*sensor*/, std::int32_t /*usec*/) {
  return static_cast<std::int32_t>(kEinval);
}
std::int64_t H_queueGetEvents(std::uint64_t /*queue*/, std::uint64_t /*events*/, std::uint64_t /*count*/) { return 0; }
}  // namespace

void RegisterSensors() {
  NOVA_HLE("ALooper_forThread", H_looperForThread);
  NOVA_HLE("ALooper_prepare", H_looperPrepare);
  NOVA_HLE("ASensorManager_getInstance", H_managerGetInstance);
  NOVA_HLE("ASensorManager_getDefaultSensor", H_getDefaultSensor);
  NOVA_HLE("ASensorManager_createEventQueue", H_createEventQueue);
  NOVA_HLE("ASensorEventQueue_enableSensor", H_queueEnable);
  NOVA_HLE("ASensorEventQueue_disableSensor", H_queueEnable);
  NOVA_HLE("ASensorEventQueue_setEventRate", H_queueSetRate);
  NOVA_HLE("ASensorEventQueue_getEvents", H_queueGetEvents);
}
}  // namespace nova::android
