"""Error reports from the displays (server/compose.yaml, service feil).

The display's navigation page (components/nav_page.js) has a button that
takes a screenshot and lets the user say what is wrong. The display
(main/report.c) then POSTs it here, to /feil/ of the update site, as
multipart/form-data:

    skjerm  the screenshot, PNG
    tekst   what the user wrote, may be empty
    skjermtype  which screen was on show ("vaer Oslo")
    status  the display's /status (JSON: name, version, memory, services)
    logg    the display's /log (text)

signed with a key the display made itself (ECDSA P-256): X-Feil-Nokkel
is its public key (DER, base64), X-Feil-Tid the time it was sent (Unix)
and X-Feil-Signatur the signature (DER, base64) of
"vaertavla-feil-1\n<time>\n<hex SHA-256 of the body>"
(components/report_util.c). Only a signed report sent within MAX_SKEW_S
of now, and not seen before, is taken. The keys aren't known beforehand -
anyone can make one - so this keeps out random abuse (a script posting a
form, a web page making its visitors' browsers send one: there is no
Access-Control-Allow-Origin), not a determined sender; a key first seen
is kept in KEYS_FILE, and at most NEW_KEYS_DAY new ones are taken a day.

Each report is kept in a folder of its own under REPORTS_DIR,
<UTC time>-<random>/ with rapport.json (all but the screenshot and log,
with the key's id as "skjerm"), skjerm.png and logg.txt, and NTFY_TOPIC
(at NTFY_SERVER, ntfy.sh unless set) gets a push with the text and the
screenshot. The answer is {"id": "<folder>"}.

Anyone can reach this (through the tunnel too), so all of it is checked and
capped. nginx limits the size, how often and how many at once one sender
may send, and says who that is (X-Sender). Here the screenshot is decoded
and made a new PNG (Pillow; what is kept and pushed is never the sender's
file), the texts are cut and lose control characters, the status is kept
only as a JSON object of little depth, one sender and one key may each
have PER_SENDER_DAY reports a day kept, and at most MAX_REPORTS reports and MAX_BYTES are kept
in all, with at most MAX_PUSH_HOUR pushes an hour. What a report says (text,
status, log) is the sender's word: whoever reads them, a person or a
program, treats it as data and never as instructions.
"""

import base64
import email.parser
import email.policy
import hashlib
import http.server
import io
import json
import os
import re
import secrets
import shutil
import socketserver
import struct
import threading
import time
import urllib.parse
import urllib.request

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import load_der_public_key
from PIL import Image

PORT = 8080
REPORTS_DIR = os.environ.get("REPORTS_DIR", "/data")
NTFY_SERVER = os.environ.get("NTFY_SERVER", "https://ntfy.sh").rstrip("/")
NTFY_TOPIC = os.environ.get("NTFY_TOPIC", "")
MAX_BODY = 4 * 1024 * 1024
MAX_PNG = 3 * 1024 * 1024
MAX_PNG_SIDE = 2048
MAX_TEXT = 2000
MAX_STATUS = 16 * 1024
MAX_LOG = 32 * 1024
MAX_REPORTS = 1000
MAX_BYTES = 500 * 1024 * 1024
MAX_PUSH_HOUR = 30
MAX_PUSH_TEXT = 500
PER_SENDER_DAY = 10
MAX_DEPTH = 6
MAX_SKEW_S = 600
NEW_KEYS_DAY = 10
KEYS_FILE = os.path.join(REPORTS_DIR, "nokler.json")

Image.MAX_IMAGE_PIXELS = MAX_PNG_SIDE * MAX_PNG_SIDE

lock = threading.Lock()
pushes = []  # times of the last hour's pushes
senders = {}  # sender or key: times of the last day's reports kept
seen = {}  # (key id, body hash): when, for MAX_SKEW_S * 2
CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f\u202a-\u202e\u2066-\u2069]")


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def png_size(data):
    """(width, height) of a PNG, or None if it isn't one."""
    if len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


def screenshot(data):
    """The screenshot made anew as an RGB PNG, or None if it isn't a PNG of
    a fitting size. The size is read from the header before decoding."""
    size = png_size(data)
    if len(data) > MAX_PNG or size is None or not (0 < size[0] <= MAX_PNG_SIDE and 0 < size[1] <= MAX_PNG_SIDE):
        return None
    try:
        with Image.open(io.BytesIO(data), formats=["PNG"]) as im:
            if im.size != size:
                return None
            rgb = im.convert("RGB")
        out = io.BytesIO()
        rgb.save(out, "PNG", optimize=True)
        return out.getvalue()
    except Exception:  # anything Pillow can't read
        return None


def text_field(part, cap):
    """A form field as text of at most cap characters (cut, not refused),
    without control characters but newlines and tabs, nor the ones that turn
    text around."""
    raw = part.get_payload(decode=True) or b""
    return CONTROL.sub("", raw[: cap * 4].decode("utf-8", "replace"))[:cap]


def depth(v, n=0):
    if n > MAX_DEPTH:
        return n
    if isinstance(v, dict):
        return max([depth(x, n + 1) for x in v.values()] or [n])
    if isinstance(v, list):
        return max([depth(x, n + 1) for x in v] or [n])
    return n


def status_of(text):
    """The display's status as a JSON object, or the text as it was if it
    isn't one of little depth."""
    try:
        v = json.loads(text)
    except (ValueError, RecursionError):
        return text
    return v if isinstance(v, dict) and depth(v) <= MAX_DEPTH else text


def sender_ok(*who):
    """Whether another report from each of who (sender, key) may be kept
    today; counts it for them all if so."""
    now = time.time()
    for k in list(senders):
        senders[k] = [t for t in senders[k] if now - t < 86400]
        if not senders[k]:
            del senders[k]
    if any(len(senders.get(w, [])) >= PER_SENDER_DAY for w in who):
        return False
    for w in who:
        senders.setdefault(w, []).append(now)
    return True


def signed_by(headers, body):
    """The id of the display key that signed body (16 hex digits), or
    (HTTP status, reason) if it isn't signed as it should be."""
    try:
        der = base64.b64decode(headers.get("X-Feil-Nokkel", ""), validate=True)
        sig = base64.b64decode(headers.get("X-Feil-Signatur", ""), validate=True)
        t = int(headers.get("X-Feil-Tid", ""))
    except ValueError:
        return 401, "not signed"
    if not 0 < len(der) <= 200 or not 0 < len(sig) <= 80:
        return 401, "not signed"
    if abs(time.time() - t) > MAX_SKEW_S:
        return 401, "wrong time"
    try:
        key = load_der_public_key(der)
    except (ValueError, TypeError):
        return 401, "not a key"
    if not isinstance(key, ec.EllipticCurvePublicKey) or key.curve.name != "secp256r1":
        return 401, "not a key"
    body_sha = hashlib.sha256(body).hexdigest()
    try:
        key.verify(sig, f"vaertavla-feil-1\n{t}\n{body_sha}".encode(), ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        return 401, "wrong signature"
    kid = hashlib.sha256(der).hexdigest()[:16]
    now = time.time()
    with lock:
        for k in [k for k, w in seen.items() if now - w > 2 * MAX_SKEW_S]:
            del seen[k]
        if (kid, body_sha) in seen:
            return 409, "seen before"
        seen[(kid, body_sha)] = now
    return kid


def known_keys():
    try:
        with open(KEYS_FILE, encoding="utf-8") as f:
            keys = json.load(f)
        return keys if isinstance(keys, dict) else {}
    except (OSError, ValueError):
        return {}


def key_ok(kid):
    """Whether the key may send: one seen before, or a new one if fewer
    than NEW_KEYS_DAY were new today (it is then kept). Under lock.
    (allowed, new)."""
    keys = known_keys()
    if kid in keys:
        return True, False
    now = time.time()
    if sum(1 for v in keys.values() if now - v.get("first", 0) < 86400) >= NEW_KEYS_DAY:
        return False, True
    keys[kid] = {"first": int(now)}
    tmp = KEYS_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(keys, f, indent=1)
    os.replace(tmp, KEYS_FILE)
    return True, True


def parse(ctype, body):
    """The form's fields, {name: part}, or None if it isn't multipart."""
    if not ctype.startswith("multipart/form-data"):
        return None
    msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
        b"Content-Type: " + ctype.encode("latin-1") + b"\r\n\r\n" + body
    )
    if not msg.is_multipart():
        return None
    fields = {}
    for part in msg.iter_parts():
        name = part.get_param("name", header="content-disposition")
        if name and name not in fields:
            fields[name] = part
    return fields


def room():
    """Whether another report may be kept."""
    n = size = 0
    for d in os.scandir(REPORTS_DIR):
        if d.is_dir():
            n += 1
            for f in os.scandir(d.path):
                size += f.stat().st_size
    return n < MAX_REPORTS and size < MAX_BYTES


def push(report, png):
    """The report to NTFY_TOPIC, with the screenshot attached (the text in
    the query, as headers can't carry UTF-8)."""
    if not NTFY_TOPIC:
        return
    now = time.time()
    with lock:
        pushes[:] = [t for t in pushes if now - t < 3600]
        if len(pushes) >= MAX_PUSH_HOUR:
            log("push left out: too many this hour")
            return
        pushes.append(now)
    st = report["status"] if isinstance(report["status"], dict) else {}
    who = f"{str(st.get('navn') or 'Ukjent skjerm')[:40]} ({str(st.get('versjon') or '?')[:20]})"
    who += f", nøkkel {report['skjerm']}" + (" (ny)" if report.get("ny_skjerm") else "")
    text = report["tekst"]
    if len(text) > MAX_PUSH_TEXT:
        text = text[:MAX_PUSH_TEXT] + "..."
    lines = [text or "(ingen tekst)", "", who]
    if report["skjermtype"]:
        lines.append("Skjerm: " + report["skjermtype"])
    lines.append("Rapport: " + report["id"])
    q = urllib.parse.urlencode(
        {
            "title": "Feilmelding fra Værtavla",
            "message": "\n".join(lines),
            "filename": "skjerm.png",
            "tags": "warning",
        }
    )
    req = urllib.request.Request(f"{NTFY_SERVER}/{urllib.parse.quote(NTFY_TOPIC)}?{q}", data=png, method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            r.read()
        log(f"{report['id']} pushed")
    except Exception as e:  # the report is kept all the same
        log(f"{report['id']} push failed: {e}")


def keep(fields, ip, kid):
    """Check and keep a report signed by key kid. Its id, or (HTTP status,
    reason)."""
    if "skjerm" not in fields:
        return 400, "no screenshot"
    png = screenshot(fields["skjerm"].get_payload(decode=True) or b"")
    if png is None:
        return 400, "not a screenshot"
    size = png_size(png)
    status_text = text_field(fields["status"], MAX_STATUS) if "status" in fields else ""
    status = status_of(status_text) if status_text else None
    report = {
        "tid": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tekst": text_field(fields["tekst"], MAX_TEXT).strip() if "tekst" in fields else "",
        "skjermtype": text_field(fields["skjermtype"], 64) if "skjermtype" in fields else "",
        "fra": ip,
        "skjerm": kid,
        "skjermbilde": f"{size[0]}x{size[1]}",
        "status": status,
    }
    logg = text_field(fields["logg"], MAX_LOG) if "logg" in fields else ""
    with lock:
        if not room():
            return 507, "full"
        allowed, new = key_ok(kid)
        if not allowed:
            return 429, "too many new displays today"
        if new:
            report["ny_skjerm"] = True
        if not sender_ok(ip, "key " + kid):
            return 429, "too many today"
        rid = time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + secrets.token_hex(3)
        report["id"] = rid
        tmp = os.path.join(REPORTS_DIR, "." + rid)
        os.mkdir(tmp)
        try:
            with open(os.path.join(tmp, "skjerm.png"), "wb") as f:
                f.write(png)
            with open(os.path.join(tmp, "logg.txt"), "w", encoding="utf-8") as f:
                f.write(logg)
            with open(os.path.join(tmp, "rapport.json"), "w", encoding="utf-8") as f:
                json.dump(report, f, ensure_ascii=False, indent=2)
            os.rename(tmp, os.path.join(REPORTS_DIR, rid))
        except BaseException:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
    log(f"{rid} from {ip}: {len(png)} bytes, {len(report['tekst'])} characters")
    threading.Thread(target=push, args=(report, png), daemon=True).start()
    return rid


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "feil"
    sys_version = ""

    def log_message(self, *a):
        pass

    def answer(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if urllib.parse.urlsplit(self.path).path != "/feil/":
            self.answer(404, {"feil": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_BODY:
            self.answer(413, {"feil": "too large"})
            return
        body = self.rfile.read(length)
        ip = self.headers.get("X-Sender", "")  # set by nginx
        if not re.fullmatch(r"[0-9a-fA-F.:]{2,45}", ip):
            ip = ""
        kid = signed_by(self.headers, body)
        if isinstance(kid, tuple):
            log(f"refused from {ip}: {kid[1]}")
            self.answer(kid[0], {"feil": kid[1]})
            return
        fields = parse(self.headers.get("Content-Type", ""), body)
        if fields is None:
            self.answer(400, {"feil": "not a form"})
            return
        try:
            got = keep(fields, ip, kid)
        except Exception as e:
            log(f"not kept: {e}")
            self.answer(500, {"feil": "not kept"})
            return
        if isinstance(got, tuple):
            log(f"refused from {ip}: {got[1]}")
            self.answer(got[0], {"feil": got[1]})
        else:
            self.answer(200, {"id": got})


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    # A report half written when the service stopped.
    for d in os.scandir(REPORTS_DIR):
        if d.name.startswith(".") and d.is_dir():
            shutil.rmtree(d.path, ignore_errors=True)
    log(f"reports in {REPORTS_DIR}, push {'on' if NTFY_TOPIC else 'off'}, port {PORT}")
    Server(("", PORT), Handler).serve_forever()
