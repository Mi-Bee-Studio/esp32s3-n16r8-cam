# AGENTS.md — ESP32-S3-N16R8 CAM

> Firmware project for an ESP32-S3-N16R8 module + OV3660 camera. **Currently empty — bootstrapping phase.** Goal: gather this board's hardware attributes first, then design features against them.

## Hardware target

| Item | Value | Notes |
|------|-------|-------|
| Module | ESP32-S3-WROOM-1 **N16R8** | N16 = 16 MB Quad Flash · R8 = 8 MB **Octal** PSRAM |
| SoC | ESP32-S3 (Xtensa LX7 dual-core @ 240 MHz) | USB-OTG + USB-Serial/JTAG |
| Camera | **OV3660** (3 MP, 1/5", max QXGA 2048×1536) | NOT the OV2640 from the reference repos — see below |
| USB | connected (which mode is TBD — see Open questions) | |

### Why N16R8 changes the design vs the reference repos

- **16 MB Flash** (vs 8 MB on `seeed-esp32s3-cam`, 4 MB on `ai-thinker-esp32-cam`) → partition table can hold two **larger** OTA slots, bigger SPIFFS/NVS, or a factory image. Re-plan `partitions.csv` from scratch; do **not** copy the 8 MB layout.
- **8 MB Octal PSRAM** → same module family as the XIAO board, so the S3-Octal gotchas carry over (see *Octal PSRAM* below).
- **OV3660 ≠ OV2640**:
  - Higher max resolution (QXGA). Defaults copied from OV2640 (typically UXGA) will misallocate frame buffers.
  - esp32-camera exposes it via the same `esp_camera_*` API; `PIXFORMAT_JPEG` still works, but JPEG engine quality/rate differs.
  - Sensor ID is `0x77` (OV2640 is `0x26`/`0x42`) — useful for `esp_camera_sensor_get()` sanity checks.

## Toolchain

| Tool | Version | Path / Notes |
|------|---------|--------------|
| ESP-IDF | **v6.0.1 (pinned)** | `~/.espressif/v6.0.1/esp-idf/` |
| Component: `espressif/esp32-camera` | `^2.1.6` | Proven across both reference repos, supports OV3660 |
| Target | `esp32s3` | Set once per build dir |

Activation (every new shell):
```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
```

## Reference repos (carry conventions forward, do NOT copy pin tables)

| Repo | What to steal | What NOT to copy |
|------|---------------|------------------|
| https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam | S3 + Octal PSRAM sdkconfig patterns, `main/` flat module layout, dual-OTA partitioning, SPIFFS web UI embedding, build/flash/release workflow | XIAO ESP32-S3 Sense **camera pin map** (different board) |
| https://github.com/Mi-Bee-Studio/ai-thinker-esp32-cam | Simpler motion-detect/MJPEG core, ESP-IDF v6.0.1 CI badge convention | Everything ESP32-specific (plain ESP32 has no Octal PSRAM, DMA differs, IRAM pressure tuning is ESP32-only) |

The `seeed-esp32s3-cam` repo has a detailed `AGENTS.md` worth reading for S3 patterns; this file is its sibling, scoped to N16R8 + OV3660.

## Required reading before writing code

1. **This board's schematic** — the camera pin map (PWDN, RESET, XCLK, SIOD, SIOC, D0–D7, VSYNC, HREF, PCLK) and any onboard mic/LED/SD wiring. "ESP32-S3-N16R8 CAM" is a *module* designation, not a board; many vendors (Freenove, Sipeed, LilyGO, generic) ship N16R8+OV3660 boards with **different GPIO assignments**. Identify the exact board first. Pin mapping is the #1 source of "camera init failed" bugs.
2. Octal PSRAM constraint below.
3. The two reference repos' `sdkconfig.defaults` to see which knobs actually mattered.

## Octal PSRAM (non-negotiable on this module)

Copied verbatim from the working `seeed-esp32s3-cam` config — verified necessary on S3 + 8 MB Octal:

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y            # Octal, NOT QUAD — R8 module
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
# 64B cache line is MANDATORY for Octal DDR mode. 32B causes silent data corruption.
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```

Frame buffers for OV3660 at any meaningful resolution **must** live in PSRAM (`CAMERA_FB_IN_PSRAM`); internal DRAM is far too small.

## Project layout to adopt (matches both reference repos)

```
./
├── main/                 # Flat C module layout (one .c/.h pair per subsystem)
│   ├── main.c            # app_main() entry
│   ├── camera_driver.*   # OV3660 init wrapper around esp_camera
│   ├── idf_component.yml # espressif/esp32-camera dependency
│   └── web_ui/           # HTML/JS embedded into SPIFFS via spiffs_create_partition_image
├── partitions.csv        # Custom — re-plan for 16 MB Flash
├── sdkconfig.defaults    # Hardware pin map + PSRAM + watchdog + lwIP lives HERE
├── CMakeLists.txt        # project() + spiffs_create_partition_image()
└── .github/workflows/    # Tag-triggered release CI in espressif/idf:v6.0 container
```

Conventions:
- `sdkconfig.defaults` is the **single source of truth** for hardware pin config. Do not put pin numbers in `Kconfig.projbuild` menu items — both reference repos keep them in `sdkconfig.defaults` and that's where operators look.
- Project name in `CMakeLists.txt`: `mibee_cam` (consistent MiBee Cam branding across the family).
- `sdkconfig` (the generated one) and `managed_components/` are gitignored — only `sdkconfig.defaults` is committed.

## Build / flash

```bash
# First time in a fresh shell
source ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32s3

# Build
idf.py build

# Flash full image
idf.py -p /dev/ttyACM0 flash

# App-only fast iteration (offset 0x10000 is typical but confirm against partitions.csv)
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x10000 build/mibee_cam.bin

# Clean rebuild
idf.py fullclean && idf.py set-target esp32s3 && idf.py build
```

- **Serial port**: ESP32-S3 default USB-Serial/JTAG enumerates as `/dev/ttyACM0` (not `ttyUSB*`). Confirm with `ls /dev/serial/by-id/`.
- **Baudrate**: 115200 (firmware default).
- **Permission**: user must be in `uucp` (Arch) or `dialout` (Debian/Ubuntu).

## Phase 1 — hardware discovery (do this BEFORE feature work)

The user's explicit instruction: gather this device's hardware attributes first, then design features. Concrete discovery steps:

1. **Identify the exact board** (vendor + model) and locate its schematic → record the OV3660 pin map into `sdkconfig.defaults` as `CONFIG_CAMERA_PIN_*`.
2. **Confirm USB mode**: USB-Serial/JTAG (default, `ttyACM0`, no extra config) vs USB-OTG/CDC (needs `CONFIG_ESP_CONSOLE_USB_CDC` and a different driver). Watch `idf.py monitor` output to see which interface the bootloader logs to.
3. **Probe the OV3660** over SCCB: read sensor ID via `esp_camera_sensor_get()->id.PID` — expect `0x77`. If it reads `0x26`/`0x42` you actually have an OV2640.
4. **Capture one frame** at `FRAMESIZE_VGA`, `PIXFORMAT_JPEG`, `fb_count=2`, `CAMERA_FB_IN_PSRAM` — the minimal smoke test.
5. **Record findings** in this file (replace *Hardware target* + *Open questions* with verified values) before adding any feature modules.

Do not start MJPEG streaming, motion detection, NAS upload, OTA, or web UI until step 4 passes.

## Open questions (resolve during Phase 1)

- Exact board vendor/model → camera + peripheral pin map.
- USB mode (Serial/JTAG vs CDC) and which console the user wants for logs.
- Whether the board has an onboard PDM/I2S mic, SD slot, or status LED (drives whether audio/storage/LED modules are needed at all). The reference repos have all of these; this board may have none.
- Partition plan for 16 MB Flash (single factory + OTA, or dual OTA + large SPIFFS).

## Do NOT

- Copy `partitions.csv` or pin numbers from the reference repos verbatim — flash size and board differ.
- Assume OV3660 behaves like OV2640 for frame size, JPEG quality, or XCLK frequency. Validate empirically.
- Set `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=n` (Octal PSRAM corruption).
- Skip `set-target esp32s3` — without it the build silently targets the wrong chip.
- Commit `sdkconfig`, `managed_components/`, or `build/`.

## Verification contract (when code starts landing)

- Camera change → `idf.py build` clean + flash + `idf.py monitor` shows `camera initialized` and at least one successful frame grab logged.
- `sdkconfig.defaults` change → `idf.py fullclean` then rebuild (stale `sdkconfig` will mask your edits).
- Partition change → re-flash the partition table at `0x8000`, not just the app.
