# FastRobox Level 2 — Smart Classroom (ESP32-S3-CAM + Render + Neon)

This project extends the existing Level 1 FreeRTOS/WDT firmware. It keeps the same verified working RC522 SPI wiring. It is an implementation candidate; deploy and test it against the actual ESP32 board, judges' Wi-Fi and cloud.

## A. Architecture

```
ESP32-S3-CAM  --Wi-Fi-->  Render Node.js HTTPS API  --TLS-->  Neon Postgres
     |                           |
     +-- Local LittleFS page     +-- Password-protected phone dashboard
```

**The camera is not activated in Level 2; it is reserved for later levels.**

## B. Hardware wiring (verify board model first)

| Module | ESP32 GPIO |
|---|---|
| DHT11 DATA | GPIO 1 |
| IR flame DO (active LOW) | GPIO 21 |
| Panic touch wire | GPIO 2 |
| 3.3V-compatible low-current active buzzer + | GPIO 14 |
| RC522 SDA/SS | GPIO 42 |
| RC522 SCK | GPIO 38 |
| RC522 MISO | GPIO 39 |
| RC522 MOSI | GPIO 40 |
| RC522 RST | GPIO 41 |
| Built-in WS2812 RGB | GPIO 48 (board dependent) |
| Built-in BOOT button | GPIO 0 (board dependent) |

Power RC522 and DHT11 and flame sensor from 3.3V; common all GND connections. RC522 is **not 5V tolerant**. Do not use microSD with RFID assigned to GPIO38-40. The RGB LED is onboard; do not wire external RGB channels/resistors.

## C. Deploy the backend and Neon database

1. Create a free Neon Postgres project at https://console.neon.tech/ and copy the PostgreSQL **connection string** from its Connect dialog (must use TLS, usually `sslmode=require`).
2. Create a GitHub repo containing `backend/`, `firmware/`, and this README; **do not commit** `.env` or credentials.
3. Open https://dashboard.render.com/, select **New > Web Service**, choose your repo.
4. Set **Root Directory** to `backend`, **Language** to Node, **Build Command** to `npm install`, **Start Command** to `npm start`.
5. In Render Environment variables enter:
   * `DATABASE_URL` = the Neon connection string.
   * `DEVICE_ID` = `T07-D1` (replace `T07` with your actual team ID).
   * `DEVICE_TOKEN` = a unique long cryptographically random string (>=32 chars).
   * `DASH_USER` = chosen teacher dashboard username.
   * `DASH_PASSWORD` = a distinct long random dashboard password.
6. Start deploy. The backend will create its table and index at startup. Open `https://YOUR-SERVICE.onrender.com/healthz`; expected `{"ok":true}`.
7. Visit `https://YOUR-SERVICE.onrender.com/`; HTTP Basic authentication asks for `DASH_USER` / `DASH_PASSWORD`.

The dashboard is read-only, refreshes summary every 2 s and history/events every 10 s. The cloud marks devices OFFLINE when the most recent successfully received telemetry is >=30 seconds old. Free-hosted services may sleep or incur startup delay; validate timing on the selected plan.

## D. Upload firmware

1. In Arduino IDE, install **Espressif ESP32 board package 3.x**, **DHT sensor library (Adafruit)**, **Adafruit Unified Sensor**, **MFRC522**, and **Adafruit NeoPixel**.
2. Select ESP32S3 Dev Module if appropriate for your physical ESP32-S3-CAM. Set **USB CDC On Boot** based on which connector the board uses (most built-in native-USB ESP32-S3 boards: Enabled), and use `115200` baud. Choose a partition scheme that includes LittleFS/SPIFFS data storage.
3. Open `firmware/FastRobox_Level2.ino`. Set `TEAM_ID` near the top to your actual team ID, same as backend `DEVICE_ID` prefix.
4. Verify pins against the actual board before connecting. Upload. Do not touch GPIO2 until touch calibration finishes.
5. When there is no saved Wi-Fi, the ESP32 creates `SmartRoom-<TeamID>`. **Read its randomly generated password in Serial Monitor**.
6. Join that access point from your phone. If your phone displays “No Internet,” choose to stay connected. Browse to `http://192.168.10.1`.
7. Fill in the **Wi-Fi SSID**, **Wi-Fi password**, **Room ID** (e.g. `R-305`), **HTTPS ingestion URL** (`https://YOUR-SERVICE.onrender.com/api/ingest`), **DEVICE_TOKEN** and a **root CA PEM certificate** for the HTTPS endpoint. This certificate is **public**, not a private key. Obtain a valid CA root certificate matching your host's certificate chain from the certificate authority's official site; check that certificate verification succeeds. Do not enter dummy text or use `setInsecure()`.
8. Submit. Firmware saves these in ESP32 NVS and restarts. Connect phone back to the usual classroom Wi-Fi. The Serial Monitor prints the ESP32 IP and `http://classroom-<teamid>.local` (mDNS discovery depends on your router/phone).
9. Local page shows temperature, humidity, current event state, flame, door, last card UID, RSSI, chip temperature, firmware version, local Asia/Dhaka time and connection status. It refreshes every 2 seconds.
10. Open the hosted dashboard from any internet-connected browser; sign in with teacher credentials.

**Secrets:** Never push the Wi-Fi password, token or Neon connection string to GitHub. The setup page sends them only over a password-protected local device AP and saves them in NVS. For a production installation, encrypted NVS / secure boot / signed firmware and better authenticated provisioning are required.

## E. Verification

* Serial Monitor: `[NET] setup AP`, password, `[NET] connected IP`, mDNS name, `[NET] telemetry HTTP=201`.
* Scan RC522: `[CARD] uid=...` and an EVENT stored under the dashboard.
* Disconnect the upstream hotspot for 20 sec: sensors should continue, reconnect log must appear, uptime should not reset, and telemetry should resume.
* If you provide the wrong Wi-Fi credentials, setup AP should reappear after ~15s. Update SSID/password in the setup page again.
* Press BOOT continuously 5 seconds during normal operation (if your board's BOOT is GPIO0) to clear NVS settings; **do not hold BOOT during power-up/reset** (it may enter bootloader mode). Alternatively Serial command `reset`.
* Check Neon SQL editor: `SELECT device_id,seq,kind,received_at,payload FROM messages ORDER BY received_at DESC LIMIT 20;`
* Replay verification: send the **same** `(deviceId,seq)` twice with proper device token. First valid message -> HTTP 201; replay -> HTTP 409.
* Test backend restart: device should send again once service becomes reachable. If the free service sleeps, cold-start latency can exceed the judging thresholds.
* Watchdog: use `health`, `tasks`. Test `hang` only in controlled conditions; sensor tasks may keep running while loop WDT forces restart.
* Flame experiments must be conducted only by authorized judges/supervisors. A false fire test can use a safe IR source and sensor diagnostics.

## F. Known limitations and what is NOT yet verified

* There is **no hosted deployment** and no attached Neon account or credentials; you must create the Render/Neon services.
* No physical-board test or Arduino compile was performed in this environment. This firmware targets **ESP32 Arduino Core 3.x**. Core or board variations can require edits.
* Basic-auth dashboard is appropriate as a prototype, but public production systems should use proper session-based authentication, CSRF protection, per-device tokens and rate limits.
* Only the last 16 events can be queued in RAM while offline; extreme outages can drop events; telemetry history while offline is not durable. A production system needs persistent, bounded offline queues.
* Events are retried in RAM. A sudden power loss can discard unsent events; NVS sequence numbers remain monotonic.
* The current Level 1 RFID check grants a simulated door unlock for **any readable card**. Do not use it for a real physical lock.
* Local Wi-Fi network scanning blocks the network loop temporarily, though the FreeRTOS sensor tasks continue.
* Setting a correct PEM root CA and using valid NTP time are prerequisites for TLS telemetry.

## G. Why the database rejects duplicates

The `messages` table has `PRIMARY KEY (device_id, seq)`. The server executes `INSERT ... ON CONFLICT DO NOTHING RETURNING seq` and returns HTTP 409 when `rowCount` is 0. The ESP32 reserves 512 future sequence IDs into NVS before using each block; after a restart it skips any previously reserved but unused IDs, preventing reuse.
