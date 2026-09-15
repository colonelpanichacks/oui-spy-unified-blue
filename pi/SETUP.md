# Visual Scout — Raspberry Pi Setup Guide

## Hardware

| Component | Notes |
|-----------|-------|
| Raspberry Pi 5 (4 GB+) | Primary compute node |
| Google Coral M.2 Accelerator (A+E key) | Edge TPU; fits Pi 5's M.2 HAT slot |
| Pi 5 M.2 HAT+ | Provides both the PCIe M.2 slot for Coral **and** the NVMe SSD slot — you'll need an M.2 HAT that breaks out two slots, or use a USB Coral instead |
| NVMe SSD (M.2 2280, PCIe) | Training data storage — 500 GB+ recommended |
| Power supply | Pi 5 official 27 W USB-C PSU |

> **Note on M.2 slots:** The Pi 5 M.2 HAT provides a single PCIe lane.
> Use a **USB Coral** (`g950-04594-01`) alongside the NVMe SSD if you want
> both TPU and SSD without a dual-slot adapter.

---

## 1. Raspberry Pi OS Setup

```bash
# Install Raspberry Pi OS Bookworm (64-bit, full) via rpi-imager
# Enable SSH in rpi-imager Advanced Options

sudo apt update && sudo apt upgrade -y
sudo apt install -y python3-pip python3-venv git libopencv-dev python3-opencv
```

## 2. Mount the NVMe SSD

```bash
# Format (first time only)
sudo mkfs.ext4 /dev/nvme0n1

sudo mkdir -p /mnt/ssd
echo "/dev/nvme0n1  /mnt/ssd  ext4  defaults,nofail  0  2" | sudo tee -a /etc/fstab
sudo mount -a
sudo chown $USER:$USER /mnt/ssd
mkdir -p /mnt/ssd/training_data
```

## 3. Install Python Dependencies

```bash
cd pi/
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## 4. Install Coral Edge TPU Runtime (optional but recommended)

```bash
echo "deb https://packages.cloud.google.com/apt coral-edgetpu-stable main" \
  | sudo tee /etc/apt/sources.list.d/coral-edgetpu.list
curl https://packages.cloud.google.com/apt/doc/apt-key.gpg | sudo apt-key add -
sudo apt update
sudo apt install -y libedgetpu1-std

# For USB Coral:
sudo apt install -y gasket-dkms

pip install pycoral
```

## 5. Get / Train the Flock Camera Model

### Option A — Use a community-trained model (recommended to start)

```bash
mkdir -p pi/models
# Download latest model from the project releases (placeholder URL):
# wget https://github.com/colonelpanichacks/oui-spy-unified-blue/releases/latest/download/flock_yolov8n.pt \
#   -O pi/models/flock_yolov8n.pt
```

### Option B — Bootstrap with YOLOv8 nano (no Flock-specific training yet)

The pipeline will run inference with the base COCO model until you have
enough labelled training data. Camera-shaped objects will be flagged and
saved even without a custom model — use those saves to build your dataset.

```bash
# YOLOv8n downloads automatically on first run (ultralytics caches it)
```

### Option C — Convert to Edge TPU .tflite for Coral

```bash
# Install Edge TPU compiler
curl https://packages.cloud.google.com/apt/doc/apt-key.gpg | sudo apt-key add -
echo "deb https://packages.cloud.google.com/apt coral-edgetpu-stable main" \
  | sudo tee /etc/apt/sources.list.d/coral-edgetpu.list
sudo apt install -y edgetpu-compiler

# Export from ultralytics (produces flock_yolov8n_full_integer_quant.tflite)
yolo export model=pi/models/flock_yolov8n.pt format=tflite int8

# Compile for Edge TPU
edgetpu_compiler pi/models/flock_yolov8n_full_integer_quant.tflite \
  -o pi/models/
# Output: flock_yolov8n_full_integer_quant_edgetpu.tflite
```

---

## 6. Start the Deflock DB Proxy

This downloads all known Flock Safety camera locations and serves them to
the ESP32 in compact binary format.

```bash
source .venv/bin/activate
python3 pi/deflock_downloader.py
# Server runs on port 5000
# First sync takes ~30–60 seconds
```

Set the ESP32's **Pi Proxy URL** (in the Visual Scout config UI at
http://192.168.4.1) to:
```
http://<pi-ip>:5000/flock_cameras.bin
```

Then press **SYNC DB** on the ESP32's web UI.

---

## 7. Start the Visual Detector

```bash
source .venv/bin/activate

# CPU-only (slower, no Coral)
python3 pi/flock_detector.py --esp32 http://192.168.4.1

# With Coral Edge TPU
python3 pi/flock_detector.py --esp32 http://192.168.4.1 --coral \
  --coral-model pi/models/flock_yolov8n_full_integer_quant_edgetpu.tflite

# With live preview + auto-upload to crowd-source server
python3 pi/flock_detector.py \
  --esp32 http://192.168.4.1 \
  --show \
  --upload-url https://your-upload-server.example/api/detections
```

Training data is saved to `/mnt/ssd/training_data/YYYYMMDD/`.

---

## 8. Run as systemd Services (recommended for driving)

```bash
# Deflock downloader
sudo tee /etc/systemd/system/deflock-proxy.service <<EOF
[Unit]
Description=Deflock DB Proxy
After=network-online.target
Wants=network-online.target

[Service]
User=$USER
WorkingDirectory=$(pwd)
ExecStart=$(pwd)/pi/.venv/bin/python3 $(pwd)/pi/deflock_downloader.py
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

# Visual detector
sudo tee /etc/systemd/system/flock-detector.service <<EOF
[Unit]
Description=Flock Visual Detector
After=network-online.target deflock-proxy.service

[Service]
User=$USER
WorkingDirectory=$(pwd)
ExecStart=$(pwd)/pi/.venv/bin/python3 $(pwd)/pi/flock_detector.py --esp32 http://192.168.4.1
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable deflock-proxy flock-detector
sudo systemctl start  deflock-proxy flock-detector
```

---

## 9. Training Data Upload Format

When `--upload-url` is set, each detection POSTs a `multipart/form-data` request:

| Field  | Type    | Content |
|--------|---------|---------|
| `frame`| file    | Annotated JPEG (annotated frame with detection boxes) |
| `meta` | text    | JSON string with `ts_utc`, `lat`, `lon`, `detections[]`, `model` |

A minimal Flask upload server is straightforward to self-host.

---

## 10. Workflow Summary

```
Drive past area → ESP32 WiFi/BLE sniffs (existing modes)
                → ESP32 GPS alerts on known Flock DB cameras
                → ESP32 MJPEG stream → Pi + Coral TPU detects NEW cameras
                → Detections saved to NVMe SSD with GPS tags
                → Upload to crowd-source server for model retraining
                → Retrained model shared back → better detections for everyone
```
