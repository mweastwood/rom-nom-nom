#include <gtest/gtest.h>
#include <setjmp.h>

#include "boot_test_interface.h"
#include "mocks/ultra64/ultra64_mock.h"

// Global state for escaping idle's while(1) loop via longjmp
static jmp_buf s_idle_escape_buf;
static int s_callback_invocations = 0;

static void TestIdleCallback(void) {
  s_callback_invocations++;
  longjmp(s_idle_escape_buf, 1);
}

static void OnSetPriorityHook(OSThread* thread, OSPri pri) {
  (void)thread;
  (void)pri;
  // Arm callback pointer before entering the idle loop
  D_801FD628 = TestIdleCallback;
}

class BootTest : public ::testing::Test {
 protected:
  void SetUp() override {
    MockUltra64Reset();
    s_callback_invocations = 0;
  }
};

// Test 1: Verify the boot main entrypoint initializes the OS and starts the idle thread
TEST_F(BootTest, MainInitializesOSAndSpawnsIdleThread) {
  GetBootTestTarget()->Main();

  // 1. Must initialize OS kernel
  EXPECT_EQ(g_mock_state.os_initialize_calls, 1);

  // 2. Must create idle thread
  ASSERT_EQ(g_mock_state.os_create_thread_calls, 1);
  const MockThreadRecord& idle_rec = g_mock_state.created_threads[0];
  EXPECT_EQ(idle_rec.thread, &D_801241C0);
  EXPECT_EQ(idle_rec.id, 1);
  EXPECT_NE(idle_rec.entry, nullptr);
  EXPECT_EQ(idle_rec.arg, nullptr);
  EXPECT_EQ(idle_rec.sp, D_80126520);
  EXPECT_EQ(idle_rec.pri, 10);

  // 3. Must start idle thread
  ASSERT_EQ(g_mock_state.os_start_thread_calls, 1);
  EXPECT_EQ(g_mock_state.started_threads[0], &D_801241C0);
}

// Test 2: Verify idle thread initializes subsystems, mainproc thread, and executes callback
TEST_F(BootTest, IdleInitializesSubsystemsAndMainproc) {
  // 1. Set dummy callback to ensure idle() resets it to NULL initially
  D_801FD628 = reinterpret_cast<void (*)(void)>(0x12345678);

  // 2. Configure hook to arm the callback right when priority drops to 0
  g_mock_state.on_set_priority_hook = OnSetPriorityHook;

  // 3. Run idle() with escape guard
  if (setjmp(s_idle_escape_buf) == 0) {
    GetBootTestTarget()->Idle(nullptr);
    FAIL() << "Idle() should not return normally (it contains an infinite loop)";
  }

  // 4. Verify subsystems were initialized in exact expected order with expected parameters
  EXPECT_EQ(g_mock_state.func_800FD5B0_calls, 1);

  EXPECT_EQ(g_mock_state.func_800FB140_calls, 1);
  EXPECT_EQ(g_mock_state.func_800FB140_last_arg0, 2);
  EXPECT_EQ(g_mock_state.func_800FB140_last_arg1, 1);

  EXPECT_EQ(g_mock_state.func_80105B00_calls, 1);
  EXPECT_EQ(g_mock_state.func_80105B00_last_arg0, 90);

  // 5. Verify mainproc thread creation (id = 3, pri = 10, stack = D_801C6220)
  ASSERT_EQ(g_mock_state.os_create_thread_calls, 1);
  const MockThreadRecord& mainproc_rec = g_mock_state.created_threads[0];
  EXPECT_EQ(mainproc_rec.thread, &D_80124370);
  EXPECT_EQ(mainproc_rec.id, 3);
  EXPECT_NE(mainproc_rec.entry, nullptr);
  EXPECT_EQ(mainproc_rec.arg, nullptr);
  EXPECT_EQ(mainproc_rec.sp, D_801C6220);
  EXPECT_EQ(mainproc_rec.pri, 10);

  // 6. Verify mainproc thread was started
  ASSERT_EQ(g_mock_state.os_start_thread_calls, 1);
  EXPECT_EQ(g_mock_state.started_threads[0], &D_80124370);

  // 7. Verify idle thread priority dropped to 0
  ASSERT_EQ(g_mock_state.os_set_thread_priority_calls, 1);
  EXPECT_EQ(g_mock_state.priority_changes[0].thread, &D_801241C0);
  EXPECT_EQ(g_mock_state.priority_changes[0].pri, 0);

  // 8. Verify callback was executed
  EXPECT_EQ(s_callback_invocations, 1);
}
