# MiBee Cam

ESP32-S3-N16R8 + OV3660 camera firmware with MJPEG streaming, AI detection, RTSP, ONVIF, and responsive web UI.

## Hardware

- **Module**: ESP32-S3-WROOM-1 **N16R8** (16 MB Quad Flash, 8 MB Octal PSRAM)
- **Camera**: **OV3660** (3 MP, 1/5" sensor, max QXGA 2048×1536)
- **USB**: USB-Serial/JTAG (enumerates as `/dev/ttyACM0`)

## Features

- 📷 **MJPEG streaming** — real-time video via HTTP
- 🤖 **AI detection** — face, motion, QR code with live web overlay
- 📡 **RTSP server** — MJPEG-only streaming with digest auth
- 🔍 **ONVIF discovery** — network camera discovery protocol
- 💡 **Web UI** — zh/en i18n, light/dark theme, full settings control
- ⌨️ **AT commands** — serial configuration interface
- 🚦 **OTA-ready** — dual OTA partition layout

## Quick Start

```bash
# Install ESP-IDF v6.0.1
git clone --recursive https://github.com/espressif/esp-idf.git ~/.espressif/esp-idf
cd ~/.espressif/esp-idf
git checkout v6.0.1
git submodule update --init --recursive
./install.sh esp32s3

# Activate ESP-IDF (every new shell)
source ~/.espressif/esp-idf/export.sh

# Clone and build
git clone https://github.com/Mi-Bee-Studio/esp32s3-n16r8-cam.git
cd esp32s3-n16r8-cam
idf.py set-target esp32s3
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash monitor

# Open http://<device-ip>/ in a browser
```

## Documentation

- [Architecture](docs/architecture.md) — module map, boot sequence, data flow
- [Hardware](docs/hardware.md) — pin map, PSRAM constraints, partition plan
- [Web API](docs/web-api.md) — REST endpoint reference
- [Web UI](docs/web-ui.md) — UI features, i18n, theme, settings
- [Development](docs/development.md) — build, flash, CI, contributing

## Project Status

This is a production-ready firmware with:
- ✅ Working camera (OV3660)
- ✅ MJPEG streaming
- ✅ AI pipeline (face, motion, QR)
- ✅ Web UI (zh/en, light/dark)
- ✅ RTSP server
- ✅ ONVIF discovery
- ✅ AT command interface
- ✅ NVS configuration
- ✅ Dual OTA partitions

## Known Limitations

- AI requires VGA resolution (640×480)
- RTSP is MJPEG-only (no H.264)
- WiFi is 2.4 GHz only (no 5 GHz)
- Web UI requires modern browser (ES6+)

## License

GPL-3.0-or-later

## Contributing

Contributions are welcome! Please see [Development](docs/development.md) for guidelines.

## Support

- Issues: [GitHub Issues](https://github.com/Mi-Bee-Studio/esp32s3-n16r8-cam/issues)
- Documentation: [docs/](docs/)
- AGENTS.md: Agent instructions for AI-assisted development