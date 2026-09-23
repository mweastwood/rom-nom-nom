#include "boot.h"

#ifdef TARGET_N64
#include "src/c/harvest-moon-64/common.h"
#else
#include "mocks/ultra64/ultra64_mock.h"
#endif

namespace hm64 {

static std::function<void()> s_idle_callback = nullptr;

void BootSystem::Main() {
  os_initialize();
  os_create_thread(&D_801241C0, static_cast<OSId>(ThreadId::kIdle), Idle, nullptr, D_80126520,
                   static_cast<OSPri>(Priority::kIdleActive));
  os_start_thread(&D_801241C0);
}

void BootSystem::Idle(void* arg) {
  (void)arg;
  ClearIdleCallback();
  D_801FD628 = nullptr;

  func_800FD5B0();
  func_800FB140(2, 1);
  func_80105B00(90);

  os_create_thread(&D_80124370, static_cast<OSId>(ThreadId::kMainProc), mainproc, nullptr,
                   D_801C6220, static_cast<OSPri>(Priority::kMainProc));
  os_start_thread(&D_80124370);
  os_set_thread_priority(&D_801241C0, static_cast<OSPri>(Priority::kIdleDormant));

  while (true) {
    if (D_801FD628 != nullptr) {
      D_801FD628();
    } else if (s_idle_callback) {
      s_idle_callback();
    }
  }
}

void BootSystem::SetIdleCallback(std::function<void()> callback) {
  s_idle_callback = std::move(callback);
}

void BootSystem::ClearIdleCallback() {
  s_idle_callback = nullptr;
}

bool BootSystem::HasIdleCallback() {
  return s_idle_callback != nullptr || D_801FD628 != nullptr;
}

void BootSystem::ExecuteIdleCallback() {
  if (D_801FD628 != nullptr) {
    D_801FD628();
  } else if (s_idle_callback) {
    s_idle_callback();
  }
}

}  // namespace hm64
