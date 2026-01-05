# AutoWatering Firmware Installation Guide

This guide explains how to prepare a Zephyr development environment, fetch the AutoWatering sources, and build the firmware for the Arduino Nano 33 BLE hardware target.

> **Tip**: These steps track the Zephyr 4.3.0 toolchain used in this repository. If you already maintain a working Zephyr workspace, jump straight to the "Fetch the sources" and "Build" sections.

## 1. Prerequisites

- 64-bit Windows 11/10, Linux, or macOS (Ventura or newer)
- 16 GB RAM recommended when building under virtualization/WSL
- 15 GB free disk space for the Zephyr SDK, workspace, and build outputs
- Git 2.40+ and Python 3.10+
- For flashing hardware: Arduino Nano 33 BLE plus Segger J-Link, CMSIS-DAP debugger, or BOSSA (via USB bootloader)

> **Reference**: The Zephyr [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) provides platform-specific prerequisites. Follow it if any package commands differ on your OS.

## 2. Install the Zephyr Toolchain

### Windows (via WSL2, recommended)

1. Enable WSL and install Ubuntu 22.04:

```powershell
wsl --install -d Ubuntu-22.04
```

1. Inside the Ubuntu shell install the build toolchain and Python helpers:

```bash
sudo apt update
sudo apt install --yes git cmake ninja-build gperf ccache dfu-util device-tree-compiler
sudo apt install --yes gcc gcc-multilib g++ g++-multilib python3 python3-pip python3-venv
sudo apt install --yes wget curl xz-utils file libsdl2-dev libmagic1
python3 -m pip install --user --upgrade west
```

1. Download and install the Zephyr SDK (0.17.0 or later, compatible with Zephyr 4.3.0):

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-x86_64.tar.xz
tar xf zephyr-sdk-0.17.0_linux-x86_64.tar.xz
cd zephyr-sdk-0.17.0
./setup.sh -t all -c
```

### Linux

- Install the same package set as above using your distribution's package manager.
- Install the Zephyr SDK (0.17.0 or later, compatible with Zephyr 4.3.0).
- Install west with `python3 -m pip install --user west`.

### macOS

- Install Homebrew packages: `brew install cmake ninja gperf python@3.11 wget`.
- Install west with `python3 -m pip install --user west`.
- Either build inside a Linux container/WSL for SDK support or use the macOS cross-compilers from the official guide.

## 3. Create the Workspace and Fetch Sources

Choose a workspace directory (examples assume `~/autowatering-workspace`):

```bash
mkdir -p ~/autowatering-workspace
cd ~/autowatering-workspace
west init -m https://github.com/AlexMihai1804/AutoWatering.git --mf west-manifest/west.yml
west update
```

This clones the AutoWatering application alongside the Zephyr upstream tree declared in `west-manifest/west.yml` (Zephyr v4.3.0).

## 4. Install Python Dependencies

From the workspace root run:

```bash
pip3 install --user -r zephyr/scripts/requirements.txt
```

Create and activate a virtual environment first if you prefer an isolated Python setup.

## 5. Build the Firmware

### Arduino Nano 33 BLE (primary target)

```bash
cd AutoWatering
west build -b arduino_nano_33_ble --pristine
```

The build output lives in `build/arduino_nano_33_ble/`. The board overlay `boards/arduino_nano_33_ble.overlay` is pulled in automatically by the CMake configuration; add `boards/usb.overlay` only if you explicitly pass it via `DTC_OVERLAY_FILE`.

## 6. Flash the Hardware

1. Double-tap the reset button on the Arduino Nano 33 BLE to enter bootloader mode (the LED pulses).
2. From the `autowatering/` directory run:

```bash
west flash -r bossac
```

Alternatively, connect via a J-Link or CMSIS-DAP debugger and run `west flash` without the `-r bossac` argument.

Select a specific runner (e.g., `--runner jlink`) when required by your hardware setup.

## 7. Post-Install Checks

- Confirm tool versions:

```bash
west --version
cmake --version
ninja --version
```

- Inspect the serial console to verify the firmware boots and logs as described in `docs/system-architecture.md`.

## 8. Troubleshooting

- Build failures or runtime issues: review `docs/TROUBLESHOOTING.md` and the Zephyr getting-started FAQ.
- Missing SDK headers or toolchain failures usually mean the Zephyr SDK environment is not exported; rerun `<SDK>/setup.sh` and restart the terminal.
- If `west update` fails, verify network/proxy settings and rerun the command.

## 9. Next Steps

- See `docs/README.md` for the rest of the documentation index.
- Consult `docs/ble-api/README.md` before integrating with the BLE service.
- Use `docs/system-architecture.md` when modifying initialization or background tasks.

With the environment prepared, iterate on firmware changes using `west build`, flash updates with `west flash`, and refer to the remaining docs for module-specific detail.
