# AGENTS.md — ESP32-S3-N16R8 CAM

> Firmware project for an ESP32-S3-N16R8 module + OV3660 camera. **Production-ready** with MJPEG streaming, AI detection, RTSP, ONVIF, and responsive web UI.

## AT command interface (family contract v1.0, 2026-09-04)

统一契约：`docs/at-command.md`（四仓 md5 一致，地位同 api-contract）。核心集：
`AT / AT+HELP / AT+GMR / AT+STATUS / AT+WIFI?|= / AT+IP? / AT+CAMRES?|= / AT+CAMQUAL?|= /
AT+REBOOT / AT+RESTORE`（+能力裁剪项）。红线：**任何读指令不回显密码**；CAMQUAL 边界
10-63（PIT-021）。本板串口 /dev/ttyUSB1（CH340）。CAMRES/CAMQUAL 走 camera_reinit 热重配（AI 全开时锁 VGA）；实现于 main/at_command.c（含 AI/LED/RTSPPASS 扩展）。

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

> **2026-09-02 契约 v1.0 统一化**（权威规范：`docs/api-contract.md`，下表已过时）：
> 新增核心端点 `GET /api/capture`、`GET /api/scan`、`POST /api/reset`、`POST /api/reboot`、
> `POST /api/time`、`GET /api/auth`、`GET /metrics`、`GET /api/led`；
> `POST /api/camera` framesize 合法域收窄为 0-15（与广播的 `supported_resolutions` 一致）；
> capabilities 增加 `api_version`/`wifi_scan`；status 字段对齐契约
> （`camera_resolution`→`resolution`、`mjpeg_clients`→`stream_clients`，新增 `camera`/`device_name`/
> `firmware_version`/`wifi_state`/`min_heap`/`stream_clients_max`）；config 新增 `device_name` 键。
> **RTSP 鉴权已启用**：vendored `components/espp__rtsp`（自 seeed 复制，含 digest 补丁）+
> 直接依赖 `espp/base_component|socket|task`（idf_component.yml 已改，勿再加 espp/rtsp）。
> ONVIF GetSnapshotUri 已指向 `:80/api/capture`。
>
> **契约 v1.1（2026-09-02）**：移植 seeed `ota_updater`（`/api/ota`、`/api/ota/info`、
> `/api/ota/upload`、`/api/ota/spiffs`），`ota:true`；统一默认密码（真实值仅存本地 sdkconfig `CONFIG_MIBEE_CAM_DEFAULT_WEB_PASSWORD`，严禁入库）、拒绝 <6 位密码；
> 修复首次设密未持久化 bug（`known_keys` 白名单曾漏 `web_password`）；api_version=1.1。

All business endpoints use the `/api/` prefix. Returns JSON envelope `{"ok":true,"data":...}` on success, `{"ok":false,"error":"..."}` on failure.

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/status` | open | Device status (WiFi, camera, AI, system) |
| GET | `/api/config` | open | Current configuration (passwords masked) |
| POST | `/api/config` | write | Partial config update (WiFi triggers reboot) |
| GET | `/api/camera` | open | Camera configuration |
| POST | `/api/camera` | write | Update camera settings (framesize/quality requires reinit) |
| GET | `/api/capabilities` | open | Board capability flags (12 booleans) |
| GET | `/api/scan` | open | WiFi AP scan |
| POST | `/api/ai` | write | Toggle AI features (live + persist) |
| GET | `/api/ai/status` | open | AI detection results (face boxes, motion, QR) |
| POST | `/api/led` | write | Flash LED brightness control |
| GET | `/api/led` | open | Flash LED state |
| OPTIONS | `/*` | — | CORS preflight (204 No Content) |

**Auth:** `X-Password` header for write operations. When `web_password` is empty (first boot), all writes return 401 `SET_PASSWORD_FIRST` except `POST /api/config` with a `web_password` field (first-time setup).

**MJPEG stream:** Separate TCP server on port `:81` (independent of main web server on port 80).

**Exempt paths:** `/onvif/*` (SOAP) are not under `/api/`.

### Web UI Features

> **2026-09-02 UI v3 "Honey"**（与 seeed/luatos md5 一致的单一源）：蜂蜜琥珀主题、暗色优先、
> 视频主舞台 + 玻璃控制条 + 指标 chips + 分段式控制台、鉴权抽屉（401 自动唤起并重试）、
> 模态确认、按钮忙态、断流骨架屏。设计令牌集中在 style.css 顶部，规范以 seeed 仓为准。
> 下方旧描述的组件清单已部分过时。

Single-page application served from SPIFFS. Four files:
- `index.html` — page structure
- `app.js` — logic and API calls
- `style.css` — light/dark theme styles
- `i18n.js` — zh/en bilingual translations (auto-detect, persisted in localStorage)

Controls are shown or hidden based on `GET /api/capabilities`. The SPA baseline originates from this board and is ported to all boards.

- Settings panels: Camera, AI, Flash LED, Network, Streaming, System
- Real-time AI overlay (face boxes, motion score, QR text)
- Responsive design (mobile 480px → desktop 1280px+)
- Stream reconnect with exponential backoff (1s → 30s)

### Capabilities

This board returns the following from `GET /api/capabilities`:

| Capability | Supported |
|------------|-----------|
| ai | ✅ |
| sd | ❌ |
| audio | ❌ |
| ota | ❌ |
| mic | ❌ |
| flash_led | ✅ |
| recording | ❌ |
| timelapse | ❌ |
| onvif | ✅ |
| rtsp | ✅ |
| websocket | ❌ |
| mdns | ✅ |

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

# Flash full image  — NOTE flashing policy (root AGENTS.md, 2026-09-04):
# Web OTA is the default delivery path once /api/ota/* merges (currently WIP,
# capability ota:false → USB below is the temporary exception for this repo).
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
- **led_strip patch**: `patches/espressif__led_strip/` fixes a compile error (led_strip 2.5.5
  uses `MALLOC_CAP_*` without including `esp_heap_caps.h` under IDF v6.0.1). The root
  `CMakeLists.txt` copies it over `managed_components/` at configure time — source-only,
  so fresh clones and CI get it automatically. Never edit files inside `managed_components/`
  directly; edit the copy under `patches/`. If `idf.py fullclean` ever errors with a
  managed-components hash mismatch, `rm -rf managed_components build` and rebuild.

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

## Camera limits measured (2026-09-04, on-board) — VGA-only module

**实测**（PIT-021 流程：web 热重配 + 冷启动双路径 + capture 计时）：
- VGA(10)：26fps 广播 / 0.33s capture / 零故障 —— **板级上限，已全链路锁定**
- SVGA(11)/XGA(12)：热重配后 capture 死（0B，间或出一帧）
- HD(13)+：`cam_hal: FB-OVF` 风暴、httpd 楔死；**冷启动存 HD 配置时传感器输出仍是
  VGA**（配置与实际脱节——GET 谎报 resolution 的隐患源）
落地：`CAMERA_RES_BOARD_MAX=10`（camera_driver.h）+ `camera_get_effective_max_res()`；
supported_resolutions/POST/AT+CAMRES/camera_init/camera_reinit/NVS 加载全部收敛 VGA。
推流仅 ~0.4fps：本板 ch11 HT40 弱态网络所致（TCP 窗口已提至家族值 49152/32768 无感、
AMPDU 重开实验无增益且伴一次失联——已回退 =n，调优候选是挪信道/关 HT40）。
NVS 观察项：连续 AT 改 AI 键后出现 `Failed to write NVS key 'ai_motion_enable'`（运行时生效、持久化失败）——待查 NVS 页空间。

## Camera quality bounds (2026-09-04, applied)

- `CAMERA_QUALITY_MIN/MAX = 10/63`（camera_driver.h，驱动不变量：esp32-camera
  JPEG fb 按 w*h/5 分配，q<10 复杂场景超预算截帧，PITFALLS PIT-021）。POST
  /api/camera 与 POST /api/config（白名单键）越界 400；camera init/reinit 钳制；
  GET /api/camera 新增 `quality_min/quality_max`（SPA 滑杆钳制，四仓 app.js 已同步）。
- 分辨率上限见上节：**VGA-only**（同日上板实测后锁定）。
- 本仓树上有未完成的 OTA 移植 WIP（web_server.c 引用未跟踪的 ota_updater.c/h），
  以上改动未提交，随 OTA 收尾会话一并处理（PIT-018 纪律）。

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

## 2026-09-04 上午：NVS 键名红线 + AI/VGA 污染链（PIT-022）
- **NVS 键 ≤15 字符**：`ai_motion_enable`(16) 曾令 `config_save()` 整体失败（遇错即返回），
  其后所有键永不落盘——"AT 关 AI 重启复活"即此。键已改 `ai_motion_en`（JSON 字段名不变）。
  at_command.c / web_server.c 写键的字符串必须与 config_manager.c `s_keys[]` 完全一致，
  config_set 未知键现在会打 WARN。
- **AI 与 VGA 强耦合是事实上的默认态**：AI 任一开启 → 加载钳制强制 VGA + POST 非 VGA 被拒。
  之前"本模组仅 VGA"的结论被"保存失败→AI 复活→强制 VGA"污染（PIT-021/022），
  分辨率上限复测中（CAMERA_RES_BOARD_MAX 临时 15，测毕定稿）。
- 帧尺寸校验改区间（`val > max` 拒绝），不再是单值锁定；AT+INFO 现在打印 `AI: face=.. motion=.. qr=..`。
- **RTSP 会话创建包 try/catch**（PIT-025）：线程耗尽抛 system_error 曾整机 abort（rst:0xc）。
- uptime 改 `esp_timer_get_time()`（64 位，tick 回绕免疫）。
- 统一 logo favicon.svg（四仓同 md5，PIT-026 的 reconfigure 纪律适用）。

## 2026-09-04 下午：双网络支持 + 分辨率上限双网复核

- **本板此前是四仓唯一单 WiFi**（主网弱态即失联无路可退）。已加：
  `wifi_ssid_2/wifi_pass_2`（NVS 键 ≤15 字符红线遵守）+ `AT+WIFI2=ssid,pass`
  （查询脱敏；`ssid,` 空串清除）+ 三层择优/转移：
  ① 开机双网快扫 RSSI 择优（强 ≥8dB 胜出，否则沿用 NVS `wifi_pref/last_net` 上次好网）；
  ② 关联后 12s 无 IP（DHCP 盲区）直接切网；③ 运行期连败 2 次切网、切换计数
  ≥6 防乒乓后转 AP 兜底。`/api/status` 新增 `wifi_net`/`current_ssid`。
  ⚠ 开机择优只在启动时——运行期"弱而不断"不迁移（无 roaming），需要时重启板子即可重选。
- **分辨率上限双网复核**：GT（主网）与 MiBeeAP2（备用网）上 VGA/SVGA 正常、
  XGA 冷启动采集死**完全一致**——上限 SVGA 与网络无关，维持。
  **方法论纠正**：推流 delivered fps（0.5-0.8fps）是"链路 RTT/丢包 + NVR 双路订阅"
  的投递侧指标，同期板端采集 25-27fps（fbroadcast 日志）——**分辨率上限判定只看
  采集侧**（fb_get 是否出帧 + JPEG SOF 实测尺寸），勿用投递 fps 做依据。
- 实测 MiBeeAP2 板位 RTT 仍 ~50-220ms——.119 位置的射频环境两网都一般，
  网络调优（挪信道/关 HT40）仍是独立课题。

## 2026-09-04 分辨率三层上限（家族统一）+ 传感器身份纠偏

- **三层化**：`camera_get_effective_max_res() = min(sensor, board, memory)`
  （camera_driver.c，细节见 PITFALLS PIT-021 附录）。sensor 层查组件能力表
  （`esp_camera_sensor_get_info().max_size`，OV3660→QXGA=19）；board 层
  `CAMERA_RES_BOARD_MAX=11/SVGA`（双网实测，不变）；memory 层 PSRAM fb 预算
  （512K floor，只能收紧）。`GET /api/camera` 下发 `res_cap_source`；
  supported_resolutions 由静态表改为按 effective 循环生成；AT+CAMRES 同步。
  本板满配不变（10-11、source=board）。
- **传感器身份纠偏（顺带修复）**：本仓曾把 OV3660 的 PID 误记为 **0x77**
  （0x77 其实是 OV7725；组件对 OV3660 只认 **0x3660**——`ov3660_detect` 读
  0x300A/0x300B 比对）。后果：`camera_sensor_name()` 的手抄映射对实戴传感器
  返回 "unknown"，camera_init 的 "PID=0x77 confirmed" 分支永不命中。
  已改查组件表取名/确认；camera_init 不再硬拒 OV2640（换传感器由 sensor
  层自动收缩候选，符合家族"换板/换传感器自适应"方向）。硬件表中"Sensor ID:
  0x77"为误记，勿再引用。

## 2026-09-04 晚 API parity 补齐（契约 §4 违约修复，已烧录验证）

四板实测矩阵 × SPA 字段消费交叉核对后，本板补齐三个核心 status 字段
（用户报障"119 无信号显示"的根因即前两个缺失）：
- **`wifi_rssi`/`wifi_channel`**：`wifi_manager_get_rssi()/get_channel()`
  （`esp_wifi_sta_get_ap_info`，未连接返回 0）→ SPA 统计条信号芯片 +
  WiFi 页当前连接行。实测 -47dBm/ch2 正常下发。
- **`chip_temp`**：S3 片内温度传感器（`esp_driver_tsens`，CMake REQUIRES 已加），
  量程 (50,125)→(20,100)→(-10,80) 依次回退（跨档驱动拒绝，同 seeed 教训），
  **惰性安装于 web_server**（首次 /api/status 时装，实测 60°C）。
- ai-thinker 同轮补 `free_psram`（其板 4MB PSRAM）。剩余差异均为硬件/功能正当
  （经典 ESP32 无温度传感器、luatos 无 PSRAM、SD/录像/传感器微调随能力省略）。
- **timezone 不补**：本板无 NTP/时区应用路径（仅 /api/time 手动设 epoch），
  加字段即死字段——等有真实时区消费再随功能加。

## 2026-09-04 深夜：httpd 自愈误杀修复（2-4 分钟重启循环根因，已烧录验证）

**症状**：.119 每 2-4.5 分钟 `rst:0xc`（无 panic），串口签名
`httpd_accept_conn: error in accept (23)` → `httpd :80 probe failed (2/2)` →
`unresponsive for 120s — rebooting`。**注意 socket 上限不是原因**——本仓
`LWIP_MAX_SOCKETS=16`/`TCP_MSL=15000` 早已配置（与 ai-thinker/luatos 同款），
循环依旧。真因：**探针自愈无资源分类**——NVR 多路订阅挤占 lwIP 池时，探针自己
socket()/connect() 拿不到资源（EMFILE/ENOBUFS）也被计为"httpd 死"，2/2 即重启。
**修复（PIT-002 家族教训收尾，方案同 seeed/ai-thinker 两仓验证配方）**：
① 探针端：socket()/connect() 资源类失败打 WARN 并返回"不计数"，只有
"TCP 连上但应用层无响应"才计失败；② 调用端：WiFi 未连接时不计数。
**上板实证**：修复前 20:37-20:49 五连重启（2-4.5 分钟间隔）；修复后仅
20:53 一次**真卡死**正确自愈（探针 TCP 连上但 120s 无应用响应——这正是
该重启的场景），其后 16+ 分钟零重启、uptime 连续爬升。
