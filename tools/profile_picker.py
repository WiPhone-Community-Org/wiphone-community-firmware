#!/usr/bin/env python3
import html
import json
import subprocess
import sys
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parent.parent
SELECTOR = ROOT / "tools" / "select_build_profile.py"


def load_manifests(kind):
    entries = []
    for manifest_path in sorted((ROOT / kind).glob("*/manifest.json")):
        try:
            data = json.loads(manifest_path.read_text())
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"Invalid JSON in {manifest_path}: {exc}") from exc
        entries.append(data)
    return entries


def render_page(apps, settings, error=None, success=None):
    if error:
        notice = (
            '<div class="notice error"><strong>Profile not prepared.</strong><br>'
            + html.escape(error)
            + "</div>"
        )
    elif success:
        notice = (
            '<div class="notice success"><strong>Profile prepared.</strong><br>'
            + html.escape(success)
            + "</div>"
        )
    else:
        notice = ""

    app_items = "".join(
        f'<label class="choice"><input type="checkbox" name="apps" value="{html.escape(entry["id"])}">'
        f'<span><strong>{html.escape(entry.get("title", entry["id"]))}</strong>'
        f'<small>{html.escape(entry["id"])}</small></span></label>'
        for entry in apps
    )
    setting_items = "".join(
        f'<label class="choice"><input type="checkbox" name="settings" value="{html.escape(entry["id"])}">'
        f'<span><strong>{html.escape(entry.get("title", entry["id"]))}</strong>'
        f'<small>{html.escape(entry["id"])}</small></span></label>'
        for entry in settings
    )

    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>WiPhone Profile Builder</title>
  <style>
    :root {{
      color-scheme: light;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: #eef1f5;
      color: #20242a;
    }}
    * {{ box-sizing: border-box; }}
    body {{ margin: 0; }}
    main {{ max-width: 980px; margin: 0 auto; padding: 28px 20px 40px; }}
    h1 {{ margin: 0 0 6px; font-size: 28px; }}
    .subtitle {{ margin: 0 0 22px; color: #626b75; }}
    .notice {{
      margin-bottom: 16px; padding: 12px 14px; border-radius: 8px;
      border: 1px solid; line-height: 1.45;
    }}
    .notice.success {{ background: #edf9f0; border-color: #9bd2a7; }}
    .notice.error {{ background: #fff0f0; border-color: #e3a0a0; }}
    .toolbar {{
      display: flex; align-items: center; gap: 12px; margin-bottom: 16px;
      padding: 14px; background: white; border: 1px solid #ccd3dc; border-radius: 10px;
    }}
    label.name {{ font-weight: 700; }}
    input[type=text] {{ height: 38px; width: 260px; padding: 7px 10px; font-size: 16px; }}
    .columns {{ display: grid; grid-template-columns: 2fr 1fr; gap: 16px; }}
    fieldset {{
      min-width: 0; margin: 0; padding: 16px; background: white;
      border: 1px solid #ccd3dc; border-radius: 10px;
    }}
    legend {{ padding: 0 7px; font-weight: 750; }}
    .choices {{ display: grid; grid-template-columns: 1fr; gap: 8px; }}
    .choice {{
      display: flex; align-items: center; gap: 10px; padding: 10px 12px;
      border: 1px solid #e1e4e8; border-radius: 7px; cursor: pointer;
    }}
    .choice:hover {{ background: #f7f9fc; border-color: #aab4c2; }}
    .choice input {{ width: 18px; height: 18px; }}
    .choice span {{ display: flex; flex-direction: column; }}
    .choice small {{ margin-top: 2px; color: #737b85; }}
    .footer {{
      display: flex; align-items: center; justify-content: space-between;
      margin-top: 16px; padding: 14px; background: white;
      border: 1px solid #ccd3dc; border-radius: 10px;
    }}
    .targets {{ display: flex; align-items: center; gap: 18px; }}
    .targets label {{ cursor: pointer; }}
    button {{
      height: 40px; padding: 0 18px; border-radius: 7px; font-weight: 700;
      border: 1px solid #2563eb; cursor: pointer;
    }}
    button.primary {{ background: #2563eb; color: white; }}
    @media (max-width: 760px) {{
      .columns {{ grid-template-columns: 1fr; }}
      .toolbar, .footer {{ align-items: flex-start; flex-direction: column; }}
      input[type=text] {{ width: 100%; }}
    }}
  </style>
</head>
<body>
  <main>
    <h1>Custom WiPhone Profile</h1>
    <p class="subtitle">Choose optional applications and feature modules, then prepare a build profile.</p>
    {notice}
    <form method="post" action="/prepare">
      <div class="toolbar">
        <label class="name" for="name">Profile name</label>
        <input id="name" name="name" type="text" value="custom" pattern="[A-Za-z0-9_-]+" required>
      </div>
      <div class="columns">
        <fieldset>
          <legend>Optional apps</legend>
          <div class="choices">{app_items}</div>
        </fieldset>
        <fieldset>
          <legend>Feature settings</legend>
          <div class="choices">{setting_items}</div>
        </fieldset>
      </div>
      <div class="footer">
        <div class="targets">
          <strong>Build target</strong>
          <label><input type="radio" name="target" value="platformio" checked> PlatformIO</label>
          <label><input type="radio" name="target" value="arduino"> Arduino Community Extension</label>
        </div>
        <button class="primary" type="submit">Prepare Profile</button>
      </div>
    </form>
  </main>
</body>
</html>"""


class ProfilePickerHandler(BaseHTTPRequestHandler):
    apps = load_manifests("apps")
    settings = load_manifests("settings")

    def log_message(self, format_string, *args):
        return

    def send_page(self, status=200, error=None, success=None):
        body = render_page(self.apps, self.settings, error=error, success=success).encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if urlparse(self.path).path != "/":
            self.send_error(404)
            return
        self.send_page()

    def do_POST(self):
        if urlparse(self.path).path != "/prepare":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", "0"))
        form = parse_qs(self.rfile.read(length).decode(), keep_blank_values=True)
        name = form.get("name", ["custom"])[0].strip()
        apps = form.get("apps", [])
        settings = form.get("settings", [])
        target = form.get("target", ["platformio"])[0]
        command = [
            sys.executable,
            str(SELECTOR),
            "--name",
            name,
            "--apps",
            ",".join(apps),
            "--settings",
            ",".join(settings),
            "--target",
            target,
        ]
        try:
            result = subprocess.run(
                command,
                cwd=str(ROOT),
                check=True,
                capture_output=True,
                text=True,
            )
        except subprocess.CalledProcessError as exc:
            self.send_page(
                status=400,
                error=exc.stderr.strip() or str(exc),
            )
            return
        self.send_page(
            success=(
                f"{name}: apps={', '.join(apps) or '(none)'}, "
                f"settings={', '.join(settings) or '(none)'}, "
                f"output={result.stdout.strip()}"
            )
        )


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), ProfilePickerHandler)
    port = server.server_address[1]
    url = f"http://127.0.0.1:{port}/"
    print(f"Opening WiPhone Profile Builder: {url}", flush=True)
    try:
        webbrowser.open(url, new=1, autoraise=True)
    except Exception as exc:
        print(f"Could not automatically open the browser: {exc}", file=sys.stderr)
        print(f"Open this URL manually: {url}", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
