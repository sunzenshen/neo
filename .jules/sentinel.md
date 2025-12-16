## 2025-12-16 - Command Injection via System Call
**Vulnerability:** A `system()` call was used to open URLs, constructing the command string by concatenating the URL with a system command (`start` or `xdg-open`). This allowed for command injection if the URL contained shell metacharacters.
**Learning:** Even "helper" functions for opening URLs can be dangerous if they rely on shell execution without proper argument escaping.
**Prevention:** Use safer APIs like `ShellExecute` (or platform equivalents) that handle argument passing securely, or ensure rigorous input sanitization/escaping if shell execution is unavoidable.
