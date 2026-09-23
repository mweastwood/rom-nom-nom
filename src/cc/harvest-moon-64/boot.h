#ifndef HARVEST_MOON_64_BOOT_H
#define HARVEST_MOON_64_BOOT_H

#include <functional>

#include "src/c/harvest-moon-64/common.h"

namespace hm64 {

enum class ThreadId : OSId {
  Idle = 1,
  MainProc = 3,
};

enum class Priority : OSPri {
  IdleActive = 10,
  IdleDormant = 0,
  MainProc = 10,
};

class BootSystem {
 public:
  static void Main();
  static void Idle(void* arg = nullptr);

  static void SetIdleCallback(std::function<void()> callback);
  static void ClearIdleCallback();
  static bool HasIdleCallback();
  static void ExecuteIdleCallback();
};

}  // namespace hm64

#endif  // HARVEST_MOON_64_BOOT_H
