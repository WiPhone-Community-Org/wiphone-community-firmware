#!/usr/bin/env python3
"""Scan a directory for secrets and potentially public personal information.

The scanner checks text and common build binaries under the working tree. It intentionally
reports findings for human review; pattern matching cannot prove whether a value is secret.

Examples:
  python3 tools/scan_personal_info.py .
  python3 tools/scan_personal_info.py /path/to/repo --json report.json
  python3 tools/scan_personal_info.py . --exclude .pio --exclude node_modules
"""

import argparse
import fnmatch
import json
import os
import re
import socket
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT_EXCLUDES = {".git", "__pycache__", ".DS_Store"}
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".cfg", ".conf", ".csv", ".gradle", ".h", ".hpp",
    ".html", ".ini", ".ino", ".java", ".js", ".json", ".jsx", ".md", ".properties",
    ".ps1", ".py", ".sh", ".sql", ".svelte", ".ts", ".tsx", ".txt", ".xml", ".yaml", ".yml",
}
SENSITIVE_FILENAMES = re.compile(
    r"(^|[._-])(env|credential[s]?|secret[s]?|token[s]?|password[s]?|private[_-]?key|id_rsa)([._-]|$)|"
    r"\.(pem|p12|pfx|jks|keystore)$",
    re.I,
)

SECRET_PATTERNS = [
    ("Private key", re.compile(r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----")),
    ("AWS access key", re.compile(r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("GitHub token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{36,255}|github_pat_[A-Za-z0-9_]{20,255})\b")),
    ("OpenAI-like key", re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b")),
    ("Google API key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("Slack token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}\b")),
    ("Bearer authorization", re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]{16,}")),
    ("Generic assignment", re.compile(r"(?i)\b(?:api[_-]?key|access[_-]?token|auth[_-]?token|secret[_-]?key|client[_-]?secret|password|passwd|hotspot_pass|wifi[_-]?password)\b\s*[:=]\s*[\"']?[A-Za-z0-9._~+/=-]{8,}[\"']?")),
]
PERSONAL_PATTERNS = [
    ("Email address", re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b")),
    ("IPv4 address", re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")),
    ("MAC address", re.compile(r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b")),
    ("SIP/VoIP identifier", re.compile(r"\b(?:sip:|sips:)[^\s<>\]\[\",]+", re.I)),
    ("Absolute Unix path", re.compile(r"(?:/Users/[^\s\"'<>]+|/home/[^\s\"'<>]+)")),
    ("Absolute Windows path", re.compile(r"\b[A-Za-z]:\\(?:[^\s\"'<>]+\\?)+")),
    ("USB or device serial", re.compile(r"(?i)\b(?:usbserial-|cu\.usbserial-|025A[0-9A-F]{4}|!00\d{5})[^\s\"'<>]*")),
]


def line_for(data, offset):
    return data.count(b"\n", 0, offset) + 1 if isinstance(data, bytes) else data.count("\n", 0, offset) + 1


def public_ipv4(value):
    parts = value.split(".")
    if len(parts) != 4 or any(not p.isdigit() or not 0 <= int(p) <= 255 for p in parts):
        return False
    a, b = int(parts[0]), int(parts[1])
    return not (a == 10 or a == 127 or (a == 169 and b == 254) or (a == 172 and 16 <= b <= 31) or
                (a == 192 and b == 168) or a == 0 or (a == 100 and 64 <= b <= 127))


def sanitize_match(value, label):
    if label in ("Generic assignment", "Bearer authorization"):
        return value.split("=", 1)[-1].strip(" \t\"'")[:12] + "..." if "=" in value else value[:16] + "..."
    if len(value) > 160:
        return value[:157] + "..."
    return value


def local_personal_tokens(root):
    tokens = set()
    for key in ("USER", "USERNAME", "LOGNAME", "HOSTNAME", "COMPUTERNAME"):
        value = os.environ.get(key, "").strip()
        if len(value) >= 3:
            tokens.add(value.lower())
    try:
        tokens.add(socket.gethostname().lower())
    except Exception:
        pass
    root_name = root.name.lower()
    if len(root_name) >= 3:
        tokens.add(root_name)
    home = Path(os.path.expanduser("~")).resolve(strict=False)
    if home.name:
        tokens.add(home.name.lower())
    return tokens


def should_exclude(path, root, excludes):
    try:
        rel = path.relative_to(root)
    except ValueError:
        return True
    parts = rel.parts
    if any(part in DEFAULT_EXCLUDES for part in parts):
        return True
    rel_text = str(rel)
    for item in excludes:
        normalized = item.strip("/\\").replace("\\", "/")
        if normalized and (normalized in parts or fnmatch.fnmatch(rel_text.replace("\\", "/"), normalized + "/*") or
                           fnmatch.fnmatch(rel_text.replace("\\", "/"), normalized) or
                           fnmatch.fnmatch(path.name, normalized)):
            return True
    return False


def scan_file(path, root, personal_tokens, max_bytes):
    findings = []
    try:
        size = path.stat().st_size
        if size > max_bytes:
            return [{"severity": "LOW", "category": "Large file", "value": f"> {max_bytes // 1048576} MB", "line": 0,
                     "path": str(path.relative_to(root)), "note": "File was not fully scanned."}], size
        data = path.read_bytes()
    except (OSError, PermissionError) as exc:
        return [{"severity": "LOW", "category": "Scan error", "value": str(exc), "line": 0,
                 "path": str(path.relative_to(root)), "note": "Unreadable file."}], 0

    rel = str(path.relative_to(root))
    if SENSITIVE_FILENAMES.search(path.name) or path.name.lower() in {".env", "id_rsa", "id_ed25519"}:
        findings.append({"severity": "MEDIUM", "category": "Sensitive filename", "value": path.name, "line": 0,
                         "path": rel, "note": "Review contents and whether this file belongs in the repository."})

    sample = data[:4]
    nul_count = data.count(b"\x00")
    is_likely_binary = nul_count > max(32, len(data) // 20)
    if is_likely_binary:
        texts = []
        for raw in re.findall(rb"[\x20-\x7e]{4,}|(?:[\x20-\x7e]\x00){4,}", data[:max_bytes]):
            try:
                texts.append(raw.decode("ascii").replace("\x00", ""))
            except UnicodeDecodeError:
                pass
        text = "\n".join(texts)
        offsets_are_lines = False
    else:
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = data.decode("latin-1", errors="replace")
        offsets_are_lines = True

    lines = text.splitlines() if offsets_are_lines else []

    for label, pattern in SECRET_PATTERNS:
        matches = list(pattern.finditer(text))[:100]
        for match in matches:
            line = text[:match.start()].count("\n") + 1 if offsets_are_lines else 0
            value = sanitize_match(match.group(0), label)
            findings.append({"severity": "HIGH", "category": label, "value": value, "line": line,
                             "path": rel, "note": "Potential secret; rotate/revoke if real."})

    for label, pattern in PERSONAL_PATTERNS:
        matches = list(pattern.finditer(text))[:100]
        for match in matches:
            raw = match.group(0).strip(" t\"'`")
            if label == "IPv4 address":
                parts = raw.split(".")
                if len(parts) != 4 or any(not p.isdigit() or not 0 <= int(p) <= 255 for p in parts):
                    continue
                if raw.startswith("127.") or raw in {"0.0.0.0", "255.255.255.255"}:
                    continue
            if label == "Email address" and raw.lower().endswith(("example.com", "example.org", "example.net")):
                continue
            line = text[:match.start()].count("\n") + 1 if offsets_are_lines else 0
            findings.append({"severity": "MEDIUM" if label != "Absolute Unix path" else "HIGH",
                             "category": label, "value": raw[:160], "line": line, "path": rel,
                             "note": "Review whether this information should be published."})

    for token in personal_tokens:
        if len(token) < 3:
            continue
        if token in text.lower():
            occurrences = text.lower().count(token)
            findings.append({"severity": "MEDIUM", "category": "Local username/hostname",
                             "value": token, "line": 0, "path": rel,
                             "note": f"Token matched {occurrences} time(s)."})

    return findings, size


def main():
    parser = argparse.ArgumentParser(description="Scan working-tree files for secrets and personal information")
    parser.add_argument("root", nargs="?", default=".", help="Directory to scan (default: current directory)")
    parser.add_argument("--json", metavar="FILE", help="Also write machine-readable JSON to FILE")
    parser.add_argument("--exclude", action="append", default=[], metavar="DIR_OR_GLOB", help="Exclude matching path/component (repeatable)")
    parser.add_argument("--max-file-mb", type=float, default=50, help="Skip files larger than this size (default: 50 MB)")
    args = parser.parse_args()

    root = Path(args.root).expanduser().resolve()
    if not root.is_dir():
        print(f"error: scan root is not a directory: {root}", file=sys.stderr)
        return 2

    personal_tokens = local_personal_tokens(root)
    excludes = list(args.exclude)
    all_findings = []
    file_count = 0
    scanned_bytes = 0
    max_bytes = int(args.max_file_mb * 1048576)

    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if should_exclude(path, root, excludes):
            continue
        file_count += 1
        findings, size = scan_file(path, root, personal_tokens, max_bytes)
        scanned_bytes += min(size, max_bytes)
        all_findings.extend(findings)

    order = {"HIGH": 0, "MEDIUM": 1, "LOW": 2}
    all_findings.sort(key=lambda item: (order[item["severity"]], item["path"], item["line"], item["category"], item["value"]))

    print(f"Personal-information scan: {root}")
    print(f"Files visited: {file_count} | Approx. bytes scanned: {scanned_bytes} | Findings: {len(all_findings)}")
    current_path = None
    for item in all_findings:
        if item["path"] != current_path:
            current_path = item["path"]
            print(f"\n{current_path}")
        line = f":{item['line']}" if item["line"] else ""
        print(f"  [{item['severity']}]{line} {item['category']}: {item['value']} - {item['note']}")

    if args.json:
        report_path = Path(args.json).expanduser().resolve()
        report_path.write_text(json.dumps({
            "root": str(root), "files_visited": file_count, "bytes_scanned": scanned_bytes,
            "host_tokens": sorted(personal_tokens), "findings": all_findings,
        }, indent=2) + "\n")
        print(f"\nJSON report: {report_path}")

    return 1 if any(f["severity"] == "HIGH" for f in all_findings) else 0

if __name__ == "__main__":
    raise SystemExit(main())
