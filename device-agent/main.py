#!/usr/bin/env python3
"""
FOTA Device Agent — Python Reference Implementation (v2)

Implements the PRD v1.0 device-side flow:
  IDLE → CHECKING → DOWNLOADING → PATCHING → VERIFYING → SWITCHING → REBOOT

API endpoints used:
  POST /api/v1/devices/register       — first boot, returns 365-day JWT
  POST /api/v1/devices/check          — hybrid update check (v2, hw_specs)
  GET  /api/v1/devices/download/...   — Range-request resumable download
  POST /api/v1/devices/status         — report fsm_state + error_code

Usage:
  Set environment variables (or edit config.py):
    FOTA_SERVER_URL           e.g. http://localhost:8000
    FOTA_REGISTRATION_TOKEN   token embedded at manufacture
    FOTA_UNIT_SERIAL          optional serial number
    FOTA_CURRENT_VERSION      e.g. 1.0.0
"""

import base64
import hashlib
import logging
import os
import shlex
import subprocess
import time
from enum import IntEnum
from pathlib import Path
from typing import Optional

import requests
from requests.exceptions import ConnectionError, Timeout, HTTPError

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("fota-agent")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SERVER_URL          = os.getenv("FOTA_SERVER_URL", "http://localhost:8000")
REGISTRATION_TOKEN  = os.getenv("FOTA_REGISTRATION_TOKEN", "")
UNIT_SERIAL         = os.getenv("FOTA_UNIT_SERIAL", "sim-001")
CURRENT_VERSION     = os.getenv("FOTA_CURRENT_VERSION", "1.0.0")
CHECK_INTERVAL      = int(os.getenv("FOTA_CHECK_INTERVAL", "3600"))
RAM_FREE_KB         = int(os.getenv("FOTA_RAM_FREE_KB", "512"))
STORAGE_FREE_KB     = int(os.getenv("FOTA_STORAGE_FREE_KB", "4096"))
TOKEN_FILE          = Path(os.getenv("FOTA_TOKEN_FILE", ".device_token"))

# Installing on a real Linux device (see examples/linux/README.md)
#   FOTA_PUBLIC_KEY_PEM  PEM text or path of the server signing key
#                        (GET /api/v1/devices/public-key). Required unless
#                        FOTA_ALLOW_UNSIGNED=1 (development only).
#   FOTA_INSTALL_CMD     command run as: <cmd> <image path> <new version>.
#                        Exit 0 = installed. Unset = simulation mode.
#   FOTA_VERSION_FILE    file that stores the installed version across
#                        restarts; overrides FOTA_CURRENT_VERSION when present.
PUBLIC_KEY_PEM      = os.getenv("FOTA_PUBLIC_KEY_PEM", "")
ALLOW_UNSIGNED      = os.getenv("FOTA_ALLOW_UNSIGNED", "") == "1"
INSTALL_CMD         = os.getenv("FOTA_INSTALL_CMD", "")
VERSION_FILE        = Path(os.getenv("FOTA_VERSION_FILE")) if os.getenv("FOTA_VERSION_FILE") else None

# Retry / backoff settings
RETRY_MAX           = int(os.getenv("FOTA_RETRY_MAX", "4"))       # max attempts per operation
RETRY_BASE_DELAY    = float(os.getenv("FOTA_RETRY_BASE_DELAY", "5.0"))   # seconds
RETRY_MAX_DELAY     = float(os.getenv("FOTA_RETRY_MAX_DELAY", "300.0"))  # cap at 5 minutes

# ---------------------------------------------------------------------------
# FSM states and error codes (mirrors fota_fsm.h)
# ---------------------------------------------------------------------------

class FsmState:
    IDLE        = "IDLE"
    CHECKING    = "CHECKING"
    DOWNLOADING = "DOWNLOADING"
    PATCHING    = "PATCHING"
    VERIFYING   = "VERIFYING"
    SWITCHING   = "SWITCHING"
    REBOOT      = "REBOOT"
    ERROR       = "ERROR"

class FotaError(IntEnum):
    NONE               =  0
    NETWORK            = -1
    NO_UPDATE          = -2
    DOWNLOAD           = -3
    HASH_MISMATCH      = -4
    SIG_INVALID        = -5
    FLASH_WRITE        = -6
    PATCH_APPLY        = -7
    BOOT_SWITCH        = -8
    INSUFFICIENT_SPACE = -9

# ---------------------------------------------------------------------------
# Error classification helpers
# ---------------------------------------------------------------------------

def _is_transient_error(exc: Exception) -> bool:
    """
    Returns True for errors worth retrying:
    - Network unreachable / timeout → server may be temporarily down
    - HTTP 429 (rate limited) → back off and retry
    - HTTP 5xx (server error) → transient server issue

    Returns False for permanent errors (no retry):
    - HTTP 401 / 403 → token invalid or revoked, needs re-registration
    - HTTP 404 → resource gone, no point retrying
    - HTTP 400 → bad request (client bug), retrying won't help
    """
    if isinstance(exc, (ConnectionError, Timeout)):
        return True
    if isinstance(exc, HTTPError) and exc.response is not None:
        status = exc.response.status_code
        return status == 429 or status >= 500
    return False


def _retry_after_seconds(exc: Exception) -> Optional[float]:
    """Extract Retry-After header value from a 429 response, if present."""
    if isinstance(exc, HTTPError) and exc.response is not None:
        if exc.response.status_code == 429:
            retry_after = exc.response.headers.get("Retry-After")
            if retry_after:
                try:
                    return float(retry_after)
                except ValueError:
                    pass
    return None


def _backoff_delay(attempt: int, exc: Exception) -> float:
    """
    Exponential backoff: base * 2^attempt, capped at RETRY_MAX_DELAY.
    Respects Retry-After header on 429 responses.
    """
    server_hint = _retry_after_seconds(exc)
    if server_hint is not None:
        return min(server_hint, RETRY_MAX_DELAY)
    delay = RETRY_BASE_DELAY * (2 ** attempt)
    return min(delay, RETRY_MAX_DELAY)


# ---------------------------------------------------------------------------
# Agent
# ---------------------------------------------------------------------------

class FotaAgent:
    def __init__(self):
        self.server_url      = SERVER_URL.rstrip("/")
        self.current_version = CURRENT_VERSION
        if VERSION_FILE and VERSION_FILE.exists():
            stored = VERSION_FILE.read_text().strip()
            if stored:
                self.current_version = stored
        self.token: Optional[str] = None
        self.state = FsmState.IDLE
        self._load_token()

    # ------------------------------------------------------------------
    # Token persistence
    # ------------------------------------------------------------------

    def _load_token(self):
        if TOKEN_FILE.exists():
            self.token = TOKEN_FILE.read_text().strip()
            logger.info("Loaded device token from disk")

    def _save_token(self, token: str):
        self.token = token
        TOKEN_FILE.write_text(token)
        logger.info("Device token saved to disk")

    # ------------------------------------------------------------------
    # Registration (first boot or token missing)
    # ------------------------------------------------------------------

    def register(self) -> bool:
        if not REGISTRATION_TOKEN:
            logger.error("FOTA_REGISTRATION_TOKEN not set — cannot register")
            return False

        for attempt in range(RETRY_MAX):
            try:
                resp = requests.post(
                    f"{self.server_url}/api/v1/devices/register",
                    json={"registration_token": REGISTRATION_TOKEN, "unit_serial": UNIT_SERIAL},
                    timeout=10,
                )
                resp.raise_for_status()
                data = resp.json()
                self._save_token(data["access_token"])
                logger.info(f"Registered as unit_id={data['unit_id']} model={data['model_key']}")
                return True

            except Exception as exc:
                if not _is_transient_error(exc):
                    # Permanent error (e.g. 404 invalid token, 403 forbidden)
                    logger.error(f"Registration permanently failed (attempt {attempt+1}): {exc}")
                    return False

                delay = _backoff_delay(attempt, exc)
                if attempt + 1 < RETRY_MAX:
                    logger.warning(
                        f"Registration failed (attempt {attempt+1}/{RETRY_MAX}), "
                        f"retrying in {delay:.0f}s: {exc}"
                    )
                    time.sleep(delay)
                else:
                    logger.error(f"Registration failed after {RETRY_MAX} attempts: {exc}")

        return False

    def _auth_headers(self) -> dict:
        return {"Authorization": f"Bearer {self.token}"}

    # ------------------------------------------------------------------
    # FSM helpers
    # ------------------------------------------------------------------

    def _transition(self, new_state: str):
        logger.info(f"FSM: {self.state} → {new_state}")
        self.state = new_state

    def _report(self, status: str, fsm_state: str, error_code: int = 0, message: str = ""):
        """Fire-and-forget status report. Never blocks the FSM."""
        try:
            requests.post(
                f"{self.server_url}/api/v1/devices/status",
                json={
                    "status": status,
                    "version": self.current_version,
                    "fsm_state": fsm_state,
                    "error_code": error_code,
                    "message": message,
                },
                headers=self._auth_headers(),
                timeout=5,
            )
        except Exception as exc:
            # Status reporting is best-effort; never abort the FSM for this
            logger.warning(f"Status report failed (non-fatal): {exc}")

    # ------------------------------------------------------------------
    # Main update cycle
    # ------------------------------------------------------------------

    def run_once(self) -> bool:
        """Run one complete update cycle. Returns True if firmware was updated."""
        if not self.token and not self.register():
            return False

        # ---- CHECKING ----
        self._transition(FsmState.CHECKING)
        update = self._check_update()
        if update is None:
            self._report("no_update", FsmState.IDLE, FotaError.NO_UPDATE)
            self._transition(FsmState.IDLE)
            return False

        payload_type     = update.get("payload_type", "FULL")
        download_url     = update["download_url"]
        target_version   = update["target_version"]
        expected_checksum = update.get("checksum") or ""
        logger.info(f"Update available: {self.current_version} → {target_version} ({payload_type})")

        # ---- DOWNLOADING ----
        self._transition(FsmState.DOWNLOADING)
        self._report("downloading", FsmState.DOWNLOADING)
        local_path = self._download(download_url, target_version, payload_type)
        if not local_path:
            self._report("failed", FsmState.ERROR, FotaError.DOWNLOAD, "Download failed")
            self._transition(FsmState.IDLE)
            return False

        # ---- VERIFY PAYLOAD ----
        # checksum is the SHA-256 of what was downloaded (the FULL image or
        # the DELTA patch), so it is checked before patching.
        if not expected_checksum or not self._verify_sha256(local_path, expected_checksum):
            logger.error("Missing or wrong SHA-256 — aborting update")
            self._report("failed", FsmState.ERROR, FotaError.HASH_MISMATCH, "SHA-256 mismatch")
            Path(local_path).unlink(missing_ok=True)
            self._transition(FsmState.IDLE)
            return False

        # ---- PATCHING ----
        self._transition(FsmState.PATCHING)
        self._report("patching", FsmState.PATCHING)
        if payload_type == "DELTA":
            patched_path = self._apply_delta(local_path)
            if not patched_path:
                self._report("failed", FsmState.ERROR, FotaError.PATCH_APPLY, "Delta patch failed")
                self._transition(FsmState.IDLE)
                return False
        else:
            patched_path = local_path

        # ---- VERIFYING ----
        # The signature covers the final image, for FULL and DELTA alike.
        self._transition(FsmState.VERIFYING)
        self._report("verifying", FsmState.VERIFYING)
        if not self._verify_signature(patched_path, update.get("signature") or ""):
            self._report("failed", FsmState.ERROR, FotaError.SIG_INVALID, "Signature invalid")
            Path(patched_path).unlink(missing_ok=True)
            self._transition(FsmState.IDLE)
            return False

        # ---- SWITCHING ----
        self._transition(FsmState.SWITCHING)
        self._report("switching", FsmState.SWITCHING)
        if INSTALL_CMD:
            logger.info(f"Installing {target_version}: {INSTALL_CMD} {patched_path} {target_version}")
            # No shell: the version string comes from the server.
            result = subprocess.run(shlex.split(INSTALL_CMD) + [str(patched_path), target_version],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                logger.error(f"Install command failed ({result.returncode}): {result.stderr.strip()}")
                self._report("failed", FsmState.ERROR, FotaError.BOOT_SWITCH,
                             f"install exit {result.returncode}")
                Path(patched_path).unlink(missing_ok=True)
                self._transition(FsmState.IDLE)
                return False
        else:
            logger.info(f"Simulating boot slot switch to firmware {target_version} (FOTA_INSTALL_CMD not set)")
        self.current_version = target_version
        if VERSION_FILE:
            VERSION_FILE.write_text(target_version + "\n")
        Path(patched_path).unlink(missing_ok=True)

        # ---- REBOOT ----
        self._transition(FsmState.REBOOT)
        self._report("success", FsmState.REBOOT, message=f"Updated to {target_version}")
        logger.info("Update complete — simulating reboot (agent continues in simulation mode)")
        self._transition(FsmState.IDLE)
        return True

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _check_update(self) -> Optional[dict]:
        """
        POST /devices/check with exponential backoff on transient errors.
        Returns None on both "no update" and permanent failure.
        On 401, clears token so next cycle triggers re-registration.
        """
        for attempt in range(RETRY_MAX):
            try:
                resp = requests.post(
                    f"{self.server_url}/api/v1/devices/check",
                    json={
                        "current_version": self.current_version,
                        "hw_specs": {
                            "ram_free": RAM_FREE_KB,
                            "storage_free": STORAGE_FREE_KB,
                        },
                    },
                    headers=self._auth_headers(),
                    timeout=10,
                )
                resp.raise_for_status()
                data = resp.json()
                if not data.get("update_available"):
                    logger.info("No update available")
                    return None
                return data

            except HTTPError as exc:
                status = exc.response.status_code if exc.response is not None else 0
                if status == 401:
                    # Token expired or revoked — clear and re-register next cycle
                    logger.warning("Device token rejected (401) — will re-register next cycle")
                    self.token = None
                    TOKEN_FILE.unlink(missing_ok=True)
                    return None
                if not _is_transient_error(exc):
                    logger.error(f"Update check permanently failed (HTTP {status}): {exc}")
                    return None
                # Transient (429, 5xx): fall through to backoff
                delay = _backoff_delay(attempt, exc)
                logger.warning(
                    f"Update check failed HTTP {status} (attempt {attempt+1}/{RETRY_MAX}), "
                    f"retrying in {delay:.0f}s"
                )
                if attempt + 1 < RETRY_MAX:
                    time.sleep(delay)

            except (ConnectionError, Timeout) as exc:
                # Server unreachable — log and back off; device continues normally
                delay = _backoff_delay(attempt, exc)
                logger.warning(
                    f"Server unreachable (attempt {attempt+1}/{RETRY_MAX}), "
                    f"retrying in {delay:.0f}s: {exc}"
                )
                if attempt + 1 < RETRY_MAX:
                    time.sleep(delay)

            except Exception as exc:
                logger.error(f"Unexpected error during update check: {exc}")
                return None

        logger.error(f"Update check failed after {RETRY_MAX} attempts — will retry next cycle")
        return None

    def _download(self, url: str, version: str, payload_type: str) -> Optional[str]:
        """
        Download with Range-request resume support and exponential backoff.
        On failure, preserves partial file for resume on next attempt.
        """
        ext  = ".patch" if payload_type == "DELTA" else ".bin"
        dest = Path(f"firmware_{version}{ext}")

        if url.startswith("/"):
            url = self.server_url + url

        for attempt in range(RETRY_MAX):
            resume_offset = dest.stat().st_size if dest.exists() else 0
            headers = dict(self._auth_headers())
            if resume_offset:
                headers["Range"] = f"bytes={resume_offset}-"
                logger.info(f"Resuming download from byte {resume_offset}")

            try:
                resp = requests.get(url, headers=headers, stream=True, timeout=30)
                resp.raise_for_status()

                mode = "ab" if resume_offset and resp.status_code == 206 else "wb"
                with open(dest, mode) as f:
                    for chunk in resp.iter_content(chunk_size=8192):
                        if chunk:
                            f.write(chunk)

                logger.info(f"Downloaded {dest} ({dest.stat().st_size} bytes)")
                return str(dest)

            except HTTPError as exc:
                status = exc.response.status_code if exc.response is not None else 0
                if status == 401:
                    logger.warning("Token rejected during download (401)")
                    self.token = None
                    TOKEN_FILE.unlink(missing_ok=True)
                    return None
                if not _is_transient_error(exc):
                    logger.error(f"Download permanently failed (HTTP {status}): {exc}")
                    return None
                delay = _backoff_delay(attempt, exc)
                logger.warning(
                    f"Download failed HTTP {status} (attempt {attempt+1}/{RETRY_MAX}), "
                    f"retrying in {delay:.0f}s"
                )
                if attempt + 1 < RETRY_MAX:
                    time.sleep(delay)

            except (ConnectionError, Timeout) as exc:
                delay = _backoff_delay(attempt, exc)
                logger.warning(
                    f"Download interrupted (attempt {attempt+1}/{RETRY_MAX}), "
                    f"resuming in {delay:.0f}s: {exc}"
                )
                if attempt + 1 < RETRY_MAX:
                    time.sleep(delay)

            except Exception as exc:
                logger.error(f"Unexpected download error: {exc}")
                return None

        logger.error(f"Download failed after {RETRY_MAX} attempts — partial file preserved for next cycle")
        return None

    def _apply_delta(self, delta_path: str) -> Optional[str]:
        """Apply bsdiff4 delta patch (simulation: treat delta as full firmware)."""
        try:
            import bsdiff4  # type: ignore
            logger.info("Applying bsdiff4 delta patch (simulation)")
            patched = delta_path.replace(".patch", ".bin")
            import shutil
            shutil.copy(delta_path, patched)
            Path(delta_path).unlink(missing_ok=True)
            return patched
        except ImportError:
            logger.warning("bsdiff4 not installed — treating delta as full firmware")
            return delta_path
        except Exception as exc:
            logger.error(f"Delta patch failed: {exc}")
            return None

    def _verify_signature(self, file_path: str, signature_b64url: str) -> bool:
        """ECDSA P-256 / SHA-256 over the image; signature is base64url DER."""
        if not PUBLIC_KEY_PEM:
            if ALLOW_UNSIGNED:
                logger.warning("FOTA_PUBLIC_KEY_PEM not set — skipping signature check (FOTA_ALLOW_UNSIGNED=1)")
                return True
            logger.error("FOTA_PUBLIC_KEY_PEM not set — refusing unsigned update")
            return False
        if not signature_b64url:
            logger.error("Server sent no signature")
            return False
        try:
            from cryptography.exceptions import InvalidSignature
            from cryptography.hazmat.primitives import hashes, serialization
            from cryptography.hazmat.primitives.asymmetric import ec
        except ImportError:
            logger.error("pip install cryptography to verify signatures")
            return False
        pem = PUBLIC_KEY_PEM
        if not pem.lstrip().startswith("-----BEGIN") and Path(pem).exists():
            pem = Path(pem).read_text()
        key = serialization.load_pem_public_key(pem.encode())
        sig = base64.urlsafe_b64decode(signature_b64url + "=" * (-len(signature_b64url) % 4))
        try:
            key.verify(sig, Path(file_path).read_bytes(), ec.ECDSA(hashes.SHA256()))
        except InvalidSignature:
            logger.error("ECDSA signature invalid")
            return False
        logger.info("ECDSA signature verified ✓")
        return True

    def _verify_sha256(self, file_path: str, expected: str) -> bool:
        sha256 = hashlib.sha256()
        with open(file_path, "rb") as f:
            for chunk in iter(lambda: f.read(4096), b""):
                sha256.update(chunk)
        actual = sha256.hexdigest()
        if actual == expected:
            logger.info("SHA-256 verified ✓")
            return True
        logger.error(f"SHA-256 mismatch: expected={expected} actual={actual}")
        return False

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    def run(self):
        logger.info(f"FOTA Agent started — server={self.server_url} version={self.current_version}")
        while True:
            try:
                self.run_once()
            except Exception as exc:
                # Catch-all: device must never crash regardless of server state
                logger.error(f"Unexpected error in run_once: {exc}", exc_info=True)
                self._report("error", FsmState.ERROR, message=str(exc))
                self._transition(FsmState.IDLE)

            logger.info(f"Sleeping {CHECK_INTERVAL}s until next check")
            time.sleep(CHECK_INTERVAL)


if __name__ == "__main__":
    FotaAgent().run()
