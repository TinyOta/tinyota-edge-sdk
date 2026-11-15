#!/usr/bin/env python3
"""Generate AI assistant context files from docs/AI_CONTEXT.md.

    python3 scripts/sync_ai_context.py          # write the copies
    python3 scripts/sync_ai_context.py --check  # CI: fail if stale or out of sync with headers

One source, four copies, so Claude Code (CLAUDE.md), Codex and others
(AGENTS.md), GitHub Copilot (.github/copilot-instructions.md) and Cursor
(.cursor/rules/tinyota.mdc) all read the same, header-checked text.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "docs" / "AI_CONTEXT.md"
HEADERS = ["include/fota_hal.h", "include/fota_platform.h", "include/fota_fsm.h"]
NOTE = "<!-- Generated from docs/AI_CONTEXT.md by scripts/sync_ai_context.py. Edit that file, not this one. -->\n\n"
CURSOR_FRONT = (
    "---\n"
    "description: TinyOTA Edge SDK integration rules (OTA firmware updates)\n"
    "alwaysApply: true\n"
    "---\n\n"
)
# Names that older docs used and that do not exist in the SDK.
STALE_NAMES = ["hal_reboot(", "hal_get_device_id", "hal_get_boot_active_slot",
               "fota_platform_config_t", "check-update"]


def targets(body: str) -> dict:
    return {
        ROOT / "AGENTS.md": NOTE + body,
        ROOT / "CLAUDE.md": NOTE + body,
        ROOT / ".github" / "copilot-instructions.md": NOTE + body,
        ROOT / ".cursor" / "rules" / "tinyota.mdc": CURSOR_FRONT + NOTE + body,
    }


def header_functions() -> set:
    names = set()
    for rel in HEADERS:
        text = (ROOT / rel).read_text()
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for m in re.finditer(r"^\s*[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{]*\)\s*;", text, flags=re.M):
            if not m.group(1).startswith("typedef"):
                names.add(m.group(1))
    return names


def main() -> int:
    body = SOURCE.read_text()
    files = targets(body)
    if "--check" not in sys.argv:
        for path, content in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            print(f"wrote {path.relative_to(ROOT)}")
        return 0

    errors = []
    for name in sorted(header_functions()):
        if name not in body:
            errors.append(f"docs/AI_CONTEXT.md does not mention {name}() from the public headers")
    for name in STALE_NAMES:
        if name in body:
            errors.append(f"docs/AI_CONTEXT.md uses '{name}', which is not part of the SDK")
    for path, content in files.items():
        if not path.exists() or path.read_text() != content:
            errors.append(f"{path.relative_to(ROOT)} is stale: run python3 scripts/sync_ai_context.py")
    for e in errors:
        print("error:", e)
    if not errors:
        print(f"AI context OK ({len(header_functions())} header functions covered)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
