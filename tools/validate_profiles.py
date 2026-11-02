#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def fail(message):
    print(f"profile-validation: FAIL: {message}", file=sys.stderr)
    return False


def main():
    valid = True
    app_ids = {
        json.loads(manifest.read_text())["id"]
        for manifest in sorted((ROOT / "apps").glob("*/manifest.json"))
    }
    setting_ids = {
        json.loads(manifest.read_text())["id"]
        for manifest in sorted((ROOT / "settings").glob("*/manifest.json"))
    }

    for profile_file in sorted((ROOT / "profiles").glob("*.json")):
        try:
            profile = json.loads(profile_file.read_text())
        except json.JSONDecodeError as exc:
            valid &= fail(f"{profile_file}: invalid JSON: {exc}")
            continue
        expected_name = profile_file.stem
        if profile.get("name") != expected_name:
            valid &= fail(
                f"{profile_file}: name must be {expected_name}"
            )
            continue
        unknown_apps = sorted(set(profile.get("apps", [])) - app_ids)
        unknown_settings = sorted(set(profile.get("settings", [])) - setting_ids)
        if unknown_apps:
            valid &= fail(f"{profile_file}: unknown apps: {', '.join(unknown_apps)}")
        if unknown_settings:
            valid &= fail(f"{profile_file}: unknown settings: {', '.join(unknown_settings)}")
        if len(profile.get("apps", [])) != len(set(profile.get("apps", []))):
            valid &= fail(f"{profile_file}: duplicate app IDs")
        if len(profile.get("settings", [])) != len(set(profile.get("settings", []))):
            valid &= fail(f"{profile_file}: duplicate setting IDs")

        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "generated.h"
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/generate_extension_registry.py"),
                    "--profile",
                    expected_name,
                    "--root",
                    str(ROOT),
                    "--output",
                    str(output),
                ],
                cwd=str(ROOT),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if result.returncode:
                valid &= fail(f"{profile_file}: generation failed: {result.stderr.strip()}")

    if valid:
        print("profile-validation: PASS")
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
