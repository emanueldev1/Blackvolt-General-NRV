#include "check.h"

int main() {
    for (auto& c : check::registry()) {
        const int before = check::failures();
        c.fn();
        std::printf("%s %s\n", check::failures() == before ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", check::registry().size(), check::failures());
    return check::failures() == 0 ? 0 : 1;
}
