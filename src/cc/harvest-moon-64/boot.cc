#include "boot.h"

#ifdef TARGET_N64
#include "src/c/harvest-moon-64/types.h"
#else
#include "mocks/ultra64/ultra64_mock.h"
#endif

namespace hm64 {

static std::function<void()> s_idle_callback = nullptr;

void BootSystem::Main() {
  os_initialize();
  os_create_thread(&g_idle_thread, static_cast<OSId>(ThreadId::kIdle), Idle, nullptr,
                   g_idle_thread_stack, static_cast<OSPri>(Priority::kIdleActive));
  os_start_thread(&g_idle_thread);
}

void BootSystem::Idle(void* arg) {
  (void)arg;
  ClearIdleCallback();
  g_idle_callback = nullptr;

  func_800FD5B0();
  func_800FB140(2, 1);
  func_80105B00(90);

  os_create_thread(&g_main_thread, static_cast<OSId>(ThreadId::kMainProc), mainproc, nullptr,
                   g_main_thread_stack, static_cast<OSPri>(Priority::kMainProc));
  os_start_thread(&g_main_thread);
  os_set_thread_priority(&g_idle_thread, static_cast<OSPri>(Priority::kIdleDormant));

  while (true) {
    if (g_idle_callback != nullptr) {
      g_idle_callback();
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
  return s_idle_callback != nullptr || g_idle_callback != nullptr;
}

void BootSystem::ExecuteIdleCallback() {
  if (g_idle_callback != nullptr) {
    g_idle_callback();
  } else if (s_idle_callback) {
    s_idle_callback();
  }
}

}  // namespace hm64
