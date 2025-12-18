# Palette's Journal

## 2024-10-27 - Colorblind-Safe HUD Colors
**Learning:** Default red/green indicators are indistinguishable for Deuteranopia/Protanopia users.
**Action:** Always include a shape/icon difference alongside color changes, or use high-contrast blue/orange pairs.

## 2024-10-28 - Text Scaling in VGUI
**Learning:** Hardcoded pixel sizes in VGUI schemes break on high-DPI (4k) displays.
**Action:** Always use "proportional_xpos" and "proportional_float" types for HUD coordinates and sizes.

## 2024-10-29 - Sound Feedback
**Learning:** Visual-only feedback for critical events (low ammo, hit marker) is often missed in chaotic combat.
**Action:** Pair critical visual cues with distinct sound effects or subtle screen pulses.
