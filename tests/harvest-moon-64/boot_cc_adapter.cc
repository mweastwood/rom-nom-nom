#include "boot_test_interface.h"
#include "src/cc/harvest-moon-64/boot.h"

namespace {

class CcBootAdapter : public IBootTestTarget {
public:
    void Main() override {
        hm64::BootSystem::Main();
    }

    void Idle(void* arg) override {
        hm64::BootSystem::Idle(arg);
    }
};

static CcBootAdapter s_adapter;

} // namespace

IBootTestTarget* GetBootTestTarget() {
    return &s_adapter;
}
