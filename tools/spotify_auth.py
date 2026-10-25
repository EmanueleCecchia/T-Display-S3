"""Log in to Spotify once and store the device credentials in secrets.h.

Uses the Authorization Code with PKCE flow, so no Client Secret is needed or stored.

Setup (once):
  1. https://developer.spotify.com/dashboard -> your app -> Settings
  2. Add the Redirect URI:  http://127.0.0.1:8888/callback
  3. Copy the app's Client ID

Run from the repo root:
  python tools/spotify_auth.py

It writes SPOTIFY_CLIENT_ID and SPOTIFY_REFRESH_TOKEN into secrets.h (gitignored).
Refresh tokens expire after 6 months: run this again when the display asks you to.
"""
import base64
import hashlib
import http.server
import json
import re
import secrets
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from pathlib import Path

REDIRECT_HOST = "127.0.0.1"
REDIRECT_PORT = 8888
REDIRECT_URI = f"http://{REDIRECT_HOST}:{REDIRECT_PORT}/callback"
# Read playback state and the queue (up next) + skip/play/pause. Nothing else.
SCOPES = "user-read-playback-state user-read-currently-playing user-modify-playback-state"
SECRETS_H = Path(__file__).resolve().parent.parent / "secrets.h"


def read_define(text, name):
    m = re.search(rf'^#define\s+{name}\s+"([^"]*)"', text, re.M)
    return m.group(1) if m else ""


def set_define(text, name, value):
    line = f'#define {name:<28} "{value}"'
    pattern = rf'^#define\s+{name}\s+"[^"]*".*$'
    if re.search(pattern, text, re.M):
        return re.sub(pattern, lambda _: line, text, flags=re.M)
    return text.rstrip("\n") + "\n" + line + "\n"


def wait_for_code(expected_state):
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != "/callback":
                self.send_error(404)
                return
            query = urllib.parse.parse_qs(url.query)
            if query.get("state", [""])[0] != expected_state:
                result["error"] = "state mismatch"
            elif "error" in query:
                result["error"] = query["error"][0]
            else:
                result["code"] = query.get("code", [""])[0]
            ok = "code" in result
            self.send_response(200 if ok else 400)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            msg = "Spotify login done, you can close this tab." if ok else f"Login failed: {result['error']}"
            self.wfile.write(f"<html><body><h2>{msg}</h2></body></html>".encode())

        def log_message(self, *args):
            pass

    with http.server.HTTPServer((REDIRECT_HOST, REDIRECT_PORT), Handler) as server:
        while not result:
            server.handle_request()
    if "error" in result:
        sys.exit(f"Login failed: {result['error']}")
    return result["code"]


def request_token(fields):
    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode(fields).encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        sys.exit(f"Token request failed ({e.code}): {body}")


def main():
    text = SECRETS_H.read_text(encoding="utf-8") if SECRETS_H.exists() else ""

    client_id = read_define(text, "SPOTIFY_CLIENT_ID")
    hint = f" [{client_id[:6]}...]" if client_id else ""
    client_id = input(f"Spotify Client ID{hint}: ").strip() or client_id
    if not re.fullmatch(r"[0-9a-fA-F]{32}", client_id):
        sys.exit("That doesn't look like a Client ID (32 hex characters).")

    verifier = secrets.token_urlsafe(64)  # 86 chars of [A-Za-z0-9_-]
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        "scope": SCOPES,
        "state": state,
    })

    print("\nOpening the Spotify login page. If it doesn't open, visit:\n" + auth_url + "\n")
    webbrowser.open(auth_url)
    code = wait_for_code(state)

    token = request_token({
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT_URI,
        "client_id": client_id,
        "code_verifier": verifier,
    })
    refresh_token = token.get("refresh_token")
    if not refresh_token:
        sys.exit("Spotify did not return a refresh token.")

    text = set_define(text, "SPOTIFY_CLIENT_ID", client_id)
    text = set_define(text, "SPOTIFY_REFRESH_TOKEN", refresh_token)
    SECRETS_H.write_text(text, encoding="utf-8")
    print(f"Saved SPOTIFY_CLIENT_ID and SPOTIFY_REFRESH_TOKEN to {SECRETS_H}")
    print("Now build and flash the 'spotify' environment.")


if __name__ == "__main__":
    main()
