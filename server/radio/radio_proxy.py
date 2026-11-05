"""Radio that Sonos can pause (server/compose.yaml, service radio).

Sonos plays an address it is given as a radio station (x-rincon-mp3radio:)
with Stop only, but a plain http:// file with Pause too. This serves each
station of stations.json as such a file, /radio/<key>.mp3, of a made-up
length (Sonos asks for a byte range only when it knows one), and records
the station while a speaker is paused, so Play goes on from where it was.

What Sonos was seen to do (a Roam, firmware 97.2, November 2026):
  - Started (and Play after Stop): a GET without a range, which plays,
    and short probes beside it: another GET without a range, the last 128
    bytes (an ID3v1 tag) and bytes=10- (an ID3v2 tag).
  - Pause: it closes the connection.
  - Play after a pause: a probing GET without a range, then the one that
    plays, asking for the byte it had come to (bytes=1032192-).
  - Seek: a range from its own guess of where that time is in the file,
    which isn't the bitrate's (it took 2 GB of 192 kbit/s for 69 hours).
So a GET without a range starts at what the station sends now, and a range
goes on in the listening that had come that far: the latest one from that
speaker still open, or one that played for a while. Listenings are kept for
RADIO_PAUSE_MINUTES after their last connection, and a station is recorded
while one of its listenings is kept.

Where a listening is (what the speaker plays now, for the song's title and
how far behind it is) is counted here, as Sonos' own position can't be
mapped to bytes: from where it started, on at the station's bitrate while
a connection is open. So that the speaker isn't far ahead of that, it gets
at most AHEAD_S seconds more than it has played.

/radio/<key>.json?speaker=IP says what the speaker's listening plays,
{"title": "...", "artist": "...", "image": "art/ID.jpg", "behind": seconds},
as the station sent it in the stream (ICY metadata, taken out of what the
speaker gets): StreamTitle, and StreamUrl where it says more - P4's JSON
with the artist, title and a picture, Bauer's (NRJ) an address of the
same at planetradio.co.uk. The picture is fetched and made 240 x 240 (the
display's cover, a baseline JPEG it decodes) when /radio/art/ID.jpg is
asked for, ID one this server gave out; with
&skip=N it first moves N seconds on (never past now), or to now with
skip=live. The display then asks the speaker to seek (anywhere), which
throws away what it had fetched ahead: the range it then asks for goes on
from the new place.

Only Python's standard library; nginx (server/nginx.conf) sends /radio/ here
from the home network only, the speaker's address in X-Real-IP.
"""

import hashlib
import http.server
import io
import json
import os
import re
import socketserver
import threading
import time
import urllib.parse
import urllib.request
from collections import OrderedDict

from PIL import Image

PORT = 8080
LENGTH = 2_000_000_000  # Sonos' track length in bytes: some 17 hours of 256 kbit/s
PAUSE_S = int(os.environ.get("RADIO_PAUSE_MINUTES", "60")) * 60
CHUNK = 16384
PREROLL_S = 3  # seconds a start goes back from now, so the speaker starts at once
TAIL = 4096  # a range this near the end is a tag probe: zeros
PLAYED_S = 2.0  # a connection this long played, it wasn't a probe
AHEAD_S = 10  # the most a speaker gets ahead of where it plays
STARTUP_S = 1.0  # from a connection's start till the speaker plays
BURST_S = 1.5  # a station sends some seconds it has at once: waited for before a start
SEEK_S = 5  # how long after a skip the speaker's seek is waited for
ART_SIDE = 240  # the display's cover
ART_KEPT = 40  # pictures kept made
PLANET = "https://listenapi.planetradio.co.uk/"  # Bauer's song details
# Where the stations' pictures are (P4's, Bauer's): the stream says what is
# fetched, so only from these - not anywhere, nor inside the home network.
ART_HOSTS = ("ddjh0q34m0xak.cloudfront.net", "media.bauerradio.com")
Image.MAX_IMAGE_PIXELS = 4096 * 4096  # a cover, not a decompression bomb
STATIONS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "stations.json")))


def log(*a):
    print(*a, flush=True)  # docker logs -t gives the time


def icy_fields(meta):
    """StreamTitle and StreamUrl of an ICY metadata block ("" if missing);
    None if it has no StreamTitle."""
    text = meta.rstrip(b"\0")
    try:
        text = text.decode("utf-8")
    except UnicodeDecodeError:
        text = text.decode("latin-1")
    t = re.search(r"StreamTitle='(.*?)';", text, re.S)
    u = re.search(r"StreamUrl='(.*?)';", text, re.S)
    return (t.group(1).strip(), u.group(1).strip() if u else "") if t else None


def fetch(url, limit=2_000_000):
    req = urllib.request.Request(url, headers={"User-Agent": "MultiDisplay radio"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read(limit)


class Song:
    """What a station sent from a place in its stream on."""

    def __init__(self, offset, raw, title, artist="", image=""):
        self.offset, self.raw = offset, raw
        self.title, self.artist, self.image = title, artist, image


def song_of(offset, title, url):
    """A Song of StreamTitle and StreamUrl; Bauer's details are fetched
    after (details())."""
    song = Song(offset, (title, url), title)
    if url.startswith("{"):  # P4's
        try:
            d = json.loads(url)
        except ValueError:
            d = {}
        if d.get("type") == "music" and d.get("title"):
            song.title, song.artist = d["title"], d.get("artist", "")
        if art_ok(d.get("imageUrl")):
            song.image = art_id(d["imageUrl"])
    return song


def details(song, url):
    """Bauer's song details at `url` into `song`."""
    try:
        d = json.loads(fetch(url, 65536))
    except Exception as e:  # noqa: BLE001 - shown without
        log("details failed:", e)
        return
    if d.get("eventSongTitle"):
        song.title, song.artist = d["eventSongTitle"], d.get("eventSongArtist", "")
    image = d.get("eventImageUrl")
    if art_ok(image):
        song.image = art_id(image)


art_urls = {}  # id -> the picture's address, of the ids given out
art_made = OrderedDict()  # id -> the JPEG made, the newest last
art_lock = threading.Lock()


def art_ok(url):
    """Whether `url` is a picture of the stations' own hosts."""
    if not isinstance(url, str) or not url:
        return False
    u = urllib.parse.urlsplit(url)
    if u.scheme == "https" and u.hostname in ART_HOSTS and u.port is None:
        return True
    log("picture from elsewhere left out:", url[:200])
    return False


def art_id(url):
    i = hashlib.sha1(url.encode()).hexdigest()[:16]
    with art_lock:
        art_urls[i] = url
        while len(art_urls) > 2000:
            art_urls.pop(next(iter(art_urls)))
    return i


def art(i):
    """Picture `i` as the display's cover, None if it can't be had."""
    with art_lock:
        if i in art_made:
            art_made.move_to_end(i)
            return art_made[i]
        url = art_urls.get(i)
    if url is None:
        return None
    try:
        im = Image.open(io.BytesIO(fetch(url, 8_000_000)))
        im.draft("RGB", (ART_SIDE, ART_SIDE))  # a big JPEG decoded smaller
        im = im.convert("RGB")
        side = min(im.size)  # the middle square
        left, top = (im.width - side) // 2, (im.height - side) // 2
        im = im.crop((left, top, left + side, top + side)).resize((ART_SIDE, ART_SIDE), Image.LANCZOS)
        out = io.BytesIO()
        im.save(out, "JPEG", quality=85, progressive=False)
    except Exception as e:  # noqa: BLE001 - the logo instead
        log("picture failed:", url, e)
        return None
    with art_lock:
        art_made[i] = out.getvalue()
        while len(art_made) > ART_KEPT:
            art_made.popitem(last=False)
    return out.getvalue()


class Recorder:
    """A station's stream as chunks numbered by their byte offset from the
    first, and the titles it sent by where they began; read by the
    listenings while one of them is kept."""

    def __init__(self, key, url):
        self.key, self.url = key, url
        self.cond = threading.Condition()
        self.chunks = []  # (offset, bytes), oldest first
        self.titles = []  # Songs, oldest first
        self.start = 0  # offset of chunks[0]
        self.end = 0  # offset after the last chunk
        self.rate = 16000  # bytes a second, from icy-br once it answers
        self.first_t = 0.0  # when this recording's first bytes came
        self.running = False
        self.wanted = False

    def ensure(self):
        with self.cond:
            self.wanted = True
            if not self.running:
                self.running = True
                threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        log(self.key, "recording")
        wait = 1
        while True:
            with self.cond:
                if not self.wanted:
                    self.running = False
                    self.chunks, self.titles, self.start = [], [], self.end
                    log(self.key, "stopped")
                    return
            try:
                req = urllib.request.Request(self.url, headers={"User-Agent": "MultiDisplay radio", "Icy-MetaData": "1"})
                with urllib.request.urlopen(req, timeout=15) as r:
                    wait = 1
                    br = re.match(r"\d+", r.headers.get("icy-br") or "")
                    if br and 32 <= int(br.group()) <= 320:
                        self.rate = int(br.group()) * 125
                    every = int(r.headers.get("icy-metaint") or 0)
                    while self.wanted:
                        data = r.read(every or CHUNK)
                        if not data:
                            break
                        self.add(data)
                        if every:
                            n = r.read(1)
                            meta = r.read(n[0] * 16) if n and n[0] else b""
                            fields = icy_fields(meta) if meta else None
                            if fields is not None:
                                self.note(*fields)
                time.sleep(1)  # the station ended the stream: again
            except Exception as e:  # noqa: BLE001 - any failure: try again
                log(self.key, "stream failed:", e)
                time.sleep(wait)
                wait = min(wait * 2, 30)

    def add(self, data):
        with self.cond:
            if not self.chunks:
                self.first_t = time.time()
            self.chunks.append((self.end, data))
            self.end += len(data)
            # Only PAUSE_S back (listenings expire by then).
            while len(self.chunks) > 1 and self.end - self.chunks[1][0] > self.keep_bytes():
                self.chunks.pop(0)
            self.start = self.chunks[0][0] if self.chunks else self.end
            while len(self.titles) > 1 and self.titles[1].offset <= self.start:
                self.titles.pop(0)
            self.cond.notify_all()

    def note(self, title, url):
        with self.cond:
            if self.titles and self.titles[-1].raw == (title, url):
                return
            song = song_of(self.end, title, url)
            self.titles.append(song)
        if url.startswith(PLANET):
            threading.Thread(target=details, args=(song, url), daemon=True).start()

    def song_at(self, offset):
        """The Song at `offset`; before the first one came, the first."""
        with self.cond:
            for song in reversed(self.titles):
                if song.offset <= offset:
                    return song
            return self.titles[0] if self.titles else None

    def keep_bytes(self):
        # Enough for 320 kbit/s, the most a station sends.
        return max(PAUSE_S * 40_000, 8 * 1024 * 1024)

    def read(self, offset, timeout=30):
        """Bytes from `offset` on, waiting for them; b"" if none came."""
        with self.cond:
            if offset < self.start:
                offset = self.start  # gone: from the oldest kept
            if offset >= self.end:
                self.cond.wait_for(lambda: self.end > offset, timeout)
                if offset >= self.end:
                    return offset, b""
            for o, d in reversed(self.chunks):
                if o <= offset:
                    return offset, d[offset - o : offset - o + CHUNK]
        return offset, b""


class Listening:
    """A speaker's listening to a station: its byte 0 is the recording's
    `base`, and it plays its byte `head` (counted on from `head_t` while a
    connection is open)."""

    def __init__(self, ip, key, base, offset):
        self.ip, self.key, self.base = ip, key, base
        self.open = 0
        self.opened = 0.0  # when the first open connection began
        self.played = False
        self.sent = offset  # the furthest byte sent (in its own numbering)
        self.head, self.head_t = offset, time.time() + STARTUP_S
        self.rebase = None  # (where it plays, until when) after a skip
        self.gen = 0  # raised by the seek after a skip: older connections end
        self.last = time.time()

    def head_now(self, rate):
        """Where it plays now (lock held)."""
        h = self.head
        if self.open:
            h += max(0.0, time.time() - self.head_t) * rate
        return int(min(h, self.sent))


recorders = {k: Recorder(k, s["url"]) for k, s in STATIONS.items()}
listenings = []
lock = threading.Lock()


def tidy():
    """Listenings expired, and recorders no longer needed stopped."""
    now = time.time()
    with lock:
        listenings[:] = [x for x in listenings if x.open or now - x.last < PAUSE_S]
        needed = {x.key for x in listenings}
    for k, r in recorders.items():
        if k not in needed and r.wanted:
            with r.cond:
                r.wanted = False


def opened(x, now):
    """A connection of `x` begun (lock held): it plays on from where it
    stood."""
    if x.open == 0:
        x.head_t = now + STARTUP_S
        x.opened = now
    x.open += 1


def listening_for(ip, key, offset):
    """The listening a request for `offset` goes on in, a new one if none;
    counted as open."""
    rec = recorders[key]
    rec.ensure()
    with rec.cond:
        rec.cond.wait_for(lambda: rec.chunks, 15)
        first = rec.first_t
    # Just started: the seconds the station had sent at once, so that "now"
    # is now.
    time.sleep(max(0.0, min(BURST_S, first + BURST_S - time.time())))
    now = time.time()
    with lock:
        if offset > 0:
            for x in reversed(listenings):
                if x.ip != ip or x.key != key:
                    continue
                if x.rebase and now < x.rebase[1]:
                    # The seek after a skip: on from where the skip went.
                    x.base, x.head, x.sent, x.rebase = x.rebase[0] - offset, offset, offset, None
                    x.head_t = now + STARTUP_S
                    x.gen += 1
                    x.open += 1
                    return x
                if (x.open or x.played) and offset <= x.sent + 4 * CHUNK:
                    opened(x, now)
                    return x
        with rec.cond:
            base = max(rec.start, rec.end - PREROLL_S * rec.rate) - offset
        x = Listening(ip, key, base, offset)
        opened(x, now)
        listenings.append(x)
        return x


def playing(ip, key):
    """The speaker's listening it plays (lock held), None if none."""
    mine = [x for x in listenings if x.ip == ip and x.key == key and (x.open or x.played)]
    return max(mine, key=lambda x: (x.open > 0, x.last), default=None)


def status(ip, key, skip):
    rec = recorders[key]
    with lock:
        x = playing(ip, key)
        if x is None:
            return {}
        at = x.base + x.head_now(rec.rate)
        if skip:
            with rec.cond:
                live = max(rec.start, rec.end - PREROLL_S * rec.rate)
            to = live if skip == "live" else min(live, at + int(skip) * rec.rate)
            if to > at:
                # The bytes it fetches next come from there, and the range
                # of the seek that should follow, too.
                x.base += to - at
                x.head = x.head_now(rec.rate)
                x.head_t = time.time()
                x.rebase = (to, time.time() + SEEK_S)
                log(key, ip, f"skipped {(to - at) // rec.rate} s")
                at = to
    with rec.cond:
        behind = max(0, rec.end - at) // rec.rate
    song = rec.song_at(at)
    return {
        "title": song.title if song else "",
        "artist": song.artist if song else "",
        "image": f"art/{song.image}.jpg" if song and song.image else "",
        "behind": int(behind),
    }


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "radio"
    sys_version = ""

    def log_message(self, *a):
        pass

    def do_HEAD(self):
        self.serve(False)

    def do_GET(self):
        self.serve(True)

    def answer_status(self, key):
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        speaker = (q.get("speaker") or [""])[0]
        skip = (q.get("skip") or [""])[0]
        if not re.fullmatch(r"[0-9.]{7,15}", speaker) or not re.fullmatch(r"|live|[0-9]{1,5}", skip):
            self.send_error(400)
            return
        body = json.dumps(status(speaker, key, skip), ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def answer_art(self, i, body):
        data = art(i)
        if data is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "public, max-age=86400")
        self.end_headers()
        if body:
            self.wfile.write(data)

    def serve(self, body):
        a = re.fullmatch(r"/radio/art/([0-9a-f]{16})\.jpg", self.path.split("?")[0])
        if a:
            self.answer_art(a.group(1), body)
            return
        m = re.fullmatch(r"/radio/([a-z0-9_]+)\.(mp3|json)", self.path.split("?")[0])
        if not m or m.group(1) not in recorders:
            self.send_error(404)
            return
        key = m.group(1)
        if m.group(2) == "json":
            self.answer_status(key)
            return
        ip = self.headers.get("X-Real-IP") or self.client_address[0]
        offset, ranged = 0, False
        r = re.fullmatch(r"bytes=(\d+)-", self.headers.get("Range", ""))
        if r:
            offset, ranged = int(r.group(1)), True
            if offset >= LENGTH:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{LENGTH}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        self.send_response(206 if ranged else 200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(LENGTH - offset))
        self.send_header("Cache-Control", "no-store")
        if ranged:
            self.send_header("Content-Range", f"bytes {offset}-{LENGTH - 1}/{LENGTH}")
        self.end_headers()
        if not body:
            return
        if offset >= LENGTH - TAIL:
            self.wfile.write(bytes(LENGTH - offset))
            return
        tidy()
        x = listening_for(ip, key, offset)
        rec = recorders[key]
        if ranged and offset > PREROLL_S * rec.rate:
            log(key, ip, f"from {offset}, {(rec.end - (x.base + offset)) // rec.rate} s behind")
        t0 = time.time()
        pos = offset
        gen = x.gen
        try:
            while pos < LENGTH and x.gen == gen:
                with lock:
                    ahead = pos - x.head_now(rec.rate)
                    start = x.base + pos
                if ahead > AHEAD_S * rec.rate:
                    time.sleep(0.25)
                    continue
                at, data = rec.read(start)
                if not data:
                    break  # the station sent nothing for a while
                with lock:
                    if x.base + pos != start or x.gen != gen:
                        continue  # skipped meanwhile: from the new place
                    pos = at - x.base
                data = data[: LENGTH - pos]
                self.wfile.write(data)
                pos += len(data)
                with lock:
                    if x.gen == gen:
                        x.sent = max(x.sent, pos)
                    x.last = time.time()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        finally:
            with lock:
                if x.open == 1:
                    x.head = x.head_now(rec.rate)  # paused: it stands there
                x.open -= 1
                x.last = time.time()
                if time.time() - t0 >= PLAYED_S:
                    x.played = True
            tidy()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def tidy_loop():
    while True:
        time.sleep(60)
        tidy()


if __name__ == "__main__":
    threading.Thread(target=tidy_loop, daemon=True).start()
    log(f"{len(recorders)} stations, pause kept {PAUSE_S // 60} min, port {PORT}")
    Server(("", PORT), Handler).serve_forever()
