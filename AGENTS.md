# Building NEOTOKYO Rebuild

This document provides instructions for building NEOTOKYO Rebuild (NT;RE) using Visual Studio 2022 and CMake.

## Requirements

*   **Visual Studio 2022 (MSVC v143)**:
    *   Include "Desktop development with C++" workload.
    *   Ensure the following components are selected:
        *   C++ MFC for latest v143 build tools (x86 & x64)
        *   Windows 10/11 SDK
        *   C++ CMake tools for Windows
*   **CMake**: Required for generating the build system.
*   **Ninja** (Optional): Can be used as a build system generator.

## Building with Visual Studio 2022

1.  Open Visual Studio 2022.
2.  Select **File > Open > CMake...**.
3.  Navigate to the `src` directory and select `CMakeLists.txt`.
4.  Visual Studio should detect the CMake configuration.
5.  In the **Solution Explorer**, switch to **CMake Targets View** (usually available via a toggle or right-click menu, if not default).
6.  Select the desired configuration (e.g., `windows-debug`, `windows-release`) from the configuration dropdown.
7.  Build the project using **Build > Build All** or by right-clicking the root CMake target and selecting **Build**.

## Building with CLI (Linux)

This project can be built on Linux using CMake and Ninja.

### Prerequisites

*   **Tools**: `gcc`, `g++`, `cmake`, `ninja-build`.
*   **Git Tags**: The build system requires git tags to be present to generate version information. If you are working on a shallow clone or a fresh checkout without tags, you may need to create a tag (e.g., `git tag v99.0-test`) or fetch tags (`git fetch --tags`).

### Build Steps

1.  Navigate to the `src` directory:
    ```bash
    cd src
    ```
2.  Configure the build using the `linux-debug` preset:
    ```bash
    cmake --preset linux-debug
    ```
    *   If this fails with "Failed to get git tag", create a dummy tag: `git tag v99.0-test`.
3.  Build the project:
    ```bash
    cmake --build --preset linux-debug --parallel $(nproc)
    ```

### Output

The build output (shared libraries) will be placed in `game/neo/bin/linux64`.

## Debugging with Visual Studio 2022

To debug the project, you need to configure the launch settings.

1.  In the **CMake Targets View**, right-click the `client` (or `server`) target.
2.  Select **Add Debug Configuration**. This will create or open a `.json` launch configuration file (usually `launch.vs.json`).
3.  Configure the launch settings similar to the following:

```json
{
  "version": "0.2.1",
  "defaults": {},
  "configurations": [
    {
      "type": "dll",
      "exe": "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Source SDK Base 2013 Multiplayer\\hl2_win64.exe",
      "project": "CMakeLists.txt",
      "projectTarget": "client.dll (game\\client\\client.dll)",
      "name": "client.dll (game\\client\\client.dll)",
      "currentDir": "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Source SDK Base 2013 Multiplayer",
      "args": [
        "-allowdebug",
        "-insecure",
        "-dev",
        "-sw",
        "-game",
        "C:\\PATH\\TO\\REPO_ROOT\\neo\\game\\neo"
      ]
    }
  ]
}
```

*   **exe**: Path to `hl2_win64.exe` in your Source SDK Base 2013 Multiplayer installation.
*   **currentDir**: The directory containing `hl2_win64.exe`.
*   **args**:
    *   `-game`: Path to the `game/neo` directory in this repository.
    *   `-insecure`: Recommended to avoid VAC issues.
    *   `-sw`: Start in windowed mode.
    *   `-dev`: Enable developer mode.

## Additional Resources

*   See `README.md` for more detailed build instructions, including Linux and CLI.
*   See `CONTRIBUTING.md` for detailed contribution guidelines and debugging tips.
