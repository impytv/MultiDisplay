#!/usr/bin/env python3
"""Publish a firmware release to the update site (see docs/auto-update-plan.md).

    publish_firmware.py --channel test --notes "Direction for train lines"
    publish_firmware.py --promote 1.3.0           # the test release -> stable
    publish_firmware.py --channel stable --notes "..."

A publish builds the firmware (signed with keys/ota_signing_key.pem), checks
the signature, copies the image into <site>/firmware/, records it in
<site>/releases.json, writes the channel's manifest last (through a rename, so
a display never reads half a manifest), regenerates <site>/index.html and
tags the commit v<version>. The version comes from version.txt and must be
higher than the channel's current one.

A promote points manifest.json (stable) at a release already published,
with the same image: nothing is rebuilt.
"""

import argparse
import datetime
import hashlib
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY = os.path.join(ROOT, "keys", "ota_signing_key.pem")
IMAGE = os.path.join(ROOT, "build", "multi_display.bin")
PROJECT = "multi_display"
MANIFESTS = {"stable": "manifest.json", "test": "manifest-test.json"}
IDF_EXPORT = os.path.expanduser(os.environ.get("IDF_EXPORT", "~/esp/esp-idf/export.sh"))


def die(msg):
    sys.exit("publish_firmware: " + msg)


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, **kw)


def idf(cmd):
    """Run a command with the ESP-IDF environment loaded."""
    run(["bash", "-c", '. "%s" >/dev/null 2>&1 && %s' % (IDF_EXPORT, cmd)])


def version_key(v):
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)$", v or "")
    if not m:
        return None
    return tuple(int(x) for x in m.groups())


def board_name():
    with open(os.path.join(ROOT, "sdkconfig")) as f:
        m = re.search(r'^CONFIG_MULTIDISPLAY_BOARD="([^"]*)"', f.read(), re.M)
    if not m:
        die("CONFIG_MULTIDISPLAY_BOARD not found in sdkconfig (run idf.py reconfigure)")
    return m.group(1)


def image_desc(path):
    """Project name and version from the image's app description."""
    with open(path, "rb") as f:
        head = f.read(24 + 8 + 256)
    desc = head[32:]
    if head[0] != 0xE9 or int.from_bytes(desc[0:4], "little") != 0xABCD5432:
        die("%s is not an app image" % path)
    version = desc[16:48].split(b"\0")[0].decode()
    project = desc[48:80].split(b"\0")[0].decode()
    return project, version


def load_releases(site):
    path = os.path.join(site, "releases.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return {"releases": [], "channels": {}}


def write_atomic(path, text):
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".tmp-")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    os.chmod(tmp, 0o644)
    os.replace(tmp, path)


def write_manifest(site, channel, rel, board):
    manifest = {
        "project": PROJECT,
        "board": board,
        "version": rel["version"],
        "url": "firmware/" + rel["file"],
        "size": rel["size"],
        "sha256": rel["sha256"],
        "released": rel["released"],
        "notes": rel["notes"],
    }
    write_atomic(os.path.join(site, MANIFESTS[channel]), json.dumps(manifest, indent=2) + "\n")


def write_index(site, data):
    e = html.escape
    chans = data["channels"]
    rows = []
    for r in sorted(data["releases"], key=lambda r: version_key(r["version"]), reverse=True):
        tags = " ".join('<span class="tag %s">%s</span>' % (c, {"stable": "stabil", "test": "test"}[c])
                        for c in ("stable", "test")
                        if chans.get(c) == r["version"])
        rows.append('<tr><td><a href="firmware/%s">%s</a> %s</td><td>%s</td><td>%s</td>'
                    '<td class="sha">%s</td></tr>'
                    % (e(r["file"]), e(r["version"]), tags, e(r["released"]), e(r["notes"]),
                       e(r["sha256"][:12])))
    page = """<!doctype html>
<html lang="no"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MultiDisplay programvare</title>
<style>
:root{--bg:#f6f6f4;--fg:#222;--muted:#666;--line:#ddd;--accent:#2d5a86;--test:#a05a00}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#eee;--muted:#999;--line:#333;--accent:#7fb0e0;--test:#e0a050}}
body{font-family:system-ui,sans-serif;max-width:52rem;margin:2rem auto;padding:0 1rem;background:var(--bg);color:var(--fg)}
h1{font-size:1.4rem}a{color:var(--accent)}code{font-size:.9em}
table{width:100%%;border-collapse:collapse;margin-top:1rem}
td,th{text-align:left;padding:.45rem .5rem;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--muted);font-weight:600}.sha{font-family:monospace;color:var(--muted)}
.tag{font-size:.75rem;padding:.05rem .4rem;border-radius:.3rem;border:1px solid currentColor}
.tag.stable{color:var(--accent)}.tag.test{color:var(--test)}
</style>
<h1>MultiDisplay programvare</h1>
<p>Stabil: <b>%s</b> &middot; Test: <b>%s</b></p>
<p>Bruk en av disse som oppdateringsadresse p&aring; skjermens oppsettside:<br>
<code>manifest.json</code> for stabile versjoner, <code>manifest-test.json</code> for testversjoner.
Hver fil nedenfor kan ogs&aring; installeres for h&aring;nd med opplastingen p&aring; oppsettsiden.</p>
<table><tr><th>Versjon</th><th>Utgitt</th><th>Endringer</th><th>SHA-256</th></tr>
%s
</table>
</html>
""" % (e(chans.get("stable", "ingen")), e(chans.get("test", "ingen")), "\n".join(rows))
    write_atomic(os.path.join(site, "index.html"), page)


def check_key_untracked():
    tracked = run(["git", "ls-files", "keys"], capture_output=True).stdout.strip()
    if tracked:
        die("files under keys/ are tracked by git - remove them from git first:\n" + tracked)
    if not os.path.exists(KEY):
        die("no signing key at %s" % KEY)


def publish(args, site, data, board):
    with open(os.path.join(ROOT, "version.txt")) as f:
        version = f.read().strip()
    if version_key(version) is None:
        die("version.txt must hold MAJOR.MINOR.PATCH, not %r" % version)
    current = data["channels"].get(args.channel)
    if current and version_key(version) <= version_key(current):
        die("version.txt says %s, but %s already has %s - raise version.txt" % (version, args.channel, current))
    if any(r["version"] == version for r in data["releases"]):
        die("%s was published before; published versions are never replaced" % version)

    dirty = run(["git", "status", "--porcelain"], capture_output=True).stdout.strip()
    if dirty and not args.allow_dirty:
        die("the working tree has uncommitted changes (commit them, or --allow-dirty for a trial)")
    tag = "v" + version
    if not dirty and run(["git", "tag", "-l", tag], capture_output=True).stdout.strip():
        die("the tag %s exists already" % tag)

    print("Building %s..." % version)
    idf("idf.py build")
    idf('espsecure.py verify_signature --version 2 --keyfile "%s" "%s"' % (KEY, IMAGE))
    project, built = image_desc(IMAGE)
    if project != PROJECT or built != version:
        die("the image says %s %s, expected %s %s" % (project, built, PROJECT, version))

    name = "%s-%s.bin" % (PROJECT, version)
    os.makedirs(os.path.join(site, "firmware"), exist_ok=True)
    dest = os.path.join(site, "firmware", name)
    shutil.copyfile(IMAGE, dest + ".tmp")
    os.chmod(dest + ".tmp", 0o644)
    os.replace(dest + ".tmp", dest)
    with open(dest, "rb") as f:
        blob = f.read()
    commit = run(["git", "rev-parse", "--short", "HEAD"], capture_output=True).stdout.strip()
    rel = {
        "version": version,
        "released": datetime.date.today().isoformat(),
        "notes": args.notes,
        "file": name,
        "size": len(blob),
        "sha256": hashlib.sha256(blob).hexdigest(),
        "commit": commit + ("-dirty" if dirty else ""),
    }
    data["releases"].append(rel)
    data["channels"][args.channel] = version
    write_atomic(os.path.join(site, "releases.json"), json.dumps(data, indent=2) + "\n")
    write_manifest(site, args.channel, rel, board)
    write_index(site, data)
    if dirty:
        print("Not tagged: the working tree has uncommitted changes.")
    else:
        run(["git", "tag", "-a", tag, "-m", "Firmware %s: %s" % (version, args.notes)])
        print("Tagged %s (push it with: git push origin %s)" % (tag, tag))
    print("Published %s to %s (%d bytes, sha256 %s)" % (version, args.channel, rel["size"], rel["sha256"]))


def promote(args, site, data, board):
    version = args.promote
    rel = next((r for r in data["releases"] if r["version"] == version), None)
    if rel is None:
        die("%s has not been published" % version)
    current = data["channels"].get("stable")
    if current and version_key(version) <= version_key(current):
        die("stable already has %s" % current)
    path = os.path.join(site, "firmware", rel["file"])
    with open(path, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != rel["sha256"]:
            die("%s no longer matches its recorded SHA-256" % path)
    data["channels"]["stable"] = version
    write_atomic(os.path.join(site, "releases.json"), json.dumps(data, indent=2) + "\n")
    write_manifest(site, "stable", rel, board)
    write_index(site, data)
    print("Promoted %s to stable" % version)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--site", default="/srv/multidisplay", help="the site folder (default %(default)s)")
    ap.add_argument("--channel", choices=MANIFESTS.keys(), help="publish version.txt's version to this channel")
    ap.add_argument("--notes", help="release notes (required with --channel)")
    ap.add_argument("--promote", metavar="VERSION", help="make a published release the stable one")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="publish from a tree with uncommitted changes (not tagged; for trials)")
    args = ap.parse_args()
    if bool(args.channel) == bool(args.promote):
        ap.error("give either --channel or --promote")
    if args.channel and not (args.notes or "").strip():
        ap.error("--notes is required")

    site = os.path.abspath(args.site)
    if not os.path.isdir(site) or not os.access(site, os.W_OK):
        die("%s must be an existing folder you can write to" % site)
    check_key_untracked()
    board = board_name()
    data = load_releases(site)
    if args.channel:
        publish(args, site, data, board)
    else:
        promote(args, site, data, board)


if __name__ == "__main__":
    main()
