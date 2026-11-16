"""Pictures shared between displays (server/compose.yaml, service deling).

A display (main/share.c) shares a picture from its memory card with a
group, and the displays in that group download it onto theirs. Everything
is at /deling/ of the update site, through nginx:

    GET  meg?navn=<name>&kallenavn=<nickname>
                              {"status": "venter"|"godkjent"|"avvist",
                               "grupper": [...]}; a key not seen before
                               is kept as "venter" (waiting) and pushed;
                               the nickname is shown with what it shares,
                               and "opptatt": true says an approved display
                               has it already (it is then not taken)
    GET  bilder?grupper=a,b   {"navn": [...], "bilder": [{"id", "gruppe",
                               "mitt": true, "fra": n}]}: the pictures in
                               those of the display's groups (all of them
                               if none are named); mitt only on its own,
                               fra only where the sharer has a nickname,
                               navn[n]. Without spaces: a display takes at
                               most 256 KB (share.c)
    GET  mine                 {"bilder": [{"id", "gruppe", "navn"}]}: what
                               the display itself shares
    GET  bilde?id=<id>        the picture (JPEG)
    POST del?gruppe=g&navn=n  a JPEG as the body, shared with group g:
                               {"id": "<12 hex digits>"}
    POST slett?id=<id>        one the display shared, no longer shared

Only displays someone has approved here may share or download. Every
request is signed with the display's own key (ECDSA P-256, made on the
display, which never lets go of the private half): X-Deling-Nokkel is its
public key (DER, base64), X-Deling-Tid the time (Unix), X-Deling-Nonce a
one-off hex string and X-Deling-Signatur the signature (DER, base64) of

    "vaertavla-deling-1\\n<method>\\n<path>\\n<time>\\n<nonce>\\n<hex SHA-256 of the body>"

where path is what follows /deling/, query and all
(components/share_util.c). A request is taken only if the signature holds,
it is at most MAX_SKEW_S old and its nonce hasn't been seen, and - but for
meg - the key is approved and in the group. The firmware is public, so
there is no secret in it to prove a display is one; approving each key here
is what keeps everyone else out. Approving, the groups and removing are
done on the Pi with the same file:

    docker compose exec deling python -I /app/deling.py skjermer
    docker compose exec deling python -I /app/deling.py godkjenn <nøkkel> familien,hytta
    ... grupper <nøkkel> <grupper>   avvis <nøkkel>   glem <nøkkel>
    ... bilder [gruppe]              slett <id>       slett-fra <nøkkel>

Kept in DATA_DIR: skjermer.json (the keys: name, nickname, status, groups,
first seen), bilder.json (the pictures: group, who shared it, its name on that
display's card, when) and bilder/<id>.jpg. Each picture is decoded and
made anew (Pillow), upright, at most 800x480 and without its metadata
(where a photo was taken stays with whoever took it); what is kept and
sent is never the sender's file. Capped: MAX_PICTURES and MAX_BYTES in all,
PER_KEY_DAY shared by one display a day, NEW_KEYS_DAY new keys a day;
nginx limits how often one sender may ask.
"""

import base64
import contextlib
import datetime
import fcntl
import hashlib
import html
import http.server
import io
import json
import os
import re
import secrets
import socketserver
import sys
import threading
import time
import unicodedata
import urllib.parse
import urllib.request

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import load_der_public_key
from PIL import Image, ImageOps

PORT = int(os.environ.get("PORT", "8080"))
DATA_DIR = os.environ.get("DATA_DIR", "/data")
NTFY_SERVER = os.environ.get("NTFY_SERVER", "https://ntfy.sh").rstrip("/")
NTFY_TOPIC = os.environ.get("NTFY_TOPIC", "")
KEYS_FILE = os.path.join(DATA_DIR, "skjermer.json")
INDEX_FILE = os.path.join(DATA_DIR, "bilder.json")
PICS_DIR = os.path.join(DATA_DIR, "bilder")
LOCK_FILE = os.path.join(DATA_DIR, ".laas")
MAX_BODY = 2 * 1024 * 1024
MAX_PIXELS = 40_000_000
OUT_W, OUT_H = 800, 480
MAX_PICTURES = 3000  # the display lists at most this many (share.c)
MAX_BYTES = 2 * 1024 * 1024 * 1024
PER_KEY_DAY = 100
NEW_KEYS_DAY = 10
MAX_SKEW_S = 600
GROUP = re.compile(r"[a-z0-9_-]{1,24}")
PIC_ID = re.compile(r"[0-9a-f]{12}")
KEY_ID = re.compile(r"[0-9a-f]{16}")
CONTROL = re.compile(r"[\x00-\x1f\x7f‪-‮⁦-⁩]")

Image.MAX_IMAGE_PIXELS = MAX_PIXELS

tlock = threading.Lock()
seen = {}  # (key id, nonce): when, for MAX_SKEW_S * 2
shared_today = {}  # key id: times of the last day's shares


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


@contextlib.contextmanager
def locked():
    """The data, for this thread and from the admin commands alike."""
    with tlock, open(LOCK_FILE, "a") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            v = json.load(f)
        return v if isinstance(v, dict) else {}
    except (OSError, ValueError):
        return {}


def save(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


def clean_text(s, cap):
    return CONTROL.sub("", s)[:cap]


def nick_key(nick):
    """What two nicknames that read the same have in common."""
    return "".join(unicodedata.normalize("NFKC", nick).casefold().split())


def nick_taken(keys, kid, nick):
    """Whether an approved display other than kid goes by `nick`."""
    want = nick_key(nick)
    return bool(want) and any(
        k != kid and v.get("status") == "godkjent" and nick_key(v.get("kallenavn", "")) == want for k, v in keys.items()
    )


def signed_text(method, path, t, nonce, body):
    return f"vaertavla-deling-1\n{method}\n{path}\n{t}\n{nonce}\n{hashlib.sha256(body).hexdigest()}".encode()


def signed_by(headers, method, path, body):
    """(key id, public key in base64) of the display that signed the
    request, or (HTTP status, reason) if it isn't signed as it should be."""
    key_b64 = headers.get("X-Deling-Nokkel", "")
    nonce = headers.get("X-Deling-Nonce", "")
    try:
        der = base64.b64decode(key_b64, validate=True)
        sig = base64.b64decode(headers.get("X-Deling-Signatur", ""), validate=True)
        t = int(headers.get("X-Deling-Tid", ""))
    except ValueError:
        return 401, "not signed"
    if not 0 < len(der) <= 200 or not 0 < len(sig) <= 80 or not re.fullmatch(r"[0-9a-f]{16,64}", nonce):
        return 401, "not signed"
    if abs(time.time() - t) > MAX_SKEW_S:
        return 401, "wrong time"
    try:
        key = load_der_public_key(der)
    except (ValueError, TypeError):
        return 401, "not a key"
    if not isinstance(key, ec.EllipticCurvePublicKey) or key.curve.name != "secp256r1":
        return 401, "not a key"
    try:
        key.verify(sig, signed_text(method, path, t, nonce, body), ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        return 401, "wrong signature"
    kid = hashlib.sha256(der).hexdigest()[:16]
    now = time.time()
    with tlock:
        for k in [k for k, w in seen.items() if now - w > 2 * MAX_SKEW_S]:
            del seen[k]
        if (kid, nonce) in seen:
            return 409, "seen before"
        seen[(kid, nonce)] = now
    return kid, key_b64


def push(title, text):
    if not NTFY_TOPIC:
        return
    q = urllib.parse.urlencode({"title": title, "tags": "frame_with_picture"})
    req = urllib.request.Request(
        f"{NTFY_SERVER}/{urllib.parse.quote(NTFY_TOPIC)}?{q}", data=text.encode(), method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            r.read()
    except Exception as e:  # noqa: BLE001 - only a push
        log(f"push failed: {e}")


def picture(data):
    """The picture made anew: upright, at most OUT_W x OUT_H, RGB JPEG
    without metadata. None if it isn't a JPEG Pillow reads."""
    try:
        with Image.open(io.BytesIO(data), formats=["JPEG"]) as im:
            if im.width * im.height > MAX_PIXELS:
                return None
            im = ImageOps.exif_transpose(im).convert("RGB")
        im.thumbnail((OUT_W, OUT_H), Image.Resampling.LANCZOS)
        out = io.BytesIO()
        im.save(out, "JPEG", quality=88, optimize=True)
        return out.getvalue()
    except Exception:  # noqa: BLE001 - anything Pillow can't read
        return None


def pic_path(pid):
    return os.path.join(PICS_DIR, pid + ".jpg")


def remove_picture(index, pid):
    index.pop(pid, None)
    with contextlib.suppress(FileNotFoundError):
        os.remove(pic_path(pid))


# ---- The displays' requests ----


def me(kid, key_b64, q):
    navn = clean_text(q.get("navn", [""])[0], 40) or "Ukjent skjerm"
    nick = " ".join(clean_text(q.get("kallenavn", [""])[0], 40).split())
    taken = False
    with locked():
        keys = load(KEYS_FILE)
        k = keys.get(kid)
        new = k is None
        # One an approved display has stays that display's: the one this
        # had is kept (unless that is taken too, by now).
        if nick_taken(keys, kid, nick):
            taken = True
            nick = "" if new else k.get("kallenavn", "")
            if nick_taken(keys, kid, nick):
                nick = ""
        if new:
            now = int(time.time())
            if sum(1 for v in keys.values() if now - v.get("forst", 0) < 86400) >= NEW_KEYS_DAY:
                return 429, {"feil": "too many new displays today"}
            k = keys[kid] = {"navn": navn, "kallenavn": nick, "status": "venter", "grupper": [], "forst": now,
                             "nokkel": key_b64}
            save(KEYS_FILE, keys)
        elif k.get("navn") != navn or k.get("kallenavn", "") != nick:
            k["navn"] = navn
            k["kallenavn"] = nick
            save(KEYS_FILE, keys)
    if new:
        log(f"new display {kid} ({navn}) waits")
        threading.Thread(
            target=push,
            args=(
                "Ny skjerm vil dele bilder",
                f"«{navn}» (nøkkel {kid}) vil dele bilder. Godkjenn den på /deling/admin/ på oppdateringssiden.",
            ),
            daemon=True,
        ).start()
    status = k.get("status", "venter")
    out = {"status": status, "grupper": k.get("grupper", []) if status == "godkjent" else []}
    if taken:
        out["opptatt"] = True
    return 200, out


def approved(kid):
    """The display's groups if it is approved, else None."""
    k = load(KEYS_FILE).get(kid)
    return list(k.get("grupper", [])) if k and k.get("status") == "godkjent" else None


def list_pictures(kid, q):
    member = approved(kid)
    if member is None:
        return 403, {"feil": "not approved"}
    asked = [g for g in q.get("grupper", [""])[0].split(",") if g]
    groups = set(member) & set(asked) if asked else set(member)
    index = load(INDEX_FILE)
    keys = load(KEYS_FILE)
    names, at, out = [], {}, []
    for pid, p in sorted(index.items(), key=lambda kv: kv[1].get("tid", 0)):
        if p.get("gruppe") not in groups:
            continue
        e = {"id": pid, "gruppe": p["gruppe"]}
        if p.get("fra") == kid:
            e["mitt"] = True
        nick = keys.get(p.get("fra"), {}).get("kallenavn", "")
        if nick:
            if nick not in at:
                at[nick] = len(names)
                names.append(nick)
            e["fra"] = at[nick]
        out.append(e)
    return 200, {"navn": names, "bilder": out}


def my_pictures(kid):
    if approved(kid) is None:
        return 403, {"feil": "not approved"}
    index = load(INDEX_FILE)
    out = [
        {"id": pid, "gruppe": p["gruppe"], "navn": p.get("navn", "")}
        for pid, p in sorted(index.items(), key=lambda kv: kv[1].get("tid", 0))
        if p.get("fra") == kid
    ]
    return 200, {"bilder": out}


def get_picture(kid, q):
    """(status, JPEG bytes or an error object)."""
    member = approved(kid)
    if member is None:
        return 403, {"feil": "not approved"}
    pid = q.get("id", [""])[0]
    p = load(INDEX_FILE).get(pid) if PIC_ID.fullmatch(pid) else None
    if p is None or (p.get("gruppe") not in member and p.get("fra") != kid):
        return 404, {"feil": "not found"}
    try:
        with open(pic_path(pid), "rb") as f:
            return 200, f.read()
    except OSError:
        return 404, {"feil": "not found"}


def share(kid, q, body, sender):
    member = approved(kid)
    if member is None:
        return 403, {"feil": "not approved"}
    group = q.get("gruppe", [""])[0]
    if not GROUP.fullmatch(group) or group not in member:
        return 403, {"feil": "not in the group"}
    navn = clean_text(q.get("navn", [""])[0], 63)
    now = time.time()
    with tlock:
        times = shared_today[kid] = [t for t in shared_today.get(kid, []) if now - t < 86400]
        if len(times) >= PER_KEY_DAY:
            return 429, {"feil": "too many today"}
    jpg = picture(body)
    if jpg is None:
        return 415, {"feil": "not a picture"}
    with locked():
        index = load(INDEX_FILE)
        for pid, p in index.items():  # shared before: the same again
            if p.get("fra") == kid and p.get("gruppe") == group and navn and p.get("navn") == navn:
                return 200, {"id": pid}
        if len(index) >= MAX_PICTURES or sum(p.get("bytes", 0) for p in index.values()) + len(jpg) > MAX_BYTES:
            return 507, {"feil": "full"}
        pid = secrets.token_hex(6)
        while pid in index:
            pid = secrets.token_hex(6)
        tmp = pic_path("." + pid)
        with open(tmp, "wb") as f:
            f.write(jpg)
        os.replace(tmp, pic_path(pid))
        index[pid] = {"gruppe": group, "fra": kid, "navn": navn, "tid": int(now), "bytes": len(jpg)}
        save(INDEX_FILE, index)
    with tlock:
        shared_today.setdefault(kid, []).append(now)
    log(f"{pid} shared by {kid} from {sender} with {group}: {len(body)} -> {len(jpg)} bytes")
    return 200, {"id": pid}


def unshare(kid, q):
    if approved(kid) is None:
        return 403, {"feil": "not approved"}
    pid = q.get("id", [""])[0]
    with locked():
        index = load(INDEX_FILE)
        p = index.get(pid) if PIC_ID.fullmatch(pid) else None
        if p is None or p.get("fra") != kid:
            return 404, {"feil": "not found"}
        remove_picture(index, pid)
        save(INDEX_FILE, index)
    log(f"{pid} no longer shared by {kid}")
    return 200, {}


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "deling"
    sys_version = ""

    def log_message(self, *a):
        pass

    def send(self, code, obj):
        if isinstance(obj, bytes):
            body, ctype = obj, "image/jpeg"
        else:
            body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode()
            ctype = "application/json; charset=utf-8"
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def handle_one(self, method):
        i = self.path.find("/deling/")
        if i < 0:
            self.send(404, {"feil": "not found"})
            return
        rel = self.path[i + len("/deling/") :]
        what = urllib.parse.urlsplit(rel)
        q = urllib.parse.parse_qs(what.query)
        body = b""
        if method == "POST":
            try:
                length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                length = -1
            if not 0 <= length <= MAX_BODY:
                self.send(413, {"feil": "too large"})
                return
            body = self.rfile.read(length)
        sender = self.headers.get("X-Sender", "")
        if not re.fullmatch(r"[0-9a-fA-F.:]{2,45}", sender):
            sender = ""
        if what.path == "admin" or what.path.startswith("admin/"):
            try:
                web_admin(self, method, what.path, q, body, sender)
            except Exception as e:  # noqa: BLE001
                log(f"admin {method} {what.path} failed: {e}")
                self.send(500, {"feil": "failed"})
            return
        who = signed_by(self.headers, method, rel, body)
        if isinstance(who[0], int):
            log(f"refused {method} {what.path} from {sender}: {who[1]}")
            self.send(who[0], {"feil": who[1]})
            return
        kid, key_b64 = who
        try:
            if method == "GET" and what.path == "meg":
                code, obj = me(kid, key_b64, q)
            elif method == "GET" and what.path == "bilder":
                code, obj = list_pictures(kid, q)
            elif method == "GET" and what.path == "mine":
                code, obj = my_pictures(kid)
            elif method == "GET" and what.path == "bilde":
                code, obj = get_picture(kid, q)
            elif method == "POST" and what.path == "del":
                code, obj = share(kid, q, body, sender)
            elif method == "POST" and what.path == "slett":
                code, obj = unshare(kid, q)
            else:
                code, obj = 404, {"feil": "not found"}
        except Exception as e:  # noqa: BLE001
            log(f"{method} {what.path} failed: {e}")
            code, obj = 500, {"feil": "failed"}
        if code != 200:
            log(f"{method} {what.path} from {kid}: {code} {obj}")
        self.send(code, obj)

    def do_GET(self):
        self.handle_one("GET")

    def do_POST(self):
        self.handle_one("POST")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True




# ---- What the admin does (the web page below and the commands) ----


def when(t):
    return datetime.datetime.fromtimestamp(t).strftime("%d.%m.%Y %H:%M") if t else "?"


def parse_groups(s):
    """The groups in "familien, hytta" as a sorted list, or None if one
    isn't a group's name."""
    groups = [g.strip().lower() for g in re.split(r"[,; ]+", s) if g.strip()]
    if any(not GROUP.fullmatch(g) for g in groups):
        return None
    return sorted(set(groups))


def find_key(keys, kid):
    """The key that is kid or starts with it, or None if not just one."""
    if kid in keys:
        return kid
    hits = [k for k in keys if kid and k.startswith(kid)]
    return hits[0] if len(hits) == 1 else None


def act_groups(kid, groups, approve):
    """Groups `groups` for display kid, approved too with `approve`. The
    display's name, or None if there is no such key."""
    with locked():
        keys = load(KEYS_FILE)
        if kid not in keys:
            return None
        keys[kid]["grupper"] = groups
        if approve:
            if nick_taken(keys, kid, keys[kid].get("kallenavn", "")):
                keys[kid]["kallenavn"] = ""  # an approved display's already
            keys[kid]["status"] = "godkjent"
        save(KEYS_FILE, keys)
    log(f"{kid}: {keys[kid]['status']}, groups {','.join(groups) or '-'}")
    return keys[kid].get("navn", "")


def act_reject(kid):
    with locked():
        keys = load(KEYS_FILE)
        if kid not in keys:
            return None
        keys[kid]["status"] = "avvist"
        keys[kid]["grupper"] = []
        save(KEYS_FILE, keys)
    log(f"{kid} rejected")
    return keys[kid].get("navn", "")


def act_forget(kid):
    with locked():
        keys = load(KEYS_FILE)
        k = keys.pop(kid, None)
        if k is None:
            return None
        save(KEYS_FILE, keys)
    log(f"{kid} forgotten")
    return k.get("navn", "")


def act_delete(pid):
    """Picture pid no longer shared; False if there is none."""
    with locked():
        index = load(INDEX_FILE)
        if pid not in index:
            return False
        remove_picture(index, pid)
        save(INDEX_FILE, index)
    log(f"{pid} deleted by the admin")
    return True


def act_delete_from(kid):
    """All display kid shared, deleted; how many."""
    with locked():
        index = load(INDEX_FILE)
        gone = [pid for pid, p in index.items() if p.get("fra") == kid]
        for pid in gone:
            remove_picture(index, pid)
        save(INDEX_FILE, index)
    log(f"{len(gone)} pictures from {kid} deleted by the admin")
    return len(gone)


def admin(args):
    """The commands, on the Pi: docker compose exec deling python -I
    /app/deling.py <command>."""
    usage = (
        "Bruk: deling.py skjermer | godkjenn <nøkkel> <grupper> | grupper <nøkkel> <grupper> | "
        "avvis <nøkkel> | glem <nøkkel> | bilder [gruppe] | slett <id> | slett-fra <nøkkel> | nytt-passord"
    )
    if not args:
        sys.exit(usage)
    cmd, rest = args[0], args[1:]
    os.makedirs(PICS_DIR, exist_ok=True)
    keys, index = load(KEYS_FILE), load(INDEX_FILE)

    def key(arg):
        kid = find_key(keys, arg)
        if kid is None:
            sys.exit(f"Fant ikke én skjerm med nøkkelen {arg}")
        return kid

    if cmd == "skjermer" and not rest:
        if not keys:
            print("Ingen skjermer har spurt om deling ennå.")
        for kid, k in sorted(keys.items(), key=lambda kv: kv[1].get("forst", 0)):
            n = sum(1 for p in index.values() if p.get("fra") == kid)
            print(
                f"{kid}  {k.get('status', '?'):8}  {','.join(k.get('grupper', [])) or '-':20}  "
                f"{n:4} delt  først {when(k.get('forst'))}  {k.get('navn', '')}"
                f"{' «' + k['kallenavn'] + '»' if k.get('kallenavn') else ''}"
            )
    elif cmd in ("godkjenn", "grupper") and len(rest) in (1, 2):
        kid = key(rest[0])
        groups = parse_groups(rest[1] if len(rest) == 2 else "")
        if groups is None:
            sys.exit("Ugyldig gruppenavn (bare a-z, 0-9, - og _, høyst 24 tegn)")
        navn = act_groups(kid, groups, cmd == "godkjenn")
        print(f"{kid} ({navn}): grupper {', '.join(groups) or 'ingen'}")
    elif cmd == "avvis" and len(rest) == 1:
        kid = key(rest[0])
        act_reject(kid)
        print(f"{kid} er avvist. Bildene den har delt, ligger der fortsatt (slett-fra {kid}).")
    elif cmd == "glem" and len(rest) == 1:
        kid = key(rest[0])
        act_forget(kid)
        print(f"{kid} er glemt; spør den igjen, venter den på godkjenning på nytt.")
    elif cmd == "bilder" and len(rest) <= 1:
        for pid, p in sorted(index.items(), key=lambda kv: kv[1].get("tid", 0)):
            if rest and p.get("gruppe") != rest[0]:
                continue
            fra = keys.get(p.get("fra"), {}).get("navn", p.get("fra"))
            print(f"{pid}  {p.get('gruppe'):12}  {when(p.get('tid'))}  {p.get('bytes', 0) // 1024:4} kB  "
                  f"fra {fra}: {p.get('navn', '')}")
    elif cmd == "slett" and len(rest) == 1:
        if not act_delete(rest[0]):
            sys.exit(f"Fant ikke bildet {rest[0]}")
        print(f"{rest[0]} er slettet; skjermene sletter det neste gang de ser etter.")
    elif cmd == "slett-fra" and len(rest) == 1:
        kid = find_key(keys, rest[0]) or rest[0]
        print(f"{act_delete_from(kid)} bilder fra {kid} er slettet.")
    elif cmd == "nytt-passord" and not rest:
        # A forgotten password: the page asks for a new user and password,
        # from the home network.
        with contextlib.suppress(FileNotFoundError):
            os.remove(ADMIN_FILE)
        with tlock:
            sessions.clear()
        print("Brukeren og passordet er glemt. Åpne /deling/admin/ fra hjemmenettet for å velge nye.")
    else:
        sys.exit(usage)


# ---- The admin's web page: /deling/admin/ ----
#
# A user and password, chosen the first time the page is opened - from the
# home network only, as the page can be reached through the tunnel too and
# the first to come would otherwise choose them. nginx says whether a
# request came through the tunnel (X-Tunnel: Cloudflare puts CF-Connecting-
# IP on every request it passes on, and nobody outside can take it off).
# The password is kept as scrypt with a salt in ADMIN_FILE; a login gives a
# random session cookie (HttpOnly, SameSite=Strict, Secure through the
# tunnel) of SESSION_S since last use, and every form carries the session's
# own token. At most LOGIN_FAILS wrong logins from one sender in
# LOGIN_WINDOW_S, and LOGIN_FAILS_ALL through the tunnel from all - which
# closes the login through the tunnel only, so nobody outside can keep the
# admin out at home. Everything a display sent
# (its name, a picture's name) is escaped.

ADMIN_FILE = os.path.join(DATA_DIR, "admin.json")
SESSION_S = 12 * 3600
LOGIN_FAILS = 5
LOGIN_FAILS_ALL = 30
LOGIN_WINDOW_S = 15 * 60
PASSWORD_MIN = 10
COOKIE = "deling_okt"
sessions = {}  # token: {"until", "csrf", "melding"}
fails = {}  # sender: times of wrong logins
fails_tunnel = []  # times of those through the tunnel
SCRYPT = {"n": 2**14, "r": 8, "p": 1}

STYLE = """
:root{color-scheme:light dark;--bg:#f6f6f4;--card:#fff;--fg:#1d1d1b;--mut:#6b6b66;--line:#deded8;--acc:#1f6f8b;--bad:#a8322d;--ok:#2e7d32}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--card:#21211f;--fg:#ececea;--mut:#a3a39d;--line:#3a3a36;--acc:#6cb6d1;--bad:#e2756f;--ok:#7cc47f}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}
main{max-width:960px;margin:0 auto;padding:16px}h1{font-size:1.5rem;margin:.5rem 0 1rem}h2{font-size:1.15rem;margin:1.6rem 0 .6rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin:0 0 10px}
.mut{color:var(--mut);font-size:.9rem}.err{color:var(--bad)}.okm{color:var(--ok)}code{font-size:.9rem;overflow-wrap:anywhere}
label{display:block;font-size:.9rem;margin:.5rem 0 .2rem}input[type=text],input[type=password]{width:100%;max-width:360px;padding:8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}
button{font:inherit;padding:7px 12px;margin:.4rem .4rem 0 0;border-radius:6px;border:1px solid var(--acc);background:var(--acc);color:#fff;cursor:pointer}
button.sec{background:transparent;color:var(--acc)}button.bad{background:transparent;border-color:var(--bad);color:var(--bad)}
form.inl{display:inline}.row{display:flex;flex-wrap:wrap;gap:6px 14px;align-items:baseline}.row strong{font-size:1.05rem}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:10px}.grid img{width:100%;aspect-ratio:5/3;object-fit:contain;background:#000;border-radius:6px;display:block}
.top{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}
"""


def esc(s):
    return html.escape(str(s), quote=True)


def hash_password(pw, salt):
    return hashlib.scrypt(pw.encode(), salt=salt, maxmem=64 * 1024 * 1024, dklen=32, **SCRYPT)


def account():
    a = load(ADMIN_FILE)
    return a if a.get("bruker") and a.get("hash") and a.get("salt") else None


def set_account(user, pw):
    salt = secrets.token_bytes(16)
    save(ADMIN_FILE, {"bruker": user, "salt": salt.hex(), "hash": hash_password(pw, salt).hex()})
    with contextlib.suppress(OSError):
        os.chmod(ADMIN_FILE, 0o600)


def password_ok(a, user, pw):
    good = hash_password(pw, bytes.fromhex(a["salt"]))
    return secrets.compare_digest(user.encode(), a["bruker"].encode()) & secrets.compare_digest(
        good, bytes.fromhex(a["hash"])
    )


def login_allowed(sender, tunnel):
    now = time.time()
    with tlock:
        for k in list(fails):
            fails[k] = [t for t in fails[k] if now - t < LOGIN_WINDOW_S]
            if not fails[k]:
                del fails[k]
        fails_tunnel[:] = [t for t in fails_tunnel if now - t < LOGIN_WINDOW_S]
        return len(fails.get(sender, [])) < LOGIN_FAILS and not (tunnel and len(fails_tunnel) >= LOGIN_FAILS_ALL)


def login_failed(sender, tunnel):
    with tlock:
        fails.setdefault(sender, []).append(time.time())
        if tunnel:
            fails_tunnel.append(time.time())


def new_session():
    token = secrets.token_urlsafe(32)
    with tlock:
        now = time.time()
        for k in [k for k, v in sessions.items() if v["until"] < now]:
            del sessions[k]
        sessions[token] = {"until": now + SESSION_S, "csrf": secrets.token_urlsafe(24), "melding": None}
    return token


def session_of(handler):
    """The session the request's cookie names, kept alive, or None."""
    cookie = handler.headers.get("Cookie", "")
    token = next((c.split("=", 1)[1] for c in (x.strip() for x in cookie.split(";")) if c.startswith(COOKIE + "=")), "")
    with tlock:
        s = sessions.get(token)
        if s is None or s["until"] < time.time():
            sessions.pop(token, None)
            return None
        s["until"] = time.time() + SESSION_S
        return token, s


def flash(sess, text, bad=False):
    if sess:
        sess[1]["melding"] = (text, bad)


def page(handler, title, body, code=200, cookie=None):
    doc = (
        "<!doctype html><html lang=nb><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        f"<title>{esc(title)} - Værtavla</title><style>{STYLE}</style></head><body><main>{body}</main></body></html>"
    ).encode()
    handler.send_response(code)
    handler.send_header("Content-Type", "text/html; charset=utf-8")
    handler.send_header("Content-Length", str(len(doc)))
    handler.send_header("Cache-Control", "no-store")
    handler.send_header(
        "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; img-src 'self'; form-action 'self'; frame-ancestors 'none'",
    )
    handler.send_header("X-Content-Type-Options", "nosniff")
    handler.send_header("Referrer-Policy", "no-referrer")
    if cookie is not None:
        handler.send_header("Set-Cookie", cookie)
    handler.end_headers()
    handler.wfile.write(doc)


def redirect(handler, cookie=None):
    handler.send_response(303)
    handler.send_header("Location", "/deling/admin/")
    handler.send_header("Content-Length", "0")
    handler.send_header("Cache-Control", "no-store")
    if cookie is not None:
        handler.send_header("Set-Cookie", cookie)
    handler.end_headers()


def cookie_for(token, tunnel, max_age=SESSION_S):
    return (
        f"{COOKIE}={token}; Path=/deling/admin/; Max-Age={max_age}; HttpOnly; SameSite=Strict" + ("; Secure" if tunnel else "")
    )


def form_page(title, intro, action, fields, button, msg=None):
    rows = "".join(
        f"<label for={esc(n)}>{esc(lab)}</label><input id={esc(n)} name={esc(n)} type={t} "
        f"autocomplete={ac} required{' autofocus' if i == 0 else ''}>"
        for i, (n, lab, t, ac) in enumerate(fields)
    )
    m = f"<p class=err>{esc(msg)}</p>" if msg else ""
    return (
        f"<h1>{esc(title)}</h1><div class=card><p>{intro}</p>{m}"
        f"<form method=post action='/deling/admin/{action}'>{rows}<button>{esc(button)}</button></form></div>"
    )


def setup_page(handler, tunnel, msg=None):
    if tunnel:
        page(handler, "Deling", "<h1>Deling av bilder</h1><div class=card><p>Brukeren og passordet for denne siden "
             "velges første gang den åpnes, og det må gjøres fra hjemmenettet, på Pi-ens egen adresse "
             "(for eksempel <code>http://&lt;pi&gt;:8070/deling/admin/</code>). Etter det kan du logge inn "
             "herfra.</p></div>", 403)
        return
    page(handler, "Velg bruker", form_page(
        "Deling av bilder",
        "Første gang: velg brukernavn og passord for siden der skjermer godkjennes for deling av bilder. "
        f"Passordet må ha minst {PASSWORD_MIN} tegn.",
        "oppsett",
        [("bruker", "Brukernavn", "text", "username"), ("passord", "Passord", "password", "new-password"),
         ("gjenta", "Gjenta passordet", "password", "new-password")],
        "Lagre og logg inn", msg))


def login_page(handler, msg=None, code=200):
    page(handler, "Logg inn", form_page(
        "Deling av bilder", "Logg inn for å godkjenne skjermer og se hva som er delt.", "logg-inn",
        [("bruker", "Brukernavn", "text", "username"), ("passord", "Passord", "password", "current-password")],
        "Logg inn", msg), code)


def main_page(handler, sess):
    token, s = sess
    csrf = esc(s["csrf"])
    msg = s.pop("melding", None)
    s["melding"] = None
    keys, index = load(KEYS_FILE), load(INDEX_FILE)
    hidden = f"<input type=hidden name=csrf value='{csrf}'>"

    def key_form(kid, inner):
        return f"<form method=post action='/deling/admin/skjerm'>{hidden}<input type=hidden name=nokkel value='{esc(kid)}'>{inner}</form>"

    def groups_field(kid, value):
        fid = "g" + esc(kid)
        return (f"<label for={fid}>Grupper (med komma mellom)</label>"
                f"<input id={fid} type=text name=grupper value='{esc(value)}' placeholder='familien, hytta'>")

    def head(kid, k, extra=""):
        n = sum(1 for p in index.values() if p.get("fra") == kid)
        nick = k.get("kallenavn")
        return (f"<div class=row><strong>{esc(k.get('navn') or 'Ukjent skjerm')}</strong>"
                f"{f' «{esc(nick)}»' if nick else ''}"
                f"<span class=mut>nøkkel <code>{esc(kid)}</code> · først sett {esc(when(k.get('forst')))}"
                f"{f' · {n} delt' if n else ''}{extra}</span></div>")

    out = [f"<div class=top><h1>Deling av bilder</h1><form method=post action='/deling/admin/logg-ut'>{hidden}"
           f"<button class=sec>Logg ut</button></form></div>"]
    if msg:
        out.append(f"<p class={'err' if msg[1] else 'okm'}>{esc(msg[0])}</p>")
    by = sorted(keys.items(), key=lambda kv: kv[1].get("forst", 0))
    waiting = [(k, v) for k, v in by if v.get("status", "venter") == "venter"]
    ok = [(k, v) for k, v in by if v.get("status") == "godkjent"]
    rejected = [(k, v) for k, v in by if v.get("status") == "avvist"]

    out.append("<h2>Venter på godkjenning</h2>")
    if not waiting:
        out.append("<p class=mut>Ingen skjermer venter. En skjerm ber om å være med første gang noen åpner "
                   "Bilder-siden på den; nøkkelen står der under Deling.</p>")
    for kid, k in waiting:
        out.append("<div class=card>" + head(kid, k) + key_form(kid, groups_field(kid, "") +
                   "<button name=handling value=godkjenn>Godkjenn</button>"
                   "<button class=bad name=handling value=avvis>Avvis</button>"
                   "<button class=sec name=handling value=glem>Glem</button>") + "</div>")

    out.append("<h2>Godkjente skjermer</h2>")
    if not ok:
        out.append("<p class=mut>Ingen ennå.</p>")
    for kid, k in ok:
        gs = k.get("grupper", [])
        out.append("<div class=card>" + head(kid, k, " · " + (esc(", ".join(gs)) if gs else "ingen grupper")) +
                   key_form(kid, groups_field(kid, ", ".join(gs)) +
                            "<button name=handling value=grupper>Lagre gruppene</button>"
                            "<button class=bad name=handling value=avvis>Avvis</button>"
                            "<button class=bad name=handling value=slett-bilder>Slett bildene den har delt</button>") +
                   "</div>")

    if rejected:
        out.append("<h2>Avviste skjermer</h2>")
        for kid, k in rejected:
            out.append("<div class=card>" + head(kid, k) + key_form(kid, groups_field(kid, "") +
                       "<button name=handling value=godkjenn>Godkjenn likevel</button>"
                       "<button class=sec name=handling value=glem>Glem</button>"
                       "<button class=bad name=handling value=slett-bilder>Slett bildene den har delt</button>") +
                       "</div>")

    out.append(f"<h2>Delte bilder ({len(index)})</h2>")
    if not index:
        out.append("<p class=mut>Ingenting er delt ennå.</p>")
    groups = sorted({p.get("gruppe", "") for p in index.values()})
    for g in groups:
        pics = sorted(((pid, p) for pid, p in index.items() if p.get("gruppe") == g), key=lambda kv: -kv[1].get("tid", 0))
        cells = "".join(
            f"<div><img src='/deling/admin/bilde?id={esc(pid)}' loading=lazy alt=''>"
            f"<div class=mut>{esc(keys.get(p.get('fra'), {}).get('navn') or p.get('fra'))} · {esc(when(p.get('tid')))}</div>"
            f"<form method=post action='/deling/admin/bilde-slett'>{hidden}<input type=hidden name=id value='{esc(pid)}'>"
            f"<button class=bad>Slett</button></form></div>"
            for pid, p in pics
        )
        out.append(f"<div class=card><strong>{esc(g)}</strong> <span class=mut>({len(pics)})</span>"
                   f"<div class=grid style='margin-top:8px'>{cells}</div></div>")

    out.append("<h2>Bytt passord</h2><div class=card>"
               f"<form method=post action='/deling/admin/passord'>{hidden}"
               "<label for=pg>Passordet nå</label><input id=pg type=password name=gammelt autocomplete=current-password required>"
               f"<label for=pn>Nytt passord (minst {PASSWORD_MIN} tegn)</label><input id=pn type=password name=nytt autocomplete=new-password required>"
               "<label for=pr>Gjenta det nye</label><input id=pr type=password name=gjenta autocomplete=new-password required>"
               "<button>Bytt passord</button></form></div>")
    page(handler, "Deling", "".join(out))


def web_admin(handler, method, path, q, body, sender):
    tunnel = handler.headers.get("X-Tunnel", "0") != "0"
    if path == "admin":  # the cookie is for /deling/admin/
        return redirect(handler)
    sub = path[len("admin"):].strip("/")
    form = urllib.parse.parse_qs(body.decode("utf-8", "replace")) if method == "POST" else {}

    def f(name):
        return form.get(name, [""])[0]

    a = account()
    if a is None:
        if method == "POST" and sub == "oppsett" and not tunnel:
            user, pw = f("bruker").strip(), f("passord")
            if not 1 <= len(user) <= 40 or CONTROL.search(user):
                return setup_page(handler, tunnel, "Brukernavnet må ha 1-40 tegn.")
            if len(pw) < PASSWORD_MIN:
                return setup_page(handler, tunnel, f"Passordet må ha minst {PASSWORD_MIN} tegn.")
            if pw != f("gjenta"):
                return setup_page(handler, tunnel, "Passordene er ikke like.")
            with locked():
                if account() is not None:  # someone was first
                    return login_page(handler)
                set_account(user, pw)
            log(f"admin user chosen from {sender}")
            return redirect(handler, cookie_for(new_session(), tunnel))
        return setup_page(handler, tunnel)

    if method == "POST" and sub == "logg-inn":
        if not login_allowed(sender, tunnel):
            log(f"login refused from {sender}: too many tries")
            return login_page(handler, "For mange feil forsøk. Vent et kvarter og prøv igjen.", 429)
        if not password_ok(a, f("bruker").strip(), f("passord")):
            login_failed(sender, tunnel)
            log(f"wrong login from {sender}")
            return login_page(handler, "Feil brukernavn eller passord.", 401)
        log(f"admin logged in from {sender}")
        return redirect(handler, cookie_for(new_session(), tunnel))

    sess = session_of(handler)
    if sess is None:
        if method == "POST":
            return redirect(handler)
        return login_page(handler)
    token, s = sess

    if method == "GET" and sub == "":
        return main_page(handler, sess)
    if method == "GET" and sub == "bilde":
        pid = q.get("id", [""])[0]
        data = b""
        if PIC_ID.fullmatch(pid):
            with contextlib.suppress(OSError), open(pic_path(pid), "rb") as fh:
                data = fh.read()
        if not data:
            return handler.send(404, {"feil": "not found"})
        handler.send_response(200)
        handler.send_header("Content-Type", "image/jpeg")
        handler.send_header("Content-Length", str(len(data)))
        handler.send_header("Cache-Control", "private, max-age=3600")
        handler.send_header("X-Content-Type-Options", "nosniff")
        handler.end_headers()
        handler.wfile.write(data)
        return None
    if method != "POST":
        return handler.send(404, {"feil": "not found"})
    if not secrets.compare_digest(f("csrf"), s["csrf"]):
        log(f"admin form without its token from {sender}")
        return redirect(handler)

    if sub == "logg-ut":
        with tlock:
            sessions.pop(token, None)
        return redirect(handler, cookie_for("", tunnel, 0))
    if sub == "skjerm":
        kid, what = f("nokkel"), f("handling")
        keys = load(KEYS_FILE)
        if not KEY_ID.fullmatch(kid) or kid not in keys:
            flash(sess, "Fant ikke skjermen.", True)
        elif what in ("godkjenn", "grupper"):
            groups = parse_groups(f("grupper"))
            if groups is None:
                flash(sess, "Et gruppenavn kan bare ha a-z, 0-9, - og _ (høyst 24 tegn), og gruppene skilles med komma.", True)
            elif what == "godkjenn" and not groups:
                flash(sess, "Skriv minst én gruppe skjermen skal være med i.", True)
            else:
                navn = act_groups(kid, groups, what == "godkjenn")
                flash(sess, f"«{navn}» er {'godkjent' if what == 'godkjenn' else 'lagret'}, med "
                            f"{'gruppen' if len(groups) == 1 else 'gruppene'} {', '.join(groups) or 'ingen'}.")
        elif what == "avvis":
            flash(sess, f"«{act_reject(kid)}» er avvist. Bildene den har delt, ligger der fortsatt.")
        elif what == "glem":
            flash(sess, f"«{act_forget(kid)}» er glemt. Spør den igjen, venter den på godkjenning på nytt.")
        elif what == "slett-bilder":
            flash(sess, f"{act_delete_from(kid)} bilder er slettet. Skjermene sletter dem neste gang de ser etter.")
        else:
            flash(sess, "Ukjent handling.", True)
        return redirect(handler)
    if sub == "bilde-slett":
        ok = PIC_ID.fullmatch(f("id")) and act_delete(f("id"))
        flash(sess, "Bildet er slettet. Skjermene sletter det neste gang de ser etter." if ok else "Fant ikke bildet.", not ok)
        return redirect(handler)
    if sub == "passord":
        if not password_ok(a, a["bruker"], f("gammelt")):
            flash(sess, "Passordet nå er feil.", True)
        elif len(f("nytt")) < PASSWORD_MIN:
            flash(sess, f"Det nye passordet må ha minst {PASSWORD_MIN} tegn.", True)
        elif f("nytt") != f("gjenta"):
            flash(sess, "De nye passordene er ikke like.", True)
        else:
            set_account(a["bruker"], f("nytt"))
            with tlock:  # other sessions out
                for k in [k for k in sessions if k != token]:
                    del sessions[k]
            log(f"admin password changed from {sender}")
            flash(sess, "Passordet er byttet.")
        return redirect(handler)
    return handler.send(404, {"feil": "not found"})


if __name__ == "__main__":
    if len(sys.argv) > 1:
        admin(sys.argv[1:])
        sys.exit(0)
    os.makedirs(PICS_DIR, exist_ok=True)
    for d in os.scandir(PICS_DIR):  # half written when the service stopped
        if d.name.startswith("."):
            os.remove(d.path)
    log(f"shared pictures in {DATA_DIR}, push {'on' if NTFY_TOPIC else 'off'}, port {PORT}")
    Server(("", PORT), Handler).serve_forever()
