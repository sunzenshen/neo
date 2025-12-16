## 2024-05-22 - Command Injection via system()
**Vulnerability:** Usage of `system()` with constructed strings to open URLs in `neo_ui.cpp`.
**Learning:** `system()` invokes the shell, allowing command injection if input is not sanitized. Standard `ShellExecute` abstracts this safely.
**Prevention:** Use `vgui::system()->ShellExecute("open", url)` for opening external links. Avoid `system()` entirely.
