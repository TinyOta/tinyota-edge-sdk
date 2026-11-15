# TinyOTA Device Agent — Python Reference Implementation

Python reference agent for integrating devices with the [TinyOTA](https://tinyota.xyz) hosted OTA platform.

> **Embedded C developers**: see the C SDK in the root of this repository (`include/`, `src/`).  
> This Python agent is provided as a quick-start reference and for simulation/testing on POSIX systems.

---

## Features

- Full 7-state FSM: `IDLE → CHECKING → DOWNLOADING → PATCHING → VERIFYING → SWITCHING → REBOOT`
- Hybrid update check — server decides DELTA or FULL based on device RAM/storage
- Range-request resumable download (survives network interruptions mid-transfer)
- SHA-256 and ECDSA P-256 signature verification before installing (needs `FOTA_PUBLIC_KEY_PEM`)
- Real installs through `FOTA_INSTALL_CMD`; without it the agent only simulates the switch
- Exponential backoff retry with Retry-After header support
- Permanent error detection (401, 404) — clears token and re-registers automatically
- Fire-and-forget status reporting (never blocks the FSM)

---

## Quick Start

### 1. Install dependencies

```bash
pip install -r requirements.txt
```

### 2. Configure

```bash
cp .env.example .env
```

Edit `.env`:

```env
FOTA_SERVER_URL=https://api.tinyota.xyz
FOTA_REGISTRATION_TOKEN=your-registration-token-here
FOTA_UNIT_SERIAL=my-device-001
FOTA_CURRENT_VERSION=1.0.0
```

Get your **Registration Token** from the TinyOTA console:  
**Device Models → (select model) → Registration Token**

### 3. Run

```bash
python main.py
```

On first run the agent registers the device and saves a long-lived token to `.device_token`.  
Subsequent runs reuse the saved token automatically.

---

## Configuration Reference

All settings are read from environment variables (or a `.env` file).

| Variable | Default | Description |
|----------|---------|-------------|
| `FOTA_SERVER_URL` | `https://api.tinyota.xyz` | TinyOTA server base URL |
| `FOTA_REGISTRATION_TOKEN` | _(required)_ | Token from the Device Model page |
| `FOTA_UNIT_SERIAL` | `sim-001` | Unique serial number for this unit |
| `FOTA_CURRENT_VERSION` | `1.0.0` | Firmware version currently running |
| `FOTA_CHECK_INTERVAL` | `3600` | Update check interval in seconds |
| `FOTA_RAM_FREE_KB` | `512` | Free RAM reported to server (for delta/full decision) |
| `FOTA_STORAGE_FREE_KB` | `4096` | Free storage reported to server |
| `FOTA_TOKEN_FILE` | `.device_token` | Path to persist device token across reboots |
| `FOTA_RETRY_MAX` | `4` | Max retry attempts per operation |
| `FOTA_RETRY_BASE_DELAY` | `5.0` | Base backoff delay in seconds |
| `FOTA_RETRY_MAX_DELAY` | `300.0` | Maximum backoff delay in seconds |
| `FOTA_PUBLIC_KEY_PEM` | _(required)_ | Server signing key, PEM text or file path (`GET /api/v1/devices/public-key`) |
| `FOTA_ALLOW_UNSIGNED` | _(unset)_ | `1` skips the signature check when no key is set. Development only |
| `FOTA_INSTALL_CMD` | _(unset = simulation)_ | Run as `<cmd> <image> <version>` after verification; exit 0 = installed |
| `FOTA_VERSION_FILE` | _(unset)_ | Stores the installed version across restarts |

---

## API Endpoints Used

| Method | Endpoint | Purpose |
|--------|----------|---------|
| `POST` | `/api/v1/devices/register` | First boot registration, returns device JWT |
| `POST` | `/api/v1/devices/check` | Hybrid update check (v2, with hw_specs) |
| `GET` | `/api/v1/devices/download/firmware/{id}` | Full firmware download (Range supported) |
| `GET` | `/api/v1/devices/download/delta/{id}` | Delta patch download (Range supported) |
| `POST` | `/api/v1/devices/status` | Report FSM state + error code |

---

## Resilience Behaviour

| Situation | Behaviour |
|-----------|-----------|
| Server down / 5xx | Exponential backoff, retry up to `FOTA_RETRY_MAX` times |
| 429 Too Many Requests | Respects `Retry-After` header |
| 401 Unauthorized | Clears saved token, re-registers on next cycle |
| Download interrupted | Resumes from last byte via Range request |
| Status report fails | Logged and silently ignored — FSM continues |
| Unexpected exception | Caught at main loop level — agent never crashes |
