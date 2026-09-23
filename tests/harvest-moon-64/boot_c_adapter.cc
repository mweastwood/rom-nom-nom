#include "boot_test_interface.h"
#include "mocks/ultra64/ultra64_mock.h"

namespace {

class CBootAdapter : public IBootTestTarget {
 public:
  void Main() override { BootMain(); }

  void Idle(void* arg) override { idle(arg); }
};

static CBootAdapter s_adapter;

}  // namespace

IBootTestTarget* GetBootTestTarget() {
  return &s_adapter;
}
