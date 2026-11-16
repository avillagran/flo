#!/usr/bin/env python3
"""A portal client that stays on the bus until its request is answered, as a
real application does (xdg-desktop-portal closes the requests of callers
that leave). Calls org.freedesktop.portal.FileChooser on the session bus and
prints the Response: "response N" and one "uri U" line per URI.

    portal-client.py open|save TITLE [current_folder] [current_name]
"""
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

kind, title = sys.argv[1], sys.argv[2]
folder = sys.argv[3] if len(sys.argv) > 3 else ""
name = sys.argv[4] if len(sys.argv) > 4 else ""
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
token = "flocheck"
sender = bus.get_unique_name()[1:].replace(".", "_")
handle = f"/org/freedesktop/portal/desktop/request/{sender}/{token}"
loop = GLib.MainLoop()


def on_response(_conn, _sender, _path, _iface, _signal, params):
    response, results = params.unpack()
    print(f"response {response}", flush=True)
    for u in results.get("uris", []):
        print(f"uri {u}", flush=True)
    loop.quit()


bus.signal_subscribe("org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response", handle,
                     None, Gio.DBusSignalFlags.NONE, on_response)
opts = {"handle_token": GLib.Variant("s", token)}
if folder:
    opts["current_folder"] = GLib.Variant("ay", folder.encode() + b"\0")
if name:
    opts["current_name"] = GLib.Variant("s", name)
method = "OpenFile" if kind == "open" else "SaveFile"
bus.call_sync("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser",
              method, GLib.Variant("(ssa{sv})", ("", title, opts)), None, Gio.DBusCallFlags.NONE, -1, None)
GLib.timeout_add_seconds(60, loop.quit)
loop.run()
