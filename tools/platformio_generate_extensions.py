from pathlib import Path
import subprocess
import sys
import os

Import("env")

root = Path(env.subst("$PROJECT_DIR"))
sys.path.insert(0, str(root / "tools"))
from profile_helpers import build_source_filter, load_profile  # noqa: E402

output = root / "shared" / "generated_extension_registry.h"
generator = root / "tools" / "generate_extension_registry.py"
pioenv = env.subst("$PIOENV").strip()
if pioenv == "wiphone-core":
    profile = "core"
elif pioenv == "books-only":
    profile = "books-only"
else:
    profile = os.environ.get("WIPHONE_PROFILE", "full")
    selected_profile = root / ".wiphone-build-profile"
    if not os.environ.get("WIPHONE_PROFILE") and selected_profile.is_file():
        profile = selected_profile.read_text().strip() or profile
profile_data = load_profile(root, profile)

result = subprocess.run([
    sys.executable,
    str(generator),
    "--profile", profile,
    "--root", str(root),
    "--output", str(output),
], cwd=str(root))
if result.returncode:
    raise RuntimeError("Failed to generate WiPhone extension registry")

apps_dir = root / "apps"
env.Append(CPPPATH=[apps_dir])
env.Append(CPPPATH=[root / "shared"])
settings_dir = root / "settings"
env.Append(CPPPATH=[settings_dir])
for manifest_path in sorted(apps_dir.glob("*/manifest.json")):
    env.Append(CPPPATH=[manifest_path.parent])

for manifest_path in sorted(settings_dir.glob("*/manifest.json")):
    env.Append(CPPPATH=[manifest_path.parent])

source_filter = build_source_filter(root, profile_data)
env.Replace(SRC_FILTER=source_filter)
