"""Tests for the Linux device agent update cycle (pytest, no server needed)."""
import base64
import hashlib
import importlib
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

HERE = Path(__file__).resolve().parent
IMAGE = b"new firmware image " * 64


@pytest.fixture
def agent_env(tmp_path, monkeypatch):
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode()
    sig = base64.urlsafe_b64encode(key.sign(IMAGE, ec.ECDSA(hashes.SHA256()))).decode()

    install_log = tmp_path / "installed.txt"
    script = tmp_path / "install.sh"
    script.write_text(f'#!/bin/sh\ncp "$1" "{tmp_path}/image.bin" && echo "$2" > "{install_log}"\n')
    script.chmod(0o755)

    monkeypatch.setenv("FOTA_PUBLIC_KEY_PEM", pem)
    monkeypatch.setenv("FOTA_INSTALL_CMD", str(script))
    monkeypatch.setenv("FOTA_VERSION_FILE", str(tmp_path / "version"))
    monkeypatch.setenv("FOTA_TOKEN_FILE", str(tmp_path / "token"))
    monkeypatch.setenv("FOTA_CURRENT_VERSION", "1.0.0")
    monkeypatch.syspath_prepend(str(HERE))
    sys.modules.pop("main", None)
    main = importlib.import_module("main")

    agent = main.FotaAgent()
    agent.token = "jwt"
    reports = []
    agent._report = lambda status, state, error_code=0, message="": reports.append((status, error_code))

    def fake_download(url, version, payload_type):
        p = tmp_path / "download.bin"
        p.write_bytes(IMAGE)
        return str(p)

    agent._download = fake_download
    return agent, sig, reports, tmp_path


def update(sig, checksum=None):
    return {"update_available": True, "target_version": "1.1.0", "payload_type": "FULL",
            "download_url": "/api/v1/devices/download/firmware/1",
            "checksum": checksum or hashlib.sha256(IMAGE).hexdigest(), "signature": sig}


def test_valid_update_is_installed(agent_env):
    agent, sig, reports, tmp = agent_env
    agent._check_update = lambda: update(sig)
    assert agent.run_once() is True
    assert (tmp / "image.bin").read_bytes() == IMAGE
    assert (tmp / "installed.txt").read_text().strip() == "1.1.0"
    assert (tmp / "version").read_text().strip() == "1.1.0"
    assert reports[-1] == ("success", 0)


def test_bad_signature_is_rejected(agent_env):
    agent, sig, reports, tmp = agent_env
    forged = base64.urlsafe_b64encode(b"\x30\x06\x02\x01\x01\x02\x01\x01").decode()
    agent._check_update = lambda: update(forged)
    assert agent.run_once() is False
    assert not (tmp / "image.bin").exists()
    assert reports[-1] == ("failed", -5)


def test_checksum_mismatch_is_rejected(agent_env):
    agent, sig, reports, tmp = agent_env
    agent._check_update = lambda: update(sig, checksum="0" * 64)
    assert agent.run_once() is False
    assert not (tmp / "image.bin").exists()
    assert reports[-1] == ("failed", -4)


def test_missing_public_key_refuses(agent_env, monkeypatch):
    agent, sig, reports, tmp = agent_env
    main = sys.modules["main"]
    monkeypatch.setattr(main, "PUBLIC_KEY_PEM", "")
    agent._check_update = lambda: update(sig)
    assert agent.run_once() is False
    assert reports[-1] == ("failed", -5)
