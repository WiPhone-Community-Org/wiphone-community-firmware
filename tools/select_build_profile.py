#!/usr/bin/env python3
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
GENERATED_HEADER = ROOT / "shared" / "generated_extension_registry.h"


def manifest_ids(kind):
    result = {}
    for manifest in sorted((ROOT / kind).glob("*/manifest.json")):
        data = json.loads(manifest.read_text())
        result[data["id"]] = manifest.parent
    return result


def copy_sources(destination, sources):
    destination.mkdir(parents=True, exist_ok=True)
    for source in sources:
        if source.is_dir():
            for child in sorted(source.iterdir()):
                if child.name == "manifest.json":
                    continue
                target = destination / child.name
                if child.is_dir():
                    shutil.copytree(child, target, dirs_exist_ok=True)
                elif child.is_file():
                    shutil.copy2(child, target)
        elif source.is_file():
            shutil.copy2(source, destination / source.name)


def copy_headers(destination, source_dir):
    for source in sorted(source_dir.rglob("*")):
        if source.is_file() and source.suffix in (".h", ".hpp"):
            shutil.copy2(source, destination / source.name)


def stage_arduino(profile_name, profile):
    stage = ROOT / "build" / "arduino" / profile_name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    # Arduino CLI requires the main .ino filename to match its parent sketch folder.
    sketch = stage / f"{profile_name}.ino"

    # Arduino does not add a sketch subdirectory named src/ to compiler include paths.
    # Place core headers and implementation files at the sketch root so #include "GUI.h"
    # and other sketch-local headers resolve as they do in the original core layout.
    copy_sources(stage, [ROOT / "core"])
    staged_sketch_copy = stage / "core.ino"
    if staged_sketch_copy.exists():
        staged_sketch_copy.unlink()
    shutil.copy2(ROOT / "core" / "core.ino", sketch)
    staged_data_copy = stage / "data"
    if staged_data_copy.exists():
        shutil.rmtree(staged_data_copy)
    copy_sources(stage, [ROOT / "lib"])
    copy_sources(stage, [ROOT / "shared"])

    app_dirs = manifest_ids("apps")
    for app_id in profile.get("apps", []):
        copy_sources(stage, [app_dirs[app_id]])

    if "maps" not in profile.get("apps", []):
        copy_headers(stage, app_dirs["maps"])
    for app_id, app_dir in app_dirs.items():
        if app_id not in profile.get("apps", []):
            copy_headers(stage, app_dir)

    setting_dirs = manifest_ids("settings")
    for setting_id in profile.get("settings", []):
        copy_sources(stage, [setting_dirs[setting_id]])
    for setting_id, setting_dir in setting_dirs.items():
        if setting_id not in profile.get("settings", []):
            copy_headers(stage, setting_dir)

    # app_gbc_xfer is a shared core web/file-transfer service even though the Game Boy
    # app manifest owns its original files. It is referenced by core networking and GUI code.
    game_boy_dir = ROOT / "apps" / "game-boy"
    copy_sources(stage, [
        game_boy_dir / "chunk_proto.h",
        game_boy_dir / "app_gbc_xfer.cpp",
        ROOT / "apps" / "files" / "files_paths.h",
        ROOT / "apps" / "books" / "kosync.h",
        ROOT / "apps" / "books" / "kosync_sync.h",
    ])

    if (ROOT / "core" / "data").is_dir():
        shutil.copytree(ROOT / "core" / "data", stage / "data", dirs_exist_ok=True)

    # GUI.h's PlatformIO/Arduino include path is ../shared/generated_extension_registry.h,
    # so shared/ is intentionally a sibling of the profile sketch.
    shared_dir = stage.parent / "shared"
    shared_dir.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "shared" / "ExtensionRegistry.h", shared_dir / "ExtensionRegistry.h")
    shutil.copy2(GENERATED_HEADER, shared_dir / "generated_extension_registry.h")
    shutil.copy2(GENERATED_HEADER, stage / "generated_extension_registry.h")

    # Arduino may compile copied .cpp files outside the sketch tree. Use a
    # sketch-local include path for staged generated registry consumers.
    for staged_file in (stage / "GUI.h", stage / "GUI.cpp", stage / "core_optional_stubs.cpp"):
        if staged_file.exists():
            text = staged_file.read_text().replace(
                "../shared/generated_extension_registry.h",
                "generated_extension_registry.h",
            )
            staged_file.write_text(text)

    arduino_json = ROOT / ".vscode" / "arduino.json"
    arduino_json.parent.mkdir(parents=True, exist_ok=True)
    data = json.loads(arduino_json.read_text()) if arduino_json.exists() else {}
    data["sketch"] = str(sketch.relative_to(ROOT))
    arduino_json.write_text(json.dumps(data, indent=4) + "\n")
    return stage


def comma_ids(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile")
    parser.add_argument("--name", help="Name for a custom profile")
    parser.add_argument("--apps", help="Comma-separated app IDs")
    parser.add_argument("--settings", help="Comma-separated setting IDs")
    parser.add_argument("--target", choices=["platformio", "arduino", "generate"], default="generate")
    args = parser.parse_args()

    if args.apps is not None or args.settings is not None:
        if not args.name:
            parser.error("--name is required when selecting custom apps or settings")
        profile_name = args.name
        if not re.fullmatch(r"[A-Za-z0-9_-]+", profile_name):
            parser.error("--name may contain only letters, numbers, hyphens, and underscores")
        selected_apps = comma_ids(args.apps or "")
        selected_settings = comma_ids(args.settings or "")
        unknown_apps = sorted(set(selected_apps) - set(manifest_ids("apps")))
        unknown_settings = sorted(set(selected_settings) - set(manifest_ids("settings")))
        if unknown_apps:
            parser.error(f"Unknown app IDs: {', '.join(unknown_apps)}")
        if unknown_settings:
            parser.error(f"Unknown setting IDs: {', '.join(unknown_settings)}")
        profile = {
            "name": profile_name,
            "apps": selected_apps,
            "settings": selected_settings,
        }
        profile_file = ROOT / "profiles" / f"{profile_name}.json"
        profile_file.write_text(json.dumps(profile, indent=2) + "\n")
    else:
        profile_name = args.profile or "full"
        profile_file = ROOT / "profiles" / f"{profile_name}.json"
        if not profile_file.exists():
            parser.error(f"Unknown WiPhone profile: {profile_name}")
        profile = json.loads(profile_file.read_text())

    subprocess.run([
        sys.executable,
        str(ROOT / "tools/generate_extension_registry.py"),
        "--profile", profile_name,
        "--root", str(ROOT),
        "--output", str(GENERATED_HEADER),
    ], check=True)

    if args.target == "platformio":
        selected = ROOT / ".wiphone-build-profile"
        selected.write_text(profile_name + "\n")
        print(profile_name)
    elif args.target == "arduino":
        stage = stage_arduino(profile_name, profile)
        print(stage.relative_to(ROOT))
    else:
        print(profile_name)


if __name__ == "__main__":
    raise SystemExit(main())
