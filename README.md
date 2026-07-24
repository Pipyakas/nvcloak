# NVCloak

NVCloak is a process-local Windows shim that hides NVIDIA display adapters from a game while leaving the NVIDIA GPU enabled system-wide for CUDA and other applications.

It was created to work around mixed-vendor rendering problems where a game renders correctly only when the NVIDIA device is disabled in Device Manager.

## What it intercepts

- DXGI adapter enumeration
- `EnumDisplayDevicesW/A`
- SetupAPI display-device enumeration
- Configuration Manager device IDs
- NVAPI loading and symbol resolution
- `D3D11CreateDevice`
- `D3D11CreateDeviceAndSwapChain`

Default or NVIDIA D3D11 device requests are redirected to the first non-software AMD adapter. NVIDIA PCI vendor ID `0x10DE` is hidden; AMD vendor ID `0x1002` is preferred.

The shim affects only the process that loads it. It does not disable hardware, modify drivers, or prevent other processes from using NVIDIA CUDA.

## Build

Requirements:

- Windows x64
- Visual Studio 2022 C++ build tools
- Windows SDK

Run:

```bat
build.bat
```

All outputs are created under `bin\`:

- `dxgi.dll`
- `version.dll`
- `winmm.dll`
- `dbghelp.dll`
- `d3d12.dll`
- `wininet.dll`
- `winhttp.dll`
- `nvcloak.asi`

The forwarding export tables and assembly stubs are generated from the build machine's System32 DLLs in a temporary `bin\obj` directory. That directory is removed automatically after a successful build. No generated source is stored in the repository.

`build.bat` only builds files; it never installs or copies them into a game directory.

## Usage

Choose exactly one NVCloak proxy filename that the target game loads early, then manually copy that file from `bin\` beside the game executable.

Do not rename one proxy DLL to another name. Each proxy forwards a different Windows DLL export table, and using the wrong exports can prevent the game from launching.

The ASI build requires an existing ASI loader and may initialize too late for some games.

## Proxy structure

All variants share the filtering implementation in:

```text
src\nvcloak.c
```

`src\build_forwarding_proxy.ps1` generates the proxy-specific `.def`, C, and assembly forwarding files at build time. Only the small hand-maintained DXGI and Version proxy definitions are stored in `src\`.

## Diagnostics

When loaded, the shim writes diagnostic events to:

```text
nvcloak.log
```

The log is created beside the loaded proxy DLL.

## Important notes

- Use only one NVCloak proxy per game unless you know the game’s loader behavior.
- Do not use it while capturing with RenderDoc; both intercept DXGI/D3D11.
- Keep a backup or use the game launcher’s file verification if testing unfamiliar proxy names.
