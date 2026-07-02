# AGENTS.md — ESP32-S3-N16R8 CAM

> Firmware project for an ESP32-S3-N16R8 module + OV3660 camera. **Production-ready** with MJPEG streaming, AI detection, RTSP, ONVIF, and responsive web UI.

## Hardware target

| Item | Value | Notes |
|------|-------|-------|
| Module | ESP32-S3-WROOM-1 **N16R8** | N16 = 16 MB Quad Flash · R8 = 8 MB **Octal** PSRAM |
| SoC | ESP32-S3 (Xtensa LX7 dual-core @ 240 MHz) | USB-OTG + USB-Serial/JTAG |
| Camera | **OV3660** (3 MP, 1/5", max QXGA 2048×1536) | NOT the OV2640 from the reference repos — see below |
| USB | USB-Serial/JTAG (enumerates as `/dev/ttyACM0`) | |

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

## Shipped Features

The firmware is production-ready with the following modules and features:

### Core Modules (15 modules)

| Module | Files | Purpose |
|--------|-------|---------|
| main.c | main.c | App entry, boot sequence orchestrator |
| config_manager | config_manager.c/h | NVS-backed config, 16 keys, TYPE_U8/TYPE_I8 |
| camera_driver | camera_driver.c/h | OV3660 init, sensor settings, coordinated reinit |
| frame_broadcaster | frame_broadcaster.c/h | Frame grab task on Core 1, publisher-subscriber pattern |
| mjpeg_streamer | mjpeg_streamer.c/h | HTTP MJPEG streaming via chunked multipart |
| ai_pipeline | ai_pipeline.cpp/h | Face + motion + QR detection, 640×480 buffers, ESP-DL |
| web_server | web_server.c/h | REST API (11 endpoints), SPIFFS static files |
| web_ui | index.html, style.css, app.js, i18n.js | Browser UI, zh/en i18n, light/dark theme |
| wifi_manager | wifi_manager.c/h | WiFi STA mode connection |
| flash_led | flash_led.c/h | GPIO flash LED control |
| at_command | at_command.c/h | Serial AT command interface |
| rtsp_server | rtsp_server.cpp/h | RTSP server with MJPEG-only streaming |
| onvif_service | onvif_service.c/h | ONVIF SOAP service |
| onvif_discovery | onvif_discovery.c/h | ONVIF WS-Discovery protocol |
| status_led | status_led.c/h | GPIO status LED |

### REST API Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/` | Static file serving (SPIFFS) |
| GET | `/stream` | MJPEG live stream |
| GET | `/status` | Device status (WiFi, camera, AI, system) |
| GET | `/config` | Current configuration |
| POST | `/config` | Update configuration (WiFi triggers reboot) |
| GET | `/camera` | Camera configuration |
| POST | `/camera` | Update camera settings (framesize/quality requires reinit) |
| POST | `/ai` | Toggle AI features (live + persist) |
| GET | `/ai/status` | AI detection results (face boxes, motion, QR) |
| POST | `/led` | Flash LED brightness control |
| OPTIONS | `/*` | CORS preflight |

### Web UI Features

- zh/en i18n (80+ keys, auto-detect, persisted)
- Light/dark theme (auto-detect, persisted)
- Settings panels: Camera, AI, Flash LED, Network, Streaming, System
- Real-time AI overlay (face boxes, motion score, QR text)
- Responsive design (mobile 480px → desktop 1280px+)
- Stream reconnect with exponential backoff (1s → 30s)

### AI Features

- **Face detection** (ESP-DL HumanFaceDetect MSRMNP_S8_V1)
- **Motion detection** (frame-difference on grayscale)
- **QR decode** (quirc library)
- Requires VGA resolution (640×480)
- Live toggle via web UI or REST API
- Mutex-protected results for thread-safe polling

### Streaming

- **MJPEG** via HTTP (`/stream`, multiple clients)
- **RTSP** server (MJPEG-only, digest auth)
- **ONVIF** discovery (WS-Discovery) + SOAP service

### Configuration

- NVS namespace: "mibee_cfg"
- 16 keys total
- Supported keys: wifi_ssid, wifi_pass, cam_framesize, cam_quality, ai_face_enable, ai_motion_enable, ai_qr_enable, rtsp_user, rtsp_pass, onvif_enable, cam_brightness, cam_contrast, cam_saturation, cam_sharpness, cam_hmirror, cam_vflip
- Type support: TYPE_U8 (uint8), TYPE_I8 (int8)

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

## Project layout

```
./
├── main/                 # Flat C module layout (one .c/.h pair per subsystem)
│   ├── main.c            # app_main() entry
│   ├── camera_driver.*   # OV3660 init wrapper around esp_camera
│   ├── config_manager.*  # NVS-backed config
│   ├── frame_broadcaster.*  # Frame grab + publisher
│   ├── mjpeg_streamer.*     # HTTP MJPEG streaming
│   ├── ai_pipeline.*        # Face/motion/QR detection (C++)
│   ├── web_server.*         # REST API
│   ├── web_ui/              # HTML/JS/CSS for browser UI
│   │   ├── index.html
│   │   ├── style.css
│   │   ├── app.js
│   │   └── i18n.js
│   ├── wifi_manager.*       # WiFi management
│   ├── flash_led.*          # Flash LED control
│   ├── at_command.*         # Serial AT commands
│   ├── rtsp_server.*        # RTSP server (C++)
│   ├── onvif_service.*      # ONVIF SOAP service
│   ├── onvif_discovery.*    # ONVIF WS-Discovery
│   ├── status_led.*         # Status LED
│   ├── CMakeLists.txt       # Component build
│   └── idf_component.yml    # Component dependencies
├── docs/                  # Documentation
│   ├── architecture.md    # Module map, boot sequence, data flow
│   ├── hardware.md        # Pin map, PSRAM constraints, partitions
│   ├── web-api.md         # REST endpoint reference
│   ├── web-ui.md          # UI features, i18n, theme
│   └── development.md     # Build, flash, CI, contributing
├── partitions.csv         # Custom partition table (16 MB Flash)
├── sdkconfig.defaults     # Hardware pin map + PSRAM + watchdog + lwIP
├── CMakeLists.txt         # project() + spiffs_create_partition_image()
├── AGENTS.md              # This file
├── README.md              # Project README
└── .github/workflows/     # Tag-triggered release CI
    └── release.yml
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

# App-only fast iteration (offset 0x10000 is ota_0)
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x10000 build/mibee_cam.bin

# Clean rebuild
idf.py fullclean && idf.py set-target esp32s3 && idf.py build
```

- **Serial port**: ESP32-S3 default USB-Serial/JTAG enumerates as `/dev/ttyACM0` (not `ttyUSB*`). Confirm with `ls /dev/serial/by-id/`.
- **Baudrate**: 115200 (firmware default).
- **Permission**: user must be in `uucp` (Arch) or `dialout` (Debian/Ubuntu).

## Verified Hardware Attributes

### Board

- **Vendor**: GOOUUU (verified from pin map)
- **Model**: ESP32-S3-N16R8 + OV3660 camera board
- **Pin map**: GOOUUU-specific (NOT XIAO or other vendors)

### OV3660 Camera

- **Sensor ID**: 0x77 (verified via SCCB read)
- **Interface**: SCCB (I2C-like)
- **Default XCLK**: 20 MHz
- **Frame format**: JPEG
- **Frame buffers**: PSRAM-resident, count 2

### Pin Map (GOOUUU board)

| Pin Name | GPIO |
|----------|------|
| PWDN | -1 (not connected) |
| RESET | -1 (not connected) |
| XCLK | 15 |
| SIOD | 4 |
| SIOC | 5 |
| D0-D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |

### USB Mode

- **Mode**: USB-Serial/JTAG (default)
- **Device**: `/dev/ttyACM0`
- **Console**: ESP-IDF monitor via USB-Serial/JTAG

### Partition Plan (16 MB Flash)

| Partition | Offset | Size | Type |
|-----------|--------|------|------|
| nvs | 0x9000 | 24 KB | data/nvs |
| phy_init | 0xf000 | 4 KB | data/phy |
| ota_0 | 0x10000 | 5 MB | app/ota_0 |
| ota_1 | 0x510000 | 5 MB | app/ota_1 |
| otadata | 0xa10000 | 8 KB | data/ota |
| spiffs | 0xa12000 | 512 KB | data/spiffs |

### Peripherals

- **Flash LED**: GPIO 2, 3, or 46 (probed at boot)
- **Status LED**: Configured in `status_led.c`
- **No onboard mic**: Audio features not included
- **No SD slot**: Storage not included

## Scope

### IN Scope

- Camera capture and streaming (MJPEG, RTSP)
- AI detection (face, motion, QR)
- Web UI with full settings control
- ONVIF discovery and SOAP service
- AT command interface
- NVS configuration persistence
- Dual OTA partitions (firmware update ready)
- SPIFFS for web UI assets

### OUT Scope

- Audio recording/playback
- SD card storage
- H.264 video encoding
- 5 GHz WiFi
- ONVIF PTZ control
- NVR/NAS upload
- Cloud integration

## Key Design Decisions

### AI ↔ VGA Coupling

- AI pipeline hardcodes 640×480 buffers
- Non-VGA framesize disabled when any AI feature enabled
- Enforced in both web UI and REST API

### Coordinated Camera Reinit

- Framesize/quality changes stop AI + broadcaster → deinit camera → reinit → restart
- Prevents crashes from accessing invalid camera state

### Live vs. Persisted Settings

- Sensor settings (brightness/contrast/saturation/sharpness/mirror/flip): Applied live
- Framesize/quality: Requires coordinated reinit
- AI features: Applied live + persisted
- WiFi settings: Saved + device reboots

### Publisher-Subscriber Pattern

- `frame_broadcaster` publishes frames
- `mjpeg_streamer` and `ai_pipeline` subscribe
- Allows multiple consumers without frame duplication

## Do NOT

- Copy `partitions.csv` or pin numbers from the reference repos verbatim — flash size and board differ.
- Assume OV3660 behaves like OV2640 for frame size, JPEG quality, or XCLK frequency. Validate empirically.
- Set `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=n` (Octal PSRAM corruption).
- Skip `set-target esp32s3` — without it the build silently targets the wrong chip.
- Commit `sdkconfig`, `managed_components/`, or `build/`.
- Add features outside scope without discussion.

## Verification contract

- Camera change → `idf.py build` clean + flash + `idf.py monitor` shows `camera initialized` and at least one successful frame grab logged.
- `sdkconfig.defaults` change → `idf.py fullclean` then rebuild (stale `sdkconfig` will mask your edits).
- Partition change → re-flash the partition table at `0x8000`, not just the app.
- Web UI change → rebuild spiffs.bin and re-flash partition.
- REST API change → build + flash + test endpoints manually.