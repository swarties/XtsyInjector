# Xtsy Injector

Xtsy Injector is a Linux injector launcher for Minecraft Java Edition.

It scans running Minecraft processes, lets you pick a payload profile, and
injects the embedded native bootstrap into the selected process.

## X11 Support

Yes. The launcher supports X11 systems. It also supports Wayland and falls back
automatically when needed.

## Build Requirements

- CMake 3.16+
- C++17 compiler
- `pkg-config`
- SDL2 development headers
- OpenGL development headers
- `libcap` development headers
- `xxd`

## Build

From repo root:

```bash
cmake -S . -B cmake-build
cmake --build cmake-build -j4
```

Binary output:

```text
build/mc_injector
```

## Run

Start Minecraft first, then run:

```bash
./build/mc_injector
```

## Payload Profiles

- `Xtsy`: uses your configured Xtsy jar path
- `Lion`: downloads LionClient injectable jar from GitHub
- `Custom`: disabled by default, supports custom jar + entry class + entry method

## Settings

Use the Settings section in the launcher to configure:

- Xtsy jar path
- Custom payload enabled/disabled
- Custom profile name
- Custom jar path
- Custom entry class
- Custom entry method

If required paths are missing, injection is blocked and the launcher shows an
error prompting you to set the payload path in Settings.

## ptrace Setup

The injector uses `ptrace`, so your system must allow attach to the Minecraft
process.

Check current Yama value:

```bash
cat /proc/sys/kernel/yama/ptrace_scope
```

Temporarily allow ptrace (until reboot):

```bash
echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope
```

Persist across reboots:

```bash
echo 'kernel.yama.ptrace_scope = 0' | sudo tee /etc/sysctl.d/10-ptrace.conf
sudo sysctl --system
```

Alternative to changing `ptrace_scope`:

```bash
sudo setcap cap_sys_ptrace+ep ./build/mc_injector
```

## LionClient Note

Lion profile target:

```text
https://github.com/LionClientINC/LionInjectable
```
