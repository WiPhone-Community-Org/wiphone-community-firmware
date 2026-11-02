import json
import os
from pathlib import Path


def load_profile(root, profile_name):
    profile_file = root / "profiles" / f"{profile_name}.json"
    if not profile_file.is_file():
        raise RuntimeError(f"Unknown WiPhone profile: {profile_name}")
    profile = json.loads(profile_file.read_text())
    if profile.get("name") != profile_name:
        raise RuntimeError(
            f"Profile name mismatch in {profile_file}: "
            f"expected {profile_name}, got {profile.get('name')}"
        )
    return profile


def build_source_filter(root, profile):
    selected_apps = set(profile.get("apps", []))
    selected_settings = set(profile.get("settings", []))
    excluded_sources = []

    for kind, selected in (("apps", selected_apps), ("settings", selected_settings)):
        base = root / kind
        for manifest_path in sorted(base.glob("*/manifest.json")):
            manifest = json.loads(manifest_path.read_text())
            extension_id = manifest["id"]
            if extension_id in selected:
                continue
            implementation = manifest.get("implementation")
            if not implementation:
                continue
            implementation_path = root / implementation
            relative = Path(os.path.relpath(implementation_path, root / "core"))
            # An unselected extension owns its whole directory: the implementation AND any
            # helper modules the app moved alongside it (apps/<id>/almanac_lines.cpp and the
            # app's own app_<id>.cpp are siblings now). Exclude the subtree so an unselected
            # app never compiles its helpers into every other profile.
            excluded_sources.append("-<" + (relative.parent / "**").as_posix() + ">")

    source_filter = (
        "+<*> +<../shared/*.cpp> +<../apps/*/*.cpp> +<../settings/*/*.cpp> "
        + " ".join(excluded_sources)
    )

    # app_gbc_xfer.cpp is shared firmware infrastructure now, not Game Boy code.
    # Books, files, maps, photos, and other apps use its uploader/sync window even when
    # the Game Boy app itself is not selected.
    if "game-boy" not in selected_apps:
        source_filter += " +<../apps/game-boy/app_gbc_xfer.cpp>"

    return source_filter
