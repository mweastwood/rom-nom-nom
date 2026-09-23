#ifndef TESTS_HARVEST_MOON_64_BOOT_TEST_INTERFACE_H
#define TESTS_HARVEST_MOON_64_BOOT_TEST_INTERFACE_H

class IBootTestTarget {
public:
    virtual ~IBootTestTarget() = default;
    virtual void Main() = 0;
    virtual void Idle(void* arg) = 0;
};

// Returns the test target implementation (instantiated by C or CC adapter)
IBootTestTarget* GetBootTestTarget();

#endif // TESTS_HARVEST_MOON_64_BOOT_TEST_INTERFACE_H
