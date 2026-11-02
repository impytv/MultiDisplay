#!/usr/bin/env python3
"""Save the radio stations Sonos can pause (server/radio/) as a Sonos
playlist, so they can be started from the Sonos app too: there they show
Pause instead of Stop, and Play goes on from where it was.

    python3 scripts/sonos_radio_playlist.py SPEAKER_IP [SITE]

SPEAKER_IP is any speaker of the home (the playlist is the whole home's).
SITE is the update site on the home network, by default
http://192.168.0.119:8070. The playlist is named PLAYLIST below, the
stations in server/radio/stations.json's order (NRK first), each with its
logo from the site's bruk/radio/. An earlier playlist of that name is
replaced; no other is touched.

A station added as a radio station in the Sonos app (TuneIn's "Add Radio
Station") can't be used for this: Sonos plays those as a radio, with Stop
only, whatever server is behind the address.
"""
import json
import os
import re
import sys
import urllib.request
from xml.sax.saxutils import escape

PLAYLIST = "Radio med pause"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def soap(ip, service, path, action, args):
    body = (
        '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
        f'<u:{action} xmlns:u="urn:schemas-upnp-org:service:{service}:1">{args}</u:{action}>'
        "</s:Body></s:Envelope>"
    )
    req = urllib.request.Request(
        f"http://{ip}:1400{path}",
        data=body.encode(),
        headers={
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPACTION": f'"urn:schemas-upnp-org:service:{service}:1#{action}"',
        },
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read().decode()


def field(xml, name):
    m = re.search(f"<{name}>([^<]*)</{name}>", xml)
    return m.group(1) if m else ""


def av(ip, action, args):
    return soap(ip, "AVTransport", "/MediaRenderer/AVTransport/Control", action, "<InstanceID>0</InstanceID>" + args)


def meta(name, art):
    return escape(
        '<DIDL-Lite xmlns:dc="http://purl.org/dc/elements/1.1/" '
        'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/" '
        'xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/">'
        f'<item id="-1" parentID="-1" restricted="true"><dc:title>{escape(name)}</dc:title>'
        f"<upnp:albumArtURI>{escape(art)}</upnp:albumArtURI>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class></item></DIDL-Lite>"
    )


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ip = sys.argv[1]
    site = (sys.argv[2] if len(sys.argv) > 2 else "http://192.168.0.119:8070").rstrip("/")
    stations = json.load(open(os.path.join(ROOT, "server", "radio", "stations.json")))

    # The earlier one, by its name.
    found = soap(
        ip,
        "ContentDirectory",
        "/MediaServer/ContentDirectory/Control",
        "Browse",
        "<ObjectID>SQ:</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>dc:title</Filter>"
        "<StartingIndex>0</StartingIndex><RequestedCount>500</RequestedCount><SortCriteria></SortCriteria>",
    )
    didl = field(found, "Result").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
    for sq, title in re.findall(r'<container id="(SQ:\d+)"[^>]*>.*?<dc:title>([^<]*)</dc:title>', didl):
        if title.replace("&amp;", "&") == PLAYLIST:
            soap(
                ip,
                "ContentDirectory",
                "/MediaServer/ContentDirectory/Control",
                "DestroyObject",
                f"<ObjectID>{sq}</ObjectID>",
            )
            print(f"Replaced {sq}")

    keys = list(stations)
    first = keys[0]
    made = av(
        ip,
        "CreateSavedQueue",
        f"<Title>{escape(PLAYLIST)}</Title><EnqueuedURI>{site}/radio/{first}.mp3</EnqueuedURI>"
        f"<EnqueuedURIMetaData>{meta(stations[first]['name'], f'{site}/bruk/radio/{first}.jpg')}"
        "</EnqueuedURIMetaData>",
    )
    sq, update = field(made, "AssignedObjectID"), field(made, "NewUpdateID")
    for key in keys[1:]:
        added = av(
            ip,
            "AddURIToSavedQueue",
            f"<ObjectID>{sq}</ObjectID><UpdateID>{update}</UpdateID>"
            f"<EnqueuedURI>{site}/radio/{key}.mp3</EnqueuedURI>"
            f"<EnqueuedURIMetaData>{meta(stations[key]['name'], f'{site}/bruk/radio/{key}.jpg')}"
            "</EnqueuedURIMetaData><AddAtIndex>4294967295</AddAtIndex>",
        )
        update = field(added, "NewUpdateID")
    print(f"{PLAYLIST}: {sq}, {len(keys)} stations")


if __name__ == "__main__":
    main()
