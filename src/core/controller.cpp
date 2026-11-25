#include "n64forge/input/controller.hpp"

namespace n64forge::input {

void Controller::setButton(N64Button button, bool pressed) {
    const auto bit = static_cast<u16>(button);
    if (pressed) {
        state_.buttons |= bit;
    } else {
        state_.buttons &= ~bit;
    }
}

} // namespace n64forge::input
