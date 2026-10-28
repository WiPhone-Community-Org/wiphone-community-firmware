#!/bin/bash
# macOS/Linux launcher for the repository personal-information scanner.
# Usage:
#   tools/scan_personal_info.sh
#   tools/scan_personal_info.sh --root .. --json scan-report.json
#
# Exit status: 0 with no high-risk findings, 1 with high-risk findings, 2 for invalid usage.
set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PYTHON=${PYTHON:-}
if [ -z "$PYTHON" ]; then
  if command -v python3 >/dev/null 2>&1; then PYTHON=python3
  elif command -v python >/dev/null 2>&1; then PYTHON=python
  else echo "error: Python 3 is required but not found in PATH" >&2; exit 2; fi
fi
ROOT="."
ARGS=()

while (($#)); do
  case "$1" in
    --root)
      if (($# < 2)); then echo "--root requires a directory" >&2; exit 2; fi
      ROOT=$2; shift 2
      ;;
    *) ARGS+=("$1"); shift ;;
  esac
done

exec "$PYTHON" "$SCRIPT_DIR/scan_personal_info.py" "$ROOT" "${ARGS[@]}"
