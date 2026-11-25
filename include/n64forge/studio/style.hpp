#pragma once
#include <QPalette>
#include <QString>

namespace n64forge::studio {

// Explicit dark palette so macOS system styling never leaves black text on a
// charcoal surface. Every text role is off-white or cool slate.
[[nodiscard]] QPalette darkPalette();

// Global stylesheet. It refines the palette (borders, focus, item views) but
// never changes the single-source Settings/layout structure.
[[nodiscard]] QString retroStyleSheet();

} // namespace n64forge::studio
