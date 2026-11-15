# Linux devices (Raspberry Pi, gateways, Yocto, OpenWrt with Python)

Runs the Python agent (`device-agent/`) as a systemd service. It checks for
updates, verifies SHA-256 and the ECDSA signature, then calls your install
script with the verified file. Covered by unit tests (`device-agent/test_agent.py`).

## Install

```bash
sudo mkdir -p /opt/tinyota /etc/tinyota /usr/local/lib/tinyota
sudo cp -r ../../device-agent /opt/tinyota/
sudo python3 -m venv /opt/tinyota/venv
sudo /opt/tinyota/venv/bin/pip install -r /opt/tinyota/device-agent/requirements.txt

sudo cp tinyota.env.example /etc/tinyota/tinyota.env      # fill it in
sudo chmod 600 /etc/tinyota/tinyota.env
curl -s https://api.tinyota.xyz/api/v1/devices/public-key \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["public_key_pem"])' \
  | sudo tee /etc/tinyota/tinyota_pubkey.pem
sudo install -m 0755 install-app.sh /usr/local/lib/tinyota/install-app.sh   # edit for your app

sudo cp tinyota-agent.service /etc/systemd/system/
sudo systemctl enable --now tinyota-agent
journalctl -u tinyota-agent -f
```

## What to upload

Whatever your `install-app.sh` expects: a single binary, a `.tar.gz` that
the script unpacks, or a `.deb` it installs with `dpkg -i`. Use the same
version string as the one in `FOTA_CURRENT_VERSION` / the version file.

For full root-filesystem A/B updates, call RAUC or SWUpdate from the install
script (`rauc install "$1"`).

## Prompt for your AI assistant

```text
Read AGENTS.md in the TinyOTA SDK and examples/linux/.
My device runs <Raspberry Pi OS>; my app is <a Go binary at /opt/myapp/myapp
run by myapp.service>. Adapt install-app.sh so it installs the verified file,
restarts the service, rolls back if the restart fails, and exits non-zero on
failure. Do not change device-agent/main.py. Show the final install-app.sh and
the tinyota.env values I need.
```
