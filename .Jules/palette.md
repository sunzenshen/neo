## 2024-12-16 - [Low Ammo Visibility]
**Learning:** Using CPanelAnimationVar allows exposing UI colors to resource files, but modifying them in code requires careful handling to ensure both numeric and graphical elements update correctly without overriding the animation system permanently (by using a local variable).
**Action:** When adding state-dependent colors to Source engine VGUI, always check if the element is drawn in multiple passes (e.g. text vs icons) and apply the color change to the graphics context before each relevant draw call, or ensure the context state persists.
