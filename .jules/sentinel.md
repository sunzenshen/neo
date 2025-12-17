## 2025-12-17 - Command Injection via system()
**Vulnerability:** A `system()` call was found in `OpenURL` function in `src/game/client/neo/ui/neo_ui.cpp` that took a constructed string as input. Even with some validation, passing user-controlled strings to `system()` is dangerous as it executes through the shell, allowing for command injection via shell metacharacters.

**Learning:** Standard library `system()` should generally be avoided in C++ applications, especially when dealing with external input or URLs. Source Engine provides safer abstractions.

**Prevention:** Use `vgui::system()->ShellExecute("open", url)` instead of `system()`. This delegates the URL opening to the OS in a safer manner, avoiding direct shell command construction.
