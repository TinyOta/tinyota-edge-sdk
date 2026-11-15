"""
Device Agent Configuration — thin wrapper around environment variables.

All settings are read by main.py directly via os.getenv().
This module is kept for backwards compatibility and IDE auto-complete.

Preferred usage: set variables in .env (see .env.example) or export them
before running:

    export FOTA_SERVER_URL=https://api.tinyota.xyz
    export FOTA_REGISTRATION_TOKEN=your-token
    python main.py
"""
import os
from pathlib import Path

SERVER_URL         = os.getenv("FOTA_SERVER_URL", "https://api.tinyota.xyz")
REGISTRATION_TOKEN = os.getenv("FOTA_REGISTRATION_TOKEN", "")
UNIT_SERIAL        = os.getenv("FOTA_UNIT_SERIAL", "sim-001")
CURRENT_VERSION    = os.getenv("FOTA_CURRENT_VERSION", "1.0.0")
CHECK_INTERVAL     = int(os.getenv("FOTA_CHECK_INTERVAL", "3600"))
RAM_FREE_KB        = int(os.getenv("FOTA_RAM_FREE_KB", "512"))
STORAGE_FREE_KB    = int(os.getenv("FOTA_STORAGE_FREE_KB", "4096"))
TOKEN_FILE         = Path(os.getenv("FOTA_TOKEN_FILE", ".device_token"))
RETRY_MAX          = int(os.getenv("FOTA_RETRY_MAX", "4"))
RETRY_BASE_DELAY   = float(os.getenv("FOTA_RETRY_BASE_DELAY", "5.0"))
RETRY_MAX_DELAY    = float(os.getenv("FOTA_RETRY_MAX_DELAY", "300.0"))
