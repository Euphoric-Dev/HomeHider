#!/usr/bin/env python3
"""Desktop test suite for Home Hider.

Builds a mock PS5 app.db (two users, MyHome groups, a rowid-alias primary key
on the group table), runs the host build of the payload, and drives it over
HTTP exactly like the web menu does.
"""
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

BIN = os.path.abspath(sys.argv[1])
PORT = 8090
BASE = "http://127.0.0.1:%d" % PORT
USERS = ["1eb71bbd", "2c4f0a11"]
STORE, GALLERY, LIBRARY, PLUS = "NPXS40047", "NPXS40075", "NPXS40071", "NPXS40037"

passed = 0


def check(cond, msg):
    global passed
    if not cond:
        raise AssertionError(msg)
    passed += 1
    print("  ok  " + msg)


# (titleId, titleName, dispLocation, visible, installStatus, conceptName, icon0Info)
APPS = [
    (STORE, "PlayStation Store", 0x8A, 1, 2, None, None),
    (GALLERY, "Media Gallery", 0x8A, 1, 2, None, None),
    (LIBRARY, "Game Library", 0x8A, 1, 2, None, None),
    (PLUS, "PlayStation Plus", 0x8A, 1, 2, None, None),
    ("PPSA01325", "Astro Playroom", 0x8A, 1, 2, "ASTRO's PLAYROOM", "/user/appmeta/PPSA01325/icon0.png?ts=1"),
    ("CUSA00900", "Some PS4 Game", 0x8A, 1, 2, None, "/etc/passwd"),
    ("NPXS40053", "TV & Video", 0xBC, 1, 2, None, None),
    ("CUSA01065", "Netflix", 0xBC, 1, 2, None, "/user/appmeta/../../etc/passwd"),
    ("PPSA99999", "Uninstalled Game", 0x8A, 0, 0, None, None),
    ("NPXS40140", "Disc placeholder", 0x8A, 1, 2, None, None),
    ("NPXS40085", "Small Diag", 0x00, 0, 2, None, None),
    ("PPSA02000", "Library Only Game", 0x00, 1, 2, None, None),
    ("NPXS40139", "App Library", 0xBC, 1, 2, None, None),
    ("NPXS40056", "All Apps", 0xBC, 1, 2, None, None),
]
EXPECTED = {STORE, GALLERY, PLUS, "PPSA01325", "CUSA00900", "NPXS40053", "CUSA01065", "PPSA02000", "NPXS40056"}


def make_db(path, layout="normal"):
    con = sqlite3.connect(path)
    c = con.cursor()
    c.execute("CREATE TABLE tbl_version(category TEXT PRIMARY KEY, status INTEGER)")
    c.execute("INSERT INTO tbl_version VALUES('access_index', 42), ('played_index', 7)")
    c.execute("CREATE TABLE tbl_contentinfo(titleId TEXT PRIMARY KEY, titleName TEXT, "
              "dispLocation INTEGER, AppInfoJson TEXT)")
    for a in APPS:
        c.execute("INSERT INTO tbl_contentinfo VALUES(?,?,?,?)", (a[0], a[1], a[2], "{}"))
    if layout == "unknown":
        con.commit()
        con.close()
        return
    for n, u in enumerate(USERS):
        c.execute(f'CREATE TABLE "tbl_iconinfo_{u}"(titleId TEXT PRIMARY KEY, titleName TEXT, '
                  'localConceptId TEXT, dispLocation INTEGER, visible INTEGER, pinnedStatus INTEGER, '
                  'lastAccessIndex INTEGER, installStatus INTEGER, emsDeletedFlag INTEGER)')
        c.execute(f'CREATE TABLE "tbl_concepticoninfo_{u}"(localConceptId TEXT, conceptName TEXT, icon0Info TEXT)')
        for i, (tid, name, disp, vis, inst, cname, icon) in enumerate(APPS):
            c.execute(f'INSERT INTO "tbl_iconinfo_{u}" VALUES(?,?,?,?,?,?,?,?,?)',
                      (tid, name, "cid:local:" + tid, disp, vis, 2, 40 - i, inst, 0))
            c.execute(f'INSERT INTO "tbl_concepticoninfo_{u}" VALUES(?,?,?)', ("cid:local:" + tid, cname, icon))
        c.execute(f'CREATE TABLE "tbl_group_{u}"(id INTEGER PRIMARY KEY, itemId TEXT, '
                  'groupName TEXT, type TEXT, idx INTEGER, lastModifiedDate TEXT, '
                  'UNIQUE(itemId, groupName))')
        for idx, tid in enumerate([PLUS, STORE, LIBRARY], start=1):
            c.execute(f'INSERT INTO "tbl_group_{u}"(itemId, groupName, type, idx, lastModifiedDate) '
                      "VALUES(?, 'MyHome', 'title', ?, '2026-01-0%d')" % idx, (tid, idx))
        # an unrelated collection that must never be touched
        c.execute(f'INSERT INTO "tbl_group_{u}"(itemId, groupName, type, idx) '
                  "VALUES(?, 'Collection_1', 'title', 1)", (STORE,))
    con.commit()
    con.close()


def dump(path, skip_pk=True):
    """Canonical content of every table (rowid-alias ids dropped)."""
    con = sqlite3.connect(path)
    out = {}
    for (name,) in con.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
        cols = [r[1] for r in con.execute(f'PRAGMA table_info("{name}")')
                if not (skip_pk and r[5] == 1 and r[2].upper() == "INTEGER")]
        rows = con.execute(f'SELECT {",".join(chr(34)+c+chr(34) for c in cols)} FROM "{name}"').fetchall()
        out[name] = sorted(rows, key=repr)
    con.close()
    return out


def q(path, sql, *args):
    con = sqlite3.connect(path)
    r = con.execute(sql, args).fetchall()
    con.close()
    return r


def qx(path, sql, *args):
    con = sqlite3.connect(path)
    con.execute(sql, args)
    con.commit()
    con.close()


def http(method, path, raw=False):
    req = urllib.request.Request(BASE + path, method=method, data=b"" if method == "POST" else None)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read()
            return r.status, (body if raw else json.loads(body))
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return e.code, (body if raw else json.loads(body))
        except ValueError:
            return e.code, body


SAFE_ENV = {"HH_PLDMGR_DIR": "/nonexistent/pldmgr", "HH_USB_FMT": "/nonexistent/usb%d",
            "HH_LOADER_PORT": "1", "HH_GITHUB_API": "http://127.0.0.1:1"}


class Server:
    def __init__(self, appdb, datadir, env_extra=None, wait=True):
        env = dict(os.environ, HH_APP_DB=appdb, HH_DATA_DIR=datadir,
                   ASAN_OPTIONS="detect_leaks=1", **SAFE_ENV)
        env.update(env_extra or {})
        self.p = subprocess.Popen([BIN], env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True)
        self.out = []
        threading.Thread(target=self._pump, daemon=True).start()
        if not wait:
            return
        end = time.time() + 60  # repairs at start (backgrounds) can take a few seconds
        while time.time() < end:
            try:
                urllib.request.urlopen(BASE + "/api/status", timeout=1).read()
                return
            except Exception:
                if self.p.poll() is not None:
                    break
                time.sleep(0.05)
        raise RuntimeError("server did not start:\n" + "".join(self.out))

    def _pump(self):
        for line in self.p.stdout:
            self.out.append(line)

    def stop(self):
        if self.p.poll() is None:
            http("POST", "/api/quit")
            self.p.wait(timeout=10)
        log = "".join(self.out)
        assert "ERROR: AddressSanitizer" not in log and "runtime error" not in log, log
        return log


def listing():
    code, j = http("GET", "/api/list")
    assert code == 200 and j["ok"], j
    return {t["id"]: t for t in j["tiles"]}, j["tiles"]


def tile(_unused, tid):
    return listing()[0][tid]


# ---------------------------------------------------------------------------
# locked tiles hidden by an older version are put back
# ---------------------------------------------------------------------------

def locked_migration_tests(tmp):
    print("locked tiles hidden by v0.2 are restored at launch")
    appdb = os.path.join(tmp, "mig.db")
    data = os.path.join(tmp, "mig-data")
    make_db(appdb)
    original = dump(appdb)
    s = Server(appdb, data)          # creates state.db
    s.stop()
    st = sqlite3.connect(os.path.join(data, "state.db"))
    con = sqlite3.connect(appdb)
    for u in USERS:
        con.execute(f'UPDATE "tbl_iconinfo_{u}" SET visible=0, dispLocation=? WHERE titleId=?', (0x88, LIBRARY))
        st.execute("INSERT INTO hidden VALUES(?,?,?,?,datetime('now'))", (LIBRARY, u, 1, 0x8A))
    st.execute("INSERT INTO wanted VALUES(?, datetime('now'))", (LIBRARY,))
    con.commit(); st.commit(); con.close(); st.close()
    s = Server(appdb, data)
    code, j = http("GET", "/api/status")
    check(j["released_locked"] == 1, "status reports 1 locked tile released")
    check(dump(appdb) == original, "Game Library row back to its original values")
    check(q(os.path.join(data, "state.db"), "SELECT count(*) FROM wanted")[0][0] == 0, "no longer on the hide list")
    s.stop()


# ---------------------------------------------------------------------------
# updater: fake GitHub, fake USB drive, fake ELF loader
# ---------------------------------------------------------------------------

import hashlib
from http import server as httpserver
import socket
import socketserver
import struct


def fake_elf(version, size=40 * 1024, marker=True):
    hdr = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9 + struct.pack("<HH", 2, 0x3E)
    body = hdr + b"\x00" * 2000
    if marker:
        body += b"junk HOMEHIDER-ELF-VERSION=\x00 more HOMEHIDER-ELF-VERSION=" + version.encode() + b";"
    return body + bytes((i * 7) % 251 for i in range(size - len(body)))


class FakeGitHub:
    def __init__(self, port):
        self.port = port
        self.release = None       # dict or None -> 404
        self.files = {}
        outer = self

        class H(httpserver.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path == "/repos/me/HomeHider/releases/latest" and outer.release is not None:
                    b = json.dumps(outer.release).encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                elif self.path == "/dl/redirect":
                    self.send_response(302)
                    self.send_header("Location", "http://127.0.0.1:%d/dl/file" % outer.port)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                elif self.path in outer.files:
                    b = outer.files[self.path]
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                else:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()

        socketserver.TCPServer.allow_reuse_address = True
        self.srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def publish(self, version, elf, digest=True, bad_digest=False):
        self.files["/dl/file"] = elf
        d = hashlib.sha256(elf).hexdigest()
        if bad_digest:
            d = "0" * 64
        self.release = {
            "tag_name": "v" + version,
            "html_url": "https://github.com/me/HomeHider/releases/tag/v" + version,
            "body": "What\u2019s new:\n- \"quoted\" \\ stuff \ud83c\udfae",
            "author": {"login": "me", "nested": {"a": [1, 2, {"b": None}]}},
            "assets": [
                {"name": "notes.txt", "browser_download_url": "http://x/notes.txt", "uploader": {"login": "me"}},
                {"name": "HomeHider.elf", "url": "http://127.0.0.1:%d/api/asset/1" % self.port,
                 "browser_download_url": "http://127.0.0.1:%d/dl/redirect" % self.port,
                 "uploader": {"login": "me", "id": 5}, "size": len(elf),
                 **({"digest": "sha256:" + d} if digest else {})},
            ],
        }

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


class FakeLoader:
    """Plays the ELF loader: receives the new build, then (like the new
    instance would) asks the running Home Hider to quit."""

    def __init__(self, port):
        self.received = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(4)
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            data = b""
            while True:
                chunk = c.recv(65536)
                if not chunk:
                    break
                data += chunk
            c.close()
            self.received.append(data)
            time.sleep(0.2)
            try:
                http("POST", "/api/quit")
            except Exception:
                pass

    def close(self):
        self.sock.close()


def wait_idle(timeout=20):
    t0 = time.time()
    while time.time() - t0 < timeout:
        code, u = http("GET", "/api/update/status")
        if code == 200 and not u["busy"]:
            return u
        time.sleep(0.1)
    raise AssertionError("updater stayed busy")


def wait_exit(server, timeout=20):
    server.p.wait(timeout=timeout)
    return server.stop()


def updater_tests(tmp):
    print("updater")
    gh_port, loader_port = 18080, 19021
    gh = FakeGitHub(gh_port)
    loader = FakeLoader(loader_port)
    try:
        appdb = os.path.join(tmp, "upd.db")
        data = os.path.join(tmp, "upd-data")
        pld = os.path.join(tmp, "pldmgr", "payloads")
        usb_fmt = os.path.join(tmp, "usb%d")
        os.makedirs(os.path.join(pld, "local", "homehider"))
        os.makedirs(os.path.join(pld, "local", "other"))
        make_db(appdb)
        current = open(BIN, "rb").read()
        old_v02 = fake_elf("0", marker=False) + b"/data/homehider/state.db"
        other = fake_elf("0", marker=False)
        open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "wb").write(current)
        open(os.path.join(pld, "HomeHider-v0.2.0.elf"), "wb").write(old_v02)
        open(os.path.join(pld, "local", "other", "ftpsrv.elf"), "wb").write(other)
        env = {"HH_PLDMGR_DIR": pld, "HH_USB_FMT": usb_fmt, "HH_LOADER_PORT": str(loader_port),
               "HH_GITHUB_API": "http://127.0.0.1:%d" % gh_port}

        cur_ver = re.search(rb"HOMEHIDER-ELF-VERSION=([0-9.]+);", current).group(1).decode()
        s = Server(appdb, data, env)
        code, st = http("GET", "/api/status")
        check(st["marker"] == "HOMEHIDER-ELF-VERSION=%s;" % cur_ver, "build carries its version marker")
        u = wait_idle()
        check(u["current"] == cur_ver and not u["usb"]["newer"] and not u["repo"], "nothing to update at start")

        print("  repository setting")
        code, r = http("POST", "/api/settings?repo=" + urllib.parse.quote("bad repo"))
        check(code == 400, "invalid repository rejected")
        code, r = http("POST", "/api/settings?repo=" + urllib.parse.quote("https://github.com/me/HomeHider.git"))
        check(code == 200, "pasted GitHub URL accepted")
        code, st = http("GET", "/api/status")
        check(st["settings"]["repo"] == "me/HomeHider", "URL normalised to owner/name")

        print("  GitHub: no release yet, then a release")
        http("POST", "/api/update/check")
        u = wait_idle()
        check(not u["github"]["ok"] and "No published release" in u["github"]["error"], "404 explained")
        new = fake_elf("9.9.9")
        gh.publish("9.9.9", new)
        http("POST", "/api/update/check")
        u = wait_idle()
        g = u["github"]
        check(g["ok"] and g["newer"] and g["version"] == "9.9.9", "newer GitHub release found")
        check(g["asset"] == "HomeHider.elf" and g["digest"], "picked the .elf asset with its digest")
        check(g["notes"].startswith("What\u2019s new") and "\U0001F3AE" in g["notes"], "release notes decoded (UTF-8, escapes)")

        print("  GitHub: checksum mismatch is refused")
        gh.publish("9.9.9", new, bad_digest=True)
        http("POST", "/api/update/check"); wait_idle()
        before = {p: open(p, "rb").read() for p in glob_elves(pld)}
        code, r = http("POST", "/api/update/install?source=github")
        check(code == 200, "install started")
        u = wait_idle()
        check(u["phase"] == "error" and "checksum" in u["message"], "bad checksum -> error, nothing installed")
        check({p: open(p, "rb").read() for p in glob_elves(pld)} == before, "payload files untouched")

        print("  GitHub: real install + restart")
        gh.publish("9.9.9", new)
        http("POST", "/api/update/check"); wait_idle()
        code, r = http("POST", "/api/update/install?source=github")
        log = wait_exit(s)
        check(loader.received and loader.received[-1] == new, "new build handed to the ELF loader")
        check(open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "rb").read() == new, "Payload Manager copy replaced")
        check(open(os.path.join(pld, "HomeHider-v0.2.0.elf"), "rb").read() == new, "old v0.2 copy recognised and replaced")
        check(open(os.path.join(pld, "local", "other", "ftpsrv.elf"), "rb").read() == other, "other payloads untouched")
        check(open(os.path.join(data, "HomeHider.elf"), "rb").read() == new, "own copy written")
        bk = os.path.join(data, "backup")
        check(open(os.path.join(bk, "HomeHider-%s.elf" % cur_ver), "rb").read() == current, "previous version backed up")
        check(os.path.exists(os.path.join(bk, "HomeHider-previous.elf")), "v0.2 copy backed up")
        check(os.path.exists(os.path.join(data, ".updated")), "restart flag left for the new instance")
        check(not [f for f in os.listdir(os.path.join(pld, "local", "homehider")) if f.endswith(".hhnew")], "no temp files left")

        print("  first start after an update")
        s = Server(appdb, data, dict(env, HH_GITHUB_API="http://127.0.0.1:1"))
        code, st = http("GET", "/api/status")
        check(st["updated_to"] == cur_ver, "status says it was just updated")
        check(not any("would open" in l for l in s.out), "browser not reopened after an update")
        check(not os.path.exists(os.path.join(data, ".updated")), "restart flag consumed")
        u = wait_idle()
        check(not u["github"]["checked"], "no automatic check right after updating")
        http("POST", "/api/update/check")
        u = wait_idle()
        check(not u["github"]["ok"] and "Couldn't reach GitHub" in (u["github"]["error"] or ""), "unreachable GitHub explained")

        print("  USB drive")
        os.makedirs(usb_fmt % 0 + "/homehider")
        os.makedirs(usb_fmt % 1)
        usb_new = fake_elf("9.9.10")
        open(usb_fmt % 0 + "/homehider/HomeHider-v9.9.10.elf", "wb").write(usb_new)
        open(usb_fmt % 1 + "/HomeHider-old.elf", "wb").write(fake_elf("0.0.1"))
        open(usb_fmt % 1 + "/random.elf", "wb").write(other)
        http("POST", "/api/update/check?usb_only=1")
        u = wait_idle()
        check(u["usb"]["newer"] and u["usb"]["version"] == "9.9.10", "newest USB build found")
        code, r = http("POST", "/api/update/install?source=usb")
        wait_exit(s)
        check(loader.received[-1] == usb_new, "USB build handed to the loader")
        check(open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "rb").read() == usb_new, "USB build installed")

        print("  upload from a phone/PC")
        s = Server(appdb, data, env)
        wait_idle()
        def upload(body, q=""):
            req = urllib.request.Request(BASE + "/api/update/upload" + q, method="POST", data=body,
                                         headers={"Origin": BASE})
            try:
                with urllib.request.urlopen(req, timeout=30) as r:
                    return r.status, json.loads(r.read())
            except urllib.error.HTTPError as e:
                return e.code, json.loads(e.read())
        code, r = upload(b"not an elf at all" * 3000)
        check(code == 409 and "isn't a PS5 payload" in r["error"], "garbage upload refused")
        code, r = upload(other)
        check(code == 409 and "isn't a Home Hider build" in r["error"], "other payload refused")
        code, r = upload(fake_elf("0.0.1"))
        check(code == 409 and "Allow older" in r["error"], "older build needs explicit permission")
        evil = urllib.request.Request(BASE + "/api/update/upload", method="POST", data=fake_elf("9.9.12"),
                                      headers={"Origin": "http://evil.example"})
        try:
            urllib.request.urlopen(evil, timeout=10); code = 200
        except urllib.error.HTTPError as e:
            code = e.code
        check(code == 403, "cross-site upload refused")
        up = fake_elf("9.9.11")
        code, r = upload(up)
        check(code == 200 and "9.9.11" in r["note"], "upload installed")
        wait_exit(s)
        check(loader.received[-1] == up, "uploaded build handed to the loader")
        check(open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "rb").read() == up, "uploaded build installed")

        print("  loader not running")
        s = Server(appdb, data, dict(env, HH_LOADER_PORT="1"))
        wait_idle()
        code, r = upload(fake_elf("9.9.13"))
        check(code == 200, "install still succeeds")
        time.sleep(1.0)
        u = wait_idle()
        check(u["phase"] == "done" and "start it again from Payload Manager" in u["message"], "asks to restart manually")
        s.stop()
    finally:
        gh.close()
        loader.close()


# ---------------------------------------------------------------------------
# folders
# ---------------------------------------------------------------------------

import zlib


def tiny_png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def png_size(path):
    d = open(path, "rb").read(32)
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    return struct.unpack(">II", d[16:24])


def form(path, fields):
    body = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(BASE + path, method="POST", data=body,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def post_bytes(path, data):
    req = urllib.request.Request(BASE + path, method="POST", data=data)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def tile_rows(appdb, tid):
    return [q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid) for u in USERS]


def vis(appdb, tid):
    return [q(appdb, f'SELECT visible FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)[0][0] for u in USERS]


def folder_tests(tmp):
    print("folders")
    appdb = os.path.join(tmp, "fold.db")
    data = os.path.join(tmp, "fold-data")
    apps = os.path.join(tmp, "user-app")
    icons = os.path.join(tmp, "icons")
    os.makedirs(icons)
    for i, tid in enumerate(["PPSA01325", "CUSA00900", "CUSA01065"]):
        open(os.path.join(icons, tid + ".png"), "wb").write(tiny_png(64 + i * 10, 64 + i * 10, (200, 40 * i, 90)))
    make_db(appdb)
    original = dump(appdb)
    env = {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons}
    s = Server(appdb, data, env)

    # the user hid one game on their own before making the folder
    http("POST", "/api/hide?id=CUSA00900")

    print("  create")
    code, r = form("/api/folders/save", {"name": "", "items": "PPSA01325"})
    check(code == 409 and "name" in r["error"], "empty name refused")
    code, r = form("/api/folders/save", {"name": "x" * 60, "items": "PPSA01325"})
    check(code == 409, "over-long name refused")
    name = 'Sports & "Racing" ★'
    code, r = form("/api/folders/save", {"name": name, "tab": "games", "color": "red",
                                         "items": "PPSA01325,CUSA00900,BOGUS,NPXS40071,PPSA01325,HHFD00001"})
    check(code == 200 and r["id"] == "HHFD00001", "folder created as HHFD00001")
    f = r["folder"]
    check([i["id"] for i in f["items"]] == ["PPSA01325", "CUSA00900"], "invalid/duplicate/locked items dropped, order kept")
    check(f["name"] == name and f["installed"], "name stored, tile installed")
    pj = json.load(open(os.path.join(apps, "HHFD00001", "sce_sys", "param.json")))
    check(pj["titleId"] == "HHFD00001" and pj["deeplinkUri"] == "http://127.0.0.1:8090/folder/HHFD00001",
          "param.json points the tile at the folder page")
    check(pj["localizedParameters"]["en-US"]["titleName"] == name, "tile name survives quotes and unicode")
    check(png_size(os.path.join(apps, "HHFD00001", "sce_sys", "icon0.png")) == (512, 512), "collage icon0.png is 512x512")
    rows = tile_rows(appdb, "HHFD00001")
    check(all(r_ and r_[0][0] == 1 and r_[0][1] & 2 and not r_[0][1] & 4 for r_ in rows), "tile visible on the Games tab")
    check(vis(appdb, "PPSA01325") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], "games inside are hidden")
    code, d = http("GET", "/api/diag", raw=True)
    d = d.decode()
    check("folder HHFD00001: app install returned 0x00000000" in d and "placed on games tab" in d,
          "folder steps written to the log shown in Diagnostics")
    check("HTTP 409 on POST /api/folders/save" in d, "refused requests logged with their reason")
    http("POST", "/api/clientlog?msg=" + urllib.parse.quote("TypeError: x is undefined @12"))
    check("menu: TypeError: x is undefined @12" in http("GET", "/api/diag", raw=True)[1].decode(),
          "menu script errors reach the log")

    print("  list and pages")
    by_id, _ = listing()
    check("HHFD00001" not in by_id, "folder tile not offered as a game")
    check(by_id["PPSA01325"]["folder"] == "HHFD00001" and by_id["CUSA01065"]["folder"] is None, "membership reported")
    code, r = http("POST", "/api/unhide?id=PPSA01325")
    check(code == 409 and "folder" in r["error"], "showing a game that's in a folder is refused")
    code, r = http("POST", "/api/hide?id=HHFD00001")
    check(code == 409, "folder tile can't be hidden like a game")
    code, page = http("GET", "/folder/HHFD00001", raw=True)
    check(code == 200 and b"Sort by" in page, "folder page served")
    code, j = http("GET", "/api/folder?id=HHFD00001")
    check(j["folder"]["items"][0]["name"] == "ASTRO's PLAYROOM", "folder API returns names")
    code, j = http("GET", "/api/folder?id=HHFD00099")
    check(code == 404, "unknown folder -> 404")
    code, png = http("GET", "/api/folder_icon?id=HHFD00001", raw=True)
    check(code == 200 and png[:4] == b"\x89PNG", "folder icon served")

    print("  edit")
    code, r = form("/api/folders/save", {"id": "HHFD00001", "name": "Sports", "tab": "media", "color": "teal",
                                         "items": "CUSA00900,CUSA01065"})
    check(code == 200, "folder edited")
    check(vis(appdb, "PPSA01325") == [1, 1], "game taken out is shown again")
    check(vis(appdb, "CUSA01065") == [0, 0], "game added is hidden")
    rows = tile_rows(appdb, "HHFD00001")
    check(all(r_[0][1] & 4 and not r_[0][1] & 2 for r_ in rows), "tile moved to the Media tab")
    code, r = form("/api/folders/save", {"name": "Favourites", "items": "CUSA01065,PPSA02000"})
    check(code == 200 and r["id"] == "HHFD00002", "second folder created")
    code, j = http("GET", "/api/folder?id=HHFD00001")
    check([i["id"] for i in j["folder"]["items"]] == ["CUSA00900"], "game moved out of the first folder")
    check(vis(appdb, "CUSA01065") == [0, 0], "moved game stays hidden")

    print("  pictures")
    before = open(os.path.join(apps, "HHFD00002", "sce_sys", "icon0.png"), "rb").read()
    code, r = post_bytes("/api/folders/icon?id=HHFD00002", b"definitely not an image" * 10)
    check(code == 409 and "picture" in r["error"], "bad picture refused")
    code, r = post_bytes("/api/folders/icon?id=HHFD00002", tiny_png(800, 400, (10, 200, 30)))
    check(code == 200, "custom picture accepted")
    after = open(os.path.join(apps, "HHFD00002", "sce_sys", "icon0.png"), "rb").read()
    check(after != before and png_size(os.path.join(apps, "HHFD00002", "sce_sys", "icon0.png")) == (512, 512),
          "picture cropped to a 512x512 tile icon")
    code, j = http("GET", "/api/folder?id=HHFD00002")
    check(j["folder"]["custom_icon"], "custom picture remembered")
    code, r = http("POST", "/api/folders/icon?id=HHFD00002&reset=1")
    check(code == 200 and not http("GET", "/api/folder?id=HHFD00002")[1]["folder"]["custom_icon"], "back to collage")

    print("  show everything / repair at launch")
    code, r = http("POST", "/api/unhide_all")
    check(vis(appdb, "CUSA01065") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], "folder games stay hidden")
    s.stop()
    con = sqlite3.connect(appdb)
    for u in USERS:
        con.execute(f'DELETE FROM "tbl_iconinfo_{u}" WHERE titleId=?', ("HHFD00002",))
        con.execute(f'UPDATE "tbl_iconinfo_{u}" SET dispLocation=138 WHERE titleId=?', ("HHFD00001",))
    con.commit()
    con.close()
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    check(st["folders_fixed"] == 2, "missing tile reinstalled and moved tile put back")
    check(all(r_ and r_[0][1] & 4 for r_ in tile_rows(appdb, "HHFD00001")), "tile back on the Media tab")
    check(all(r_ for r_ in tile_rows(appdb, "HHFD00002")), "deleted tile reinstalled")

    print("  delete")
    code, r = http("POST", "/api/folders/delete?id=HHFD00002")
    check(code == 200, "folder deleted")
    check(all(not r_ for r_ in tile_rows(appdb, "HHFD00002")), "tile removed")
    check(not os.path.exists(os.path.join(apps, "HHFD00002")), "tile files removed")
    check(vis(appdb, "CUSA01065") == [1, 1] and vis(appdb, "PPSA02000") == [1, 1], "its games are shown again")
    code, r = http("POST", "/api/folders/delete?id=HHFD00001")
    check(vis(appdb, "CUSA00900") == [0, 0], "game hidden before joining stays hidden")
    http("POST", "/api/unhide?id=CUSA00900")
    check(dump(appdb) == original, "database back to the original after folders come and go")

    print("  restore original backup removes folder tiles")
    form("/api/folders/save", {"name": "Temp", "items": "PPSA01325"})
    check(all(r_ for r_ in tile_rows(appdb, "HHFD00001")), "temp folder installed")
    code, r = http("POST", "/api/restore")
    check(code == 200 and dump(appdb) == original, "original restored")
    check(not os.path.exists(os.path.join(apps, "HHFD00001")), "folder tile files removed")
    code, j = http("GET", "/api/folders")
    check(j["folders"] == [], "folder list cleared")
    s.stop()


# ---------------------------------------------------------------------------
# pictures from USB drives and the internal pictures folder
# ---------------------------------------------------------------------------

try:
    from PIL import Image, ImageDraw
except ImportError:  # the EXIF checks need Pillow to make the test pictures
    Image = None


def exif_jpeg(path, orientation):
    """200x100 picture stored left red / right blue, with an EXIF rotation."""
    im = Image.new("RGB", (200, 100), (255, 0, 0))
    im.paste((0, 0, 255), (100, 0, 200, 100))
    exif = Image.Exif()
    exif[0x0112] = orientation
    im.save(path, "JPEG", quality=95, exif=exif.tobytes())


def px(png_bytes, x, y):
    import io
    return Image.open(io.BytesIO(png_bytes)).convert("RGB").getpixel((x, y))


def near(c, want, tol=60):
    return all(abs(a - b) <= tol for a, b in zip(c, want))


def wait_for(pred, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.1)
    return pred()


def picture_tests(tmp):
    print("folder pictures from USB / internal storage")
    appdb = os.path.join(tmp, "pics.db")
    data = os.path.join(tmp, "pics-data")
    apps = os.path.join(tmp, "pics-apps")
    usb0 = os.path.join(tmp, "usb0")
    usb3 = os.path.join(tmp, "usb3")
    outside = os.path.join(tmp, "outside")
    make_db(appdb)

    def put(path, data_):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(data_)

    put(os.path.join(usb0, "Cover.PNG"), tiny_png(300, 200, (0, 160, 0)))
    put(os.path.join(usb0, "notes.txt"), b"not a picture")
    put(os.path.join(usb0, "._Cover.png"), b"mac junk")
    put(os.path.join(usb0, "System Volume Information", "x.png"), tiny_png(8, 8, (1, 1, 1)))
    put(os.path.join(usb0, "a", "b", "c", "d", "deep4.png"), tiny_png(8, 8, (9, 9, 9)))
    put(os.path.join(usb0, "a", "b", "c", "d", "e", "deep5.png"), tiny_png(8, 8, (9, 9, 9)))
    put(os.path.join(usb3, "art.bmp"), b"")  # empty file: skipped
    put(os.path.join(outside, "secret.png"), tiny_png(8, 8, (5, 5, 5)))
    shot_dir = os.path.join(usb0, "PS5", "CREATE", "Screenshots", "ASTRO's PLAYROOM")
    os.makedirs(shot_dir)
    if Image:
        exif_jpeg(os.path.join(shot_dir, "rotated.jpg"), 6)
        exif_jpeg(os.path.join(shot_dir, "upright.jpg"), 1)
    else:
        put(os.path.join(shot_dir, "rotated.jpg"), tiny_png(10, 10, (0, 0, 0)))
    # a picture copied over FTP to the internal pictures folder
    put(os.path.join(data, "pictures", "ftp.png"), tiny_png(64, 64, (250, 200, 0)))

    s = Server(appdb, data, {"HH_USB_FMT": os.path.join(tmp, "usb%d"), "HH_USER_APP_DIR": apps})
    code, j = http("GET", "/api/usb_images")
    check(code == 200 and j["ok"], "picture list served")
    found = {os.path.relpath(p["path"], tmp): p for p in j["images"]}
    want = {"usb0/Cover.PNG", "usb0/a/b/c/d/deep4.png", "pics-data/pictures/ftp.png",
            "usb0/PS5/CREATE/Screenshots/ASTRO's PLAYROOM/rotated.jpg"}
    check(want <= set(found), "pictures found at the top, in PS5 screenshot folders and 4 folders deep")
    check(not any(k.endswith(("notes.txt", "._Cover.png", "deep5.png", "art.bmp")) or "System Volume" in k
                  for k in found), "non-pictures, hidden files, empty files and 5-deep folders skipped")
    shot = found["usb0/PS5/CREATE/Screenshots/ASTRO's PLAYROOM/rotated.jpg"]
    check(shot["folder"] == "PS5/CREATE/Screenshots/ASTRO's PLAYROOM" and shot["name"] == "rotated.jpg",
          "folder and name reported for the picker")
    check(found["pics-data/pictures/ftp.png"]["folder"] == "PS5 storage", "internal pictures labelled")

    print("  previews")
    cover = os.path.join(usb0, "Cover.PNG")
    code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(cover), raw=True)
    check(code == 200 and png[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", png[16:24]) == (192, 192),
          "preview is a 192x192 PNG")
    check(len(os.listdir(os.path.join(data, "thumbs"))) == 1, "preview cached on internal storage")
    code, png2 = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(cover), raw=True)
    check(code == 200 and png2 == png, "cached preview served again")
    for bad in ["/etc/passwd", os.path.join(outside, "secret.png"), os.path.join(usb0, "notes.txt"),
                os.path.join(usb0, "..", "outside", "secret.png"), os.path.join(tmp, "usb0x", "a.png"),
                os.path.join(usb0, "missing.png"), ""]:
        code, body = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(bad), raw=True)
        check(code == 404, "preview refused: %s" % (os.path.relpath(bad, tmp) if bad else "(empty)"))

    print("  set a folder picture from USB")
    code, r = form("/api/folders/save", {"name": "Shots", "items": "PPSA01325"})
    fid = r["id"]
    icon0 = os.path.join(apps, fid, "sce_sys", "icon0.png")
    collage = open(icon0, "rb").read()
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(outside, "secret.png"))))
    check(code == 404 and "USB" in r["error"], "picture outside USB refused")
    check(open(icon0, "rb").read() == collage, "refused picture changed nothing")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(usb0, "notes.txt"))))
    check(code == 404, "non-picture refused")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(cover)))
    check(code == 200 and r["ok"], "USB picture accepted")
    check(png_size(icon0) == (512, 512) and open(icon0, "rb").read() != collage, "tile icon replaced (512x512)")
    check(http("GET", "/api/folder?id=" + fid)[1]["folder"]["custom_icon"], "custom picture remembered")
    os.rename(cover, cover + ".moved")
    code, r = http("GET", "/api/folder_icon?id=" + fid, raw=True)
    check(code == 200 and r[:4] == b"\x89PNG", "copy kept on internal storage after the USB picture is gone")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(cover)))
    check(code == 404 and "USB" in r["error"], "unplugged drive explained")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(data, "pictures", "ftp.png"))))
    check(code == 200, "picture from the internal pictures folder accepted")

    if Image:
        print("  transparent pictures go on the folder's colour")
        clear = os.path.join(usb0, "clear.png")
        t = Image.new("RGBA", (300, 300), (0, 0, 0, 0))
        ImageDraw.Draw(t).ellipse((75, 75, 225, 225), fill=(255, 255, 255, 255))
        t.save(clear)
        code, r = form("/api/folders/save", {"id": fid, "name": "Shots", "color": "teal", "items": "PPSA01325"})
        code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(clear)))
        check(code == 200, "transparent picture accepted")
        im = Image.open(icon0)
        check(im.mode == "RGB", "tile icon has no transparency left (the console gets a plain picture)")
        check(near(im.getpixel((3, 3)), (0x1f, 0xb5, 0xc4), 30) and near(im.getpixel((256, 256)), (255, 255, 255), 10),
              "transparent area shows the folder's teal, the picture stays white")
        form("/api/folders/save", {"id": fid, "name": "Shots", "color": "red", "items": "PPSA01325"})
        im = Image.open(icon0)
        check(near(im.getpixel((3, 3)), (0xe5, 0x48, 0x4d), 30), "changing the colour repaints behind the picture")
        stored = Image.open(os.path.join(data, "folders", fid + ".png"))
        check(stored.mode == "RGBA", "the picture is kept with its transparency for later colour changes")

        print("  phone/camera rotation (EXIF)")
        rot = os.path.join(shot_dir, "rotated.jpg")
        code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(rot), raw=True)
        top, bottom, left, right = px(png, 96, 20), px(png, 96, 172), px(png, 20, 96), px(png, 172, 96)
        check(near(top, (255, 0, 0)) and near(bottom, (0, 0, 255)), "rotated photo turned upright (red top, blue bottom)")
        code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(os.path.join(shot_dir, "upright.jpg")), raw=True)
        check(near(px(png, 20, 96), (255, 0, 0)) and near(px(png, 172, 96), (0, 0, 255)), "upright photo left as is")
        code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(rot)))
        big = open(icon0, "rb").read()
        check(code == 200 and near(px(big, 256, 40), (255, 0, 0)) and near(px(big, 256, 470), (0, 0, 255)),
              "tile icon from a rotated photo is upright")
        for orient, desc in [(3, "upside down"), (8, "rotated left")]:
            p = os.path.join(shot_dir, "o%d.jpg" % orient)
            exif_jpeg(p, orient)
            code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(p), raw=True)
            if orient == 3:   # 180: blue left, red right
                ok = near(px(png, 20, 96), (0, 0, 255)) and near(px(png, 172, 96), (255, 0, 0))
            else:             # 90 counter-clockwise: blue top, red bottom
                ok = near(px(png, 96, 20), (0, 0, 255)) and near(px(png, 96, 172), (255, 0, 0))
            check(code == 200 and ok, "%s photo corrected" % desc)
        # other formats
        for ext, fmt in [("bmp", "BMP"), ("gif", "GIF"), ("tga", "TGA")]:
            p = os.path.join(usb0, "fmt." + ext)
            Image.new("RGB", (40, 30), (0, 128, 255)).save(p, fmt)
            code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(p), raw=True)
            check(code == 200 and near(px(png, 96, 96), (0, 128, 255)), "%s picture works" % fmt)
    else:
        print("  (Pillow not installed: EXIF and format checks skipped)")

    print("folder page")
    code, page = http("GET", "/folder/" + fid, raw=True)
    check(b'id="favicon"' in page and b"close=1" not in page and b"closes by itself" not in page,
          "folder page shows the folder icon and no longer promises to close itself")
    code, st = http("GET", "/api/status")
    check("autoclose" not in st["settings"], "auto-close option removed")
    s.stop()


def startup_mode_tests(tmp):
    print("menu at start: quiet for Autoload, open when started again")
    appdb = os.path.join(tmp, "boot.db")
    data = os.path.join(tmp, "boot-data")
    make_db(appdb)

    def opened(srv):
        return any("would open http://127.0.0.1:8090/" in l for l in srv.out)

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-1"})
    check(opened(s), "default: the menu opens")
    code, r = http("POST", "/api/settings?open_browser=2")
    check(code == 200 and http("GET", "/api/status")[1]["settings"]["open_browser"] == 2, "quiet-at-Autoload mode saved")
    s.stop()

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-2"})
    check(not opened(s), "first start after the console turns on (Autoload): no window")
    check("first start since boot 1 -> quiet" in http("GET", "/api/diag", raw=True)[1].decode(), "decision logged")

    # started again from Payload Manager while the Autoload copy runs
    s2 = Server(appdb, data, {"HH_BOOT_ID": "boot-2"}, wait=False)
    s.p.wait(timeout=10)
    check(wait_for(lambda: opened(s2)), "started again while running: the menu opens")
    wait_for(lambda: http_ok("/api/status"))
    s2.stop()

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-2"})
    check(opened(s), "started again later in the same session (after Quit): the menu opens")
    s.stop()

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-3"})
    check(not opened(s), "next time the console is turned on: quiet again")
    http("POST", "/api/settings?open_browser=0")
    s.stop()
    s = Server(appdb, data, {"HH_BOOT_ID": "boot-3"})
    check(not opened(s), "never-open mode stays quiet")
    http("POST", "/api/settings?open_browser=7")
    check(http("GET", "/api/status")[1]["settings"]["open_browser"] == 1, "unknown value treated as 'open'")
    menu = http("GET", "/", raw=True)[1]
    check(b"Quiet when started by Autoload" in menu and b"sw_autoclose" not in menu, "menu shows the three choices")
    s.stop()


def http_ok(path):
    try:
        return http("GET", path)[0] == 200
    except Exception:
        return False


def launch(tid, extra="", expect_job=True):
    """Starts tid like the menu does and waits for the result."""
    code, r = http("POST", "/api/launch?id=" + tid + extra)
    assert code == 200 and r["ok"], (code, r)
    if not expect_job:
        return r
    assert "job" in r, r
    for _ in range(100):
        code, j = http("GET", "/api/launch_status?job=%d" % r["job"])
        assert code == 200 and j["ok"], j
        if j["done"]:
            return j
        time.sleep(0.05)
    raise AssertionError("launch never finished")


def launch_tests(tmp):
    print("starting games and apps")
    appdb = os.path.join(tmp, "lnc.db")
    data = os.path.join(tmp, "lnc-data")
    apps = os.path.join(tmp, "lnc-apps")
    meta = os.path.join(tmp, "lnc-appmeta")
    make_db(appdb)
    # web link tiles: Payload Manager style (param.json in /user/app) ...
    pm = os.path.join(apps, "PLDM00001", "sce_sys")
    os.makedirs(pm)
    json.dump({"titleId": "PLDM00001", "applicationCategoryType": 65536,
               "deeplinkUri": "http://127.0.0.1:8084/"}, open(os.path.join(pm, "param.json"), "w"))
    # ... one whose details only the console's appmeta copy has
    os.makedirs(os.path.join(meta, "WFEX00001"))
    json.dump({"deeplinkUri": "http://127.0.0.1:5000/files"}, open(os.path.join(meta, "WFEX00001", "param.json"), "w"))
    # ... one known only from app.db
    qx(appdb, "UPDATE tbl_contentinfo SET AppInfoJson=? WHERE titleId='CUSA01065'",
      json.dumps({"deeplinkUri": "https://example.com/tv"}))
    # not web links: a real app with a program, and a script address
    real = os.path.join(apps, "PPSA05000")
    os.makedirs(os.path.join(real, "sce_sys"))
    open(os.path.join(real, "eboot.bin"), "wb").write(b"\x7fELF")
    json.dump({"deeplinkUri": "http://127.0.0.1:1/"}, open(os.path.join(real, "sce_sys", "param.json"), "w"))
    bad = os.path.join(apps, "BADL00001", "sce_sys")
    os.makedirs(bad)
    json.dump({"deeplinkUri": "javascript:alert(1)"}, open(os.path.join(bad, "param.json"), "w"))

    env = {"HH_USER_APP_DIR": apps, "HH_APPMETA_DIR": meta,
           "HH_FAKE_LAUNCH": "PPSA02000=0x80940031,NPXS40053=0x80940013"}
    s = Server(appdb, data, env)

    print("  web link apps open their page")
    r = launch("PLDM00001", "&local=1", expect_job=False)
    check(r.get("url") == "http://127.0.0.1:8084/", "Payload Manager tile -> its address, for the page to open")
    check(not any("PLDM00001" in l for l in s.out if "[launch]" in l), "launcher not asked (it can only say 'can't open')")
    r = launch("PLDM00001", "", expect_job=False)
    check(wait_for(lambda: any("would open http://127.0.0.1:8084/" in l for l in s.out)),
          "from a phone/PC: opened in the PS5 browser instead")
    r = launch("WFEX00001", "&local=1", expect_job=False)
    check(r.get("url") == "http://127.0.0.1:5000/files", "address found in the console's appmeta copy")
    r = launch("CUSA01065", "&local=1", expect_job=False)
    check(r.get("url") == "https://example.com/tv", "address found in app.db")
    j = launch("PPSA05000", "&local=1")
    check(j["result"] == "ok", "app with its own program started normally")
    j = launch("BADL00001", "&local=1")
    check("url" not in j, "script addresses are never opened")
    check("web link http://127.0.0.1:8084/" in http("GET", "/api/diag", raw=True)[1].decode(), "web links logged")

    print("  starting, switching, another game running")
    s.stop()
    s = Server(appdb, data, env)
    j = launch("PPSA01325")
    check(j["result"] == "ok" and j["note"].startswith("Starting ASTRO's PLAYROOM"), "game started, named")
    check(any("would start PPSA01325" in l for l in s.out), "launcher asked")
    j = launch("PPSA01325")
    check(j["result"] == "ok" and j["note"].startswith("Switching to"), "already running counts as started")
    j = launch("CUSA00900")
    check(j["result"] == "other_running" and j["other"] == {"id": "PPSA01325", "name": "ASTRO's PLAYROOM"},
          "another game running: reported with its name so the page can ask")
    check(not any("would close app" in l for l in s.out), "nothing closed without asking")
    j = launch("CUSA00900", "&close_running=1")
    check(j["result"] == "ok", "after the OK, the running game is closed and the new one starts")
    check(any("would close app 60000" in l for l in s.out) and any("would start CUSA00900" in l for l in s.out),
          "closed first, then started")
    log = http("GET", "/api/diag", raw=True)[1].decode()
    check("closing PPSA01325 first" in log, "closing the other game is logged")
    j = launch(GALLERY)
    check(j["result"] == "ok", "apps can start while a game runs")

    print("  errors are explained")
    j = launch("PPSA02000")
    check(j["result"] == "error" and "can't find this app" in j["error"] and j["code"] == "0x80940031",
          "launcher error turned into words, code kept")
    j = launch("NPXS40053")
    check(j["result"] == "error" and "disc" in j["error"], "another error explained")
    code, r = http("GET", "/api/launch_status?job=999")
    check(r["ok"] is False, "unknown job reported")
    code, r = http("GET", "/api/launch_status")
    check(r["ok"] is False, "missing job reported")
    s.stop()

    print("  folder page and menu use the new flow")
    s = Server(appdb, data, env)
    page = http("GET", "/folder/HHFD00001", raw=True)[1]
    check(b"launch_status" in page and b"close_running" in page and b"location.href = j.url" in page and
          b"close=1" not in page,
          "folder page polls, asks about the running game and opens web links")
    menu = http("GET", "/", raw=True)[1]
    check(b"launch_status" in menu and b"close_running" in menu, "menu Open does the same")
    s.stop()


def custom_tests(tmp):
    print("rename tiles and change their pictures")
    appdb = os.path.join(tmp, "cus.db")
    data = os.path.join(tmp, "cus-data")
    meta = os.path.join(tmp, "cus-appmeta")
    apps = os.path.join(tmp, "cus-apps")
    usb0 = os.path.join(tmp, "cus-usb0")
    os.makedirs(usb0)
    open(os.path.join(usb0, "art.png"), "wb").write(tiny_png(300, 300, (250, 120, 0)))
    make_db(appdb)
    original = dump(appdb)
    env = {"HH_APPMETA_DIR": meta, "HH_USER_APP_DIR": apps, "HH_USB_FMT": os.path.join(tmp, "cus-usb%d")}
    s = Server(appdb, data, env)

    def names(tid):
        rows = []
        for u in USERS:
            rows += [r[0] for r in q(appdb, f'SELECT titleName FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)]
            rows += [r[0] for r in q(appdb, f'SELECT conceptName FROM "tbl_concepticoninfo_{u}" WHERE localConceptId=?',
                                     "cid:local:" + tid)]
        rows += [r[0] for r in q(appdb, "SELECT titleName FROM tbl_contentinfo WHERE titleId=?", tid)]
        return rows

    def icons(tid):
        return [q(appdb, f'SELECT icon0Info FROM "tbl_concepticoninfo_{u}" WHERE localConceptId=?',
                  "cid:local:" + tid)[0][0] for u in USERS]

    code, j = http("GET", "/api/custom?id=PPSA01325")
    it = j["item"]
    check(it["name"] == "ASTRO's PLAYROOM" and it["can_rename"] and it["can_icon"] and not it["renamed"],
          "editor info: current name, can rename and change the picture")

    print("  names")
    new = "Astro \u2605 Bot"
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "  " + new + "  "})
    check(code == 200, "renamed")
    check(names("PPSA01325") == [new] * 5, "titleName (both users + contentinfo) and conceptName all changed")
    others = dump(appdb)
    for t in original:
        if t.startswith(("tbl_group", "tbl_version")):
            check(others[t] == original[t], f"{t} untouched")
    by_id, _ = listing()
    check(by_id["PPSA01325"]["name"] == new and by_id["PPSA01325"]["renamed"], "menu list shows the new name")
    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(j["item"]["orig_name"] == "ASTRO's PLAYROOM" and j["item"]["renamed"], "original name remembered")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "Second Name"})
    check(code == 200 and names("PPSA01325") == ["Second Name"] * 5, "renamed again")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "x" * 65})
    check(code == 409 and "64" in r["error"], "too long refused")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "bad\nname"})
    check(code == 409, "control characters refused")
    code, r = form("/api/custom/name?id=CUSA00900", {"name": "My PS4 Game"})
    check(code == 200 and names("CUSA00900")[0] == "My PS4 Game", "game without a concept name renamed")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "ASTRO's PLAYROOM"})
    check(code == 200 and names("PPSA01325") == ["Astro Playroom", "ASTRO's PLAYROOM"] * 2 + ["Astro Playroom"],
          "typing the original name puts every original value back")
    check(not http("GET", "/api/custom?id=PPSA01325")[1]["item"]["renamed"], "no longer marked renamed")
    code, r = http("POST", "/api/custom/name?id=CUSA00900&reset=1")
    check(code == 200 and dump(appdb) == original, "reset: database back to the original")

    print("  pictures")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325", tiny_png(640, 360, (0, 90, 250)))
    check(code == 200, "picture accepted")
    paths = icons("PPSA01325")
    check(len(set(paths)) == 1 and paths[0].startswith(os.path.join(meta, "PPSA01325", "hh_icon0_")),
          "icon0Info points at Home Hider's own file in appmeta (both users)")
    check(png_size(paths[0]) == (512, 512), "512x512 picture written")
    code, png = http("GET", "/api/icon?id=PPSA01325", raw=True)
    check(code == 200 and png == open(paths[0], "rb").read(), "menu shows the new picture")
    code, r = http("POST", "/api/custom/icon?id=PPSA01325&usb=" + urllib.parse.quote(os.path.join(usb0, "art.png")))
    check(code == 200, "picture from USB accepted")
    paths2 = icons("PPSA01325")
    check(paths2[0] != paths[0] and not os.path.exists(paths[0]) and os.path.exists(paths2[0]),
          "new file name each time (no stale cache); the previous file removed")
    by_id, _ = listing()
    check(by_id["PPSA01325"]["custom_icon"], "list marks the new picture")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325", b"not a picture at all" * 4)
    check(code == 409 and "picture" in r["error"] and icons("PPSA01325") == paths2, "bad picture refused, nothing changed")
    code, r = post_bytes("/api/custom/icon?id=" + STORE, tiny_png(64, 64, (1, 2, 3)))
    check(code == 409 and "isn't stored" in r["error"], "tile without a picture entry explained")
    check(not os.path.exists(os.path.join(meta, STORE)) or not os.listdir(os.path.join(meta, STORE)),
          "no stray file left for it")
    code, r = http("POST", "/api/custom/name?id=HHFD00001&reset=1")
    check(code == 409 and "folder" in r["error"], "folder tiles use the folder editor")

    if Image:
        import io
        t = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        ImageDraw.Draw(t).rectangle((64, 64, 192, 192), fill=(250, 250, 250, 255))
        buf = io.BytesIO()
        t.save(buf, "PNG")
        code, r = post_bytes("/api/custom/icon?id=PPSA01325", buf.getvalue())
        im = Image.open(icons("PPSA01325")[0])
        check(code == 200 and im.mode == "RGB" and near(im.getpixel((3, 3)), (0x2a, 0x30, 0x40), 12),
              "transparent tile picture placed on a dark background")
        paths2 = icons("PPSA01325")

    print("  the console puts originals back -> fixed at next start")
    form("/api/custom/name?id=PPSA01325", {"name": "Kept Name"})
    s.stop()
    u = USERS[0]
    qx(appdb, f'UPDATE "tbl_concepticoninfo_{u}" SET conceptName=?, icon0Info=? WHERE localConceptId=?',
      "ASTRO's PLAYROOM", "/user/appmeta/PPSA01325/icon0.png?ts=2", "cid:local:PPSA01325")
    os.unlink(paths2[0])
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    check(st["custom_fixed"] >= 3, "name, picture path and missing file put back (%d)" % st["custom_fixed"])
    check(names("PPSA01325") == ["Kept Name"] * 5 and icons("PPSA01325") == paths2 and os.path.exists(paths2[0]),
          "custom name and picture back in place")

    print("  back to the originals")
    code, r = http("POST", "/api/custom/icon?id=PPSA01325&reset=1")
    check(code == 200 and icons("PPSA01325") == ["/user/appmeta/PPSA01325/icon0.png?ts=2",
                                                "/user/appmeta/PPSA01325/icon0.png?ts=1"],
          "original picture paths back (the newest one the console set is kept)")
    check(not os.path.exists(paths2[0]), "Home Hider's picture file removed")
    code, r = http("POST", "/api/custom/name?id=PPSA01325&reset=1")
    check(code == 200 and names("PPSA01325") == ["Astro Playroom", "ASTRO's PLAYROOM"] * 2 + ["Astro Playroom"],
          "original name back")
    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(not j["item"]["renamed"] and not j["item"]["custom_icon"], "nothing custom left")

    print("  placeholder concept ids shared by many tiles are left alone")
    for u in USERS:
        for tid in (STORE, GALLERY, PLUS, "NPXS40053"):
            qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET localConceptId=? WHERE titleId=?', "cid:shared", tid)
        qx(appdb, f'INSERT INTO "tbl_concepticoninfo_{u}" VALUES(?,?,?)', "cid:shared", "PlayStation Store", "/x.png")
    code, r = form("/api/custom/name?id=" + STORE, {"name": "Shop"})
    check(code == 200, "system tile renamed")
    check([r_[0] for r_ in q(appdb, f'SELECT titleName FROM "tbl_iconinfo_{USERS[0]}" WHERE titleId=?', STORE)] == ["Shop"],
          "its own row changed")
    check(q(appdb, f'SELECT conceptName FROM "tbl_concepticoninfo_{USERS[0]}" WHERE localConceptId=?', "cid:shared")
          == [("PlayStation Store",)], "shared concept row untouched (other tiles keep their names)")
    http("POST", "/api/custom/name?id=%s&reset=1" % STORE)

    print("  restore original backup")
    form("/api/custom/name?id=PPSA01325", {"name": "Temp"})
    post_bytes("/api/custom/icon?id=PPSA01325", tiny_png(64, 64, (9, 9, 9)))
    f = icons("PPSA01325")[0]
    code, r = http("POST", "/api/restore")
    check(code == 200 and dump(appdb) == original, "original database restored")
    check(not os.path.exists(f), "picture files removed")
    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(not j["item"]["renamed"] and not j["item"]["custom_icon"], "custom list cleared")
    menu = http("GET", "/", raw=True)[1]
    check(b"editTile" in menu and b"first frame" in menu, "menu has the editor and explains GIFs")
    s.stop()


def dds_header(w, h, mips, fourcc=b"DX10", dxgi=98):
    """Just the header of a DDS file (enough for Home Hider to read the format)."""
    hd = bytearray(148)
    hd[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", hd, 4, 124, 0x1007 | 0x20000, h, w, 0, 0, mips)
    struct.pack_into("<II", hd, 76, 32, 4)
    hd[84:88] = fourcc
    if fourcc == b"DX10":
        struct.pack_into("<IIIII", hd, 128, dxgi, 3, 0, 1, 0)
    return bytes(hd)


def dds_info(path):
    d = open(path, "rb").read(148)
    h, w = struct.unpack_from("<II", d, 12)
    mips = struct.unpack_from("<I", d, 28)[0]
    four = d[84:88]
    dxgi = struct.unpack_from("<I", d, 128)[0] if four == b"DX10" else four.decode()
    return w, h, mips, dxgi


def background_tests(tmp):
    print("home screen backgrounds")
    if not Image:
        print("  (Pillow not installed: skipped)")
        return
    import io
    appdb = os.path.join(tmp, "bg.db")
    data = os.path.join(tmp, "bg-data")
    meta = os.path.join(tmp, "bg-appmeta")
    apps = os.path.join(tmp, "bg-apps")
    orig_dir = os.path.join(tmp, "bg-orig")
    usb0 = os.path.join(tmp, "bg-usb0")
    for d in (orig_dir, usb0):
        os.makedirs(d)
    make_db(appdb)
    for u in USERS:
        qx(appdb, f'ALTER TABLE "tbl_concepticoninfo_{u}" ADD COLUMN pic0Info TEXT')
        qx(appdb, f'ALTER TABLE "tbl_concepticoninfo_{u}" ADD COLUMN pic1Info TEXT')
    # originals: PPSA01325 has a BC7 DDS (3 mips) and a PNG; CUSA00900's is inside a
    # package Home Hider can't read; CUSA01065 uses a format it can't write
    open(os.path.join(orig_dir, "pic0.dds"), "wb").write(dds_header(1920, 1080, 3))
    Image.new("RGB", (1280, 720), (1, 2, 3)).save(os.path.join(orig_dir, "pic1.png"))
    open(os.path.join(orig_dir, "dxt3.dds"), "wb").write(dds_header(1920, 1080, 1, b"DXT3"))
    origs = {"PPSA01325": (os.path.join(orig_dir, "pic0.dds"), os.path.join(orig_dir, "pic1.png") + "?ts=4"),
             "CUSA00900": ("/mnt/sandbox/pfsmnt/CUSA00900-app0/sce_sys/pic0.dds", None),
             "CUSA01065": (os.path.join(orig_dir, "dxt3.dds"), None)}
    for u in USERS:
        for tid, (p0, p1) in origs.items():
            qx(appdb, f'UPDATE "tbl_concepticoninfo_{u}" SET pic0Info=?, pic1Info=? WHERE localConceptId=?',
               p0, p1, "cid:local:" + tid)
    original = dump(appdb)

    pic = Image.new("RGB", (1600, 900), (20, 60, 200))
    ImageDraw.Draw(pic).ellipse((600, 250, 1000, 650), fill=(250, 200, 30))
    buf = io.BytesIO()
    pic.save(buf, "JPEG", quality=95)
    jpg = buf.getvalue()
    open(os.path.join(usb0, "wide.jpg"), "wb").write(jpg)

    env = {"HH_APPMETA_DIR": meta, "HH_USER_APP_DIR": apps, "HH_USB_FMT": os.path.join(tmp, "bg-usb%d")}
    s = Server(appdb, data, env)

    def bgs(tid):
        return [q(appdb, f'SELECT pic0Info, pic1Info FROM "tbl_concepticoninfo_{u}" WHERE localConceptId=?',
                  "cid:local:" + tid)[0] for u in USERS]

    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(j["item"]["can_bg"] and not j["item"]["custom_bg"], "editor: this game's background can be changed")
    code, j = http("GET", "/api/custom?id=" + STORE)
    check(not j["item"]["can_bg"], "tiles without a background entry say so")

    print("  same format as the original")
    code, r = post_bytes("/api/custom/bg?id=PPSA01325", jpg)
    check(code == 200, "background saved")
    rows = bgs("PPSA01325")
    p0, p1 = rows[0]
    check(rows[0] == rows[1], "both users point at the same new files")
    check(p0.startswith(os.path.join(meta, "PPSA01325", "hh_bg_")) and p0.endswith(".dds"), "pic0 -> Home Hider's DDS in appmeta")
    check(p1.endswith(".png") and p1 != p0, "pic1 -> a PNG, because the original pic1 is a PNG")
    check(dds_info(p0) == (1920, 1080, 3, 98), "DDS keeps the original size, mip count and BC7 format")
    im = Image.open(p0)
    im.load()
    c = im.convert("RGB").getpixel((960, 540))
    check(near(c, (250, 200, 30), 40) and near(im.convert("RGB").getpixel((60, 60)), (20, 60, 200), 40),
          "the BC7 texture decodes to the picture")
    check(Image.open(p1).size == (1280, 720), "PNG keeps the original size")
    by_id, _ = listing()
    check(by_id["PPSA01325"]["custom_bg"], "list marks the new background")
    code, prev = http("GET", "/api/custom_bg?id=PPSA01325", raw=True)
    check(code == 200 and prev[:2] == b"\xff\xd8", "preview of the chosen background served")
    code, r = http("POST", "/api/custom/bg?id=PPSA01325&usb=" + urllib.parse.quote(os.path.join(usb0, "wide.jpg")))
    check(code == 200 and bgs("PPSA01325")[0][0] != p0 and not os.path.exists(p0) and not os.path.exists(p1),
          "picked from USB: new file names (no cached copy), old files removed")

    print("  unreadable or unusual originals")
    code, r = post_bytes("/api/custom/bg?id=CUSA00900", jpg)
    p = bgs("CUSA00900")[0][0]
    check(code == 200 and dds_info(p) == (3840, 2160, 1, 98), "original inside a game package: 4K BC7 DDS")
    code, r = post_bytes("/api/custom/bg?id=CUSA01065", jpg)
    check(code == 409 and "can't write" in r["error"], "a format Home Hider can't write is refused")
    check(bgs("CUSA01065")[0][0] == origs["CUSA01065"][0], "and nothing changed")
    code, r = post_bytes("/api/custom/bg?id=" + STORE, jpg)
    check(code == 409 and "no background" in r["error"], "tile without a background entry refused")
    code, r = post_bytes("/api/custom/bg?id=PPSA01325", b"nope" * 20)
    check(code == 409 and "picture" in r["error"], "bad picture refused")

    print("  the console resets it -> put back at next start")
    cur = bgs("PPSA01325")[0]
    s.stop()
    qx(appdb, f'UPDATE "tbl_concepticoninfo_{USERS[0]}" SET pic0Info=? WHERE localConceptId=?',
       origs["PPSA01325"][0], "cid:local:PPSA01325")
    os.unlink(cur[1])
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    now = bgs("PPSA01325")
    check(st["custom_fixed"] >= 2 and now[0] == now[1] and all(os.path.exists(x) for x in now[0]),
          "background files remade and pointed at again")

    print("  back to the original")
    code, r = http("POST", "/api/custom/bg?id=PPSA01325&reset=1")
    check(code == 200 and bgs("PPSA01325") == [origs["PPSA01325"]] * 2, "original paths back")
    check(not any(f.startswith("hh_bg_") for f in os.listdir(os.path.join(meta, "PPSA01325"))), "our files removed")
    code, r = http("POST", "/api/custom/bg?id=CUSA00900&reset=1")
    check(dump(appdb) == original, "database exactly as before")

    print("  folders")
    code, r = form("/api/folders/save", {"name": "Wide", "color": "none", "items": "PPSA01325"})
    fid = r["id"]
    tile = os.path.join(apps, fid, "sce_sys")
    im = Image.open(os.path.join(tile, "icon0.png"))
    check(im.mode == "RGBA" and im.getpixel((3, 3))[3] == 0, "colour 'none': the folder icon is see-through")
    code, r = http("POST", "/api/folders/bg?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(usb0, "wide.jpg"))))
    check(code == 200 and dds_info(os.path.join(tile, "pic0.dds")) == (3840, 2160, 1, 98),
          "folder tile gets a 4K BC7 pic0.dds")
    code, j = http("GET", "/api/folder?id=" + fid)
    check(j["folder"]["custom_bg"], "folder reports its background")
    code, prev = http("GET", "/api/folder_bg?id=" + fid, raw=True)
    check(code == 200 and prev[:2] == b"\xff\xd8", "folder background preview served")
    form("/api/folders/save", {"id": fid, "name": "Wide 2", "color": "teal", "items": "PPSA01325"})
    check(os.path.exists(os.path.join(tile, "pic0.dds")), "background kept when the folder is edited")
    clear = Image.new("RGBA", (200, 200), (0, 0, 0, 0))
    ImageDraw.Draw(clear).ellipse((50, 50, 150, 150), fill=(255, 255, 255, 255))
    buf = io.BytesIO()
    clear.save(buf, "PNG")
    form("/api/folders/save", {"id": fid, "name": "Wide 2", "color": "none", "items": "PPSA01325"})
    post_bytes("/api/folders/icon?id=" + fid, buf.getvalue())
    im = Image.open(os.path.join(tile, "icon0.png"))
    check(im.mode == "RGBA" and im.getpixel((3, 3))[3] == 0 and im.getpixel((256, 256))[3] == 255,
          "custom picture on colour 'none' stays see-through")
    code, r = http("POST", "/api/folders/bg?id=%s&reset=1" % fid)
    check(code == 200 and not os.path.exists(os.path.join(tile, "pic0.dds")), "folder background removed")

    print("  see-through tile pictures")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325&clear=1", buf.getvalue())
    f = q(appdb, f'SELECT icon0Info FROM "tbl_concepticoninfo_{USERS[0]}" WHERE localConceptId=?', "cid:local:PPSA01325")[0][0]
    im = Image.open(f)
    check(code == 200 and im.mode == "RGBA" and im.getpixel((3, 3))[3] == 0, "'keep see-through' leaves it transparent")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325", buf.getvalue())
    f = q(appdb, f'SELECT icon0Info FROM "tbl_concepticoninfo_{USERS[0]}" WHERE localConceptId=?', "cid:local:PPSA01325")[0][0]
    check(Image.open(f).mode == "RGB", "without it: dark background as before")

    print("  restore original backup")
    post_bytes("/api/custom/bg?id=PPSA01325", jpg)
    ours = bgs("PPSA01325")[0]
    code, r = http("POST", "/api/restore")
    check(code == 200 and not any(os.path.exists(x) for x in ours), "background files removed with the restore")
    menu = http("GET", "/", raw=True)[1]
    check(b"teBgField" in menu and b"fmBgFile" in menu and b"teClear" in menu, "menu has the background and see-through options")
    s.stop()


class FakeLiveShell:
    """Plays LiveShell's web API (port 9050 on the console)."""

    def __init__(self, port):
        self.port = port
        self.calls = []
        self.videos = [
            {"name": "ocean.mp4", "where": "Internal", "path": "/data/LiveShell/video/ocean.mp4", "res": "FHD",
             "snd": False, "why": "", "size": 1000, "thumb": True, "fav": False, "k4": False},
            {"name": "city night.mp4", "where": "USB 1", "path": "/mnt/usb0/liveshell/city night.mp4", "res": "4K",
             "snd": True, "why": "", "size": 2000, "thumb": False, "fav": False, "k4": True},
        ]
        self.current = self.videos[0]["path"]
        outer = self

        class H(httpserver.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, code, body, ctype="application/json"):
                b = body if isinstance(body, bytes) else body.encode()
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def do_GET(self):
                path, _, query = self.path.partition("?")
                qs = urllib.parse.parse_qs(query)
                i = int(qs.get("i", ["-1"])[0])
                outer.calls.append(self.path)
                if path == "/api/state":
                    self.reply(200, json.dumps({"enabled": True, "status": "playing", "ip": "192.168.1.50",
                                                "port": 9050, "current": outer.current, "videos": outer.videos}))
                elif path == "/api/select" and 0 <= i < len(outer.videos):
                    outer.current = outer.videos[i]["path"]
                    self.reply(200, '{"ok":true}')
                elif path == "/api/delete" and 0 <= i < len(outer.videos):
                    if outer.videos[i]["where"] != "Internal":
                        self.reply(200, '{"ok":false,"error":"Videos on USB are deleted on the drive"}')
                    else:
                        outer.videos.pop(i)
                        self.reply(200, '{"ok":true}')
                elif path == "/thumb" and i == 0:
                    self.reply(200, tiny_png(16, 9, (10, 120, 200)), "image/png")
                elif path == "/api/shutdown":
                    self.reply(200, '{"ok":true}')
                    threading.Thread(target=outer.stop, daemon=True).start()
                else:
                    self.reply(404, "")

        socketserver.TCPServer.allow_reuse_address = True
        self.srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


class RecordingLoader:
    """An ELF loader that only records what it is sent."""

    def __init__(self, port):
        self.received = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(4)
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            data = b""
            while True:
                chunk = c.recv(65536)
                if not chunk:
                    break
                data += chunk
            c.close()
            self.received.append(data)

    def close(self):
        self.sock.close()


def live_tests(tmp):
    print("live wallpaper (built-in LiveShell Engine)")
    appdb = os.path.join(tmp, "live.db")
    data = os.path.join(tmp, "live-data")
    make_db(appdb)
    elf = open(os.path.join(os.path.dirname(BIN), "..", "..", "third_party", "liveshell", "liveshell.elf"), "rb").read()
    lport, aport = 19121, 19050
    loader = RecordingLoader(lport)
    env = {"HH_LOADER_PORT": str(lport), "HH_LIVESHELL_PORT": str(aport)}

    s = Server(appdb, data, env)
    code, j = http("GET", "/api/live")
    check(code == 200 and not j["enabled"] and not j["running"] and j["version"] == "1.5.2", "off by default")
    time.sleep(0.5)
    check(loader.received == [], "LiveShell not started while it's off")
    d = http("GET", "/api/diag", raw=True)[1].decode()
    check("LiveShell 1.5.2 built in: %d bytes, sha256 %s" % (len(elf), hashlib.sha256(elf).hexdigest()) in d,
          "Diagnostics names the built-in LiveShell and its checksum")

    print("  turning it on starts the built-in LiveShell")
    code, r = http("POST", "/api/live/power?on=1")
    check(code == 200 and "Starting" in r["note"], "switched on")
    check(wait_for(lambda: len(loader.received) == 1), "liveshell.elf handed to the ELF loader")
    check(loader.received[0] == elf, "byte for byte the original liveshell.elf")
    code, j = http("GET", "/api/live")
    check(j["enabled"] and not j["running"] and j["state"] is None, "reported as starting until LiveShell answers")

    print("  menu works through LiveShell's API")
    fake = FakeLiveShell(aport)
    code, j = http("GET", "/api/live")
    check(j["running"] and j["state"]["current"] == "/data/LiveShell/video/ocean.mp4" and len(j["state"]["videos"]) == 2,
          "status and videos come from LiveShell")
    code, r = http("POST", "/api/live/select?path=" + urllib.parse.quote("/mnt/usb0/liveshell/city night.mp4"))
    check(code == 200 and "/api/select?i=1" in fake.calls and fake.current.endswith("city night.mp4"),
          "picking a video tells LiveShell (by its place in LiveShell's list)")
    code, r = http("POST", "/api/live/select?path=" + urllib.parse.quote("/etc/passwd"))
    check(code == 404 and not any("i=-1" in c for c in fake.calls), "unknown video refused, nothing sent")
    code, png = http("GET", "/api/live/thumb?path=" + urllib.parse.quote("/data/LiveShell/video/ocean.mp4"), raw=True)
    check(code == 200 and png[:4] == b"\x89PNG", "LiveShell's preview picture passed on")
    code, png = http("GET", "/api/live/thumb?path=" + urllib.parse.quote("/mnt/usb0/liveshell/city night.mp4"), raw=True)
    check(code == 404, "no preview -> 404")
    code, r = http("POST", "/api/live/delete?path=" + urllib.parse.quote("/mnt/usb0/liveshell/city night.mp4"))
    check(code == 409 and "USB" in r["error"], "LiveShell's own error passed on")
    code, r = http("POST", "/api/live/delete?path=" + urllib.parse.quote("/data/LiveShell/video/ocean.mp4"))
    check(code == 200 and len(fake.videos) == 1, "video deleted")
    evil = urllib.request.Request(BASE + "/api/live/power?on=0", method="POST", data=b"",
                                  headers={"Origin": "http://evil.example"})
    try:
        urllib.request.urlopen(evil, timeout=5)
        code = 200
    except urllib.error.HTTPError as e:
        code = e.code
    check(code == 403, "other web sites can't switch it")

    print("  starts with Home Hider, but never twice")
    s.stop()
    s = Server(appdb, data, env)
    time.sleep(4.5)
    check(len(loader.received) == 1, "already running: not loaded again")
    s.stop()
    fake.stop()
    s = Server(appdb, data, env)
    check(wait_for(lambda: len(loader.received) == 2, 10), "not running at start: started again (setting kept)")

    print("  turning it off")
    fake = FakeLiveShell(aport)
    code, r = http("POST", "/api/live/power?on=0")
    check(code == 200 and "/api/shutdown" in fake.calls, "LiveShell asked to stop")
    check(not http("GET", "/api/live")[1]["enabled"], "setting off")
    code, r = http("POST", "/api/live/select?path=x")
    check(code in (404, 409), "nothing to pick while it's off")
    menu = http("GET", "/", raw=True)[1]
    check(b"LiveShell Engine" in menu and b"ZiZc3" in menu and b"github.com/ZiZc3/LiveShell" in menu,
          "menu credits LiveShell and its author")
    s.stop()
    loader.close()


def glob_elves(root):
    out = []
    for d, _, files in os.walk(root):
        out += [os.path.join(d, f) for f in files if f.endswith(".elf")]
    return sorted(out)


def main():
    tmp = tempfile.mkdtemp(prefix="hh-test-")
    try:
        appdb = os.path.join(tmp, "app.db")
        data = os.path.join(tmp, "data")
        make_db(appdb)
        original = dump(appdb)

        print("startup")
        s = Server(appdb, data)
        check(os.path.exists(os.path.join(data, "backup", "app.db.original")), "original backup created")
        check(dump(os.path.join(data, "backup", "app.db.original")) == original, "backup matches app.db")
        check("would open http://127.0.0.1:8090/" in "".join(s.out), "browser opened on launch")
        code, st = http("GET", "/api/status")
        check(code == 200 and st["db"]["ok"] and st["db"]["users"] == 2, "status sees 2 users")
        check(STORE in listing()[0] and not tile(st, STORE)["hidden"], "store visible at start")
        check(tile(st, STORE)["pinned"], "store pinned in MyHome")
        code, html = http("GET", "/", raw=True)
        check(code == 200 and b"Home Hider" in html, "web UI served")

        print("full list of games, apps and system tiles")
        by_id, ordered = listing()
        check(set(by_id) == EXPECTED, "list = installed titles + real home tiles (got %s)" % sorted(by_id))
        check("PPSA99999" not in by_id, "uninstalled leftover excluded")
        check("NPXS40140" not in by_id and "NPXS40085" not in by_id, "internal/off-screen system entries excluded")
        check(by_id["PPSA01325"]["name"] == "ASTRO's PLAYROOM", "name taken from concept table")
        check(by_id["CUSA00900"]["name"] == "Some PS4 Game", "falls back to titleName")
        check(by_id["CUSA01065"]["tab"] == "media" and by_id["PPSA01325"]["tab"] == "games", "tabs from dispLocation")
        check(by_id["PPSA02000"]["tab"] == "none", "installed game with no tab still listed")
        check(by_id["PPSA01325"]["kind"] == "game" and by_id[STORE]["kind"] == "system", "kind reported")
        check(LIBRARY not in by_id and "NPXS40139" not in by_id, "Game Library / App Library not offered")
        check("NPXS40056" in by_id and by_id["NPXS40056"]["tab"] == "media", "All Apps offered on the Media tab")
        all_apps_before = dump(appdb)
        code, r = http("POST", "/api/hide?id=NPXS40056")
        check(code == 200 and listing()[0]["NPXS40056"]["hidden"], "All Apps can be hidden again")
        http("POST", "/api/unhide?id=NPXS40056")
        check(dump(appdb) == all_apps_before, "All Apps shown again exactly")
        before_lock = dump(appdb)
        code, r = http("POST", "/api/hide?id=" + LIBRARY)
        check(code == 409 and "always adds" in r["error"], "hiding Game Library refused")
        code, r = http("POST", "/api/hide?id=NPXS40139")
        check(code == 409, "hiding App Library refused")
        check(dump(appdb) == before_lock, "refused hides changed nothing")
        recents = [t["recent"] for t in ordered]
        check(recents == sorted(recents, reverse=True), "sorted most recent first")
        for tid in ("CUSA00900", "CUSA01065"):
            code, body = http("GET", "/api/icon?id=" + tid, raw=True)
            check(code == 404 and b"root:" not in body, f"{tid}: unsafe icon0Info path refused")
        code, body = http("GET", "/api/icon?id=PPSA01325", raw=True)
        check(code == 404, "missing icon file -> 404")

        print("hide a game and a media app")
        before_games = dump(appdb)
        for tid in ("PPSA01325", "CUSA01065"):
            code, r = http("POST", "/api/hide?id=" + tid)
            check(code == 200, f"hide {tid} ok")
        by_id, _ = listing()
        check(by_id["PPSA01325"]["hidden"] and by_id["PPSA01325"]["tab"] == "games", "hidden game keeps its tab label")
        check(by_id["CUSA01065"]["hidden"] and by_id["CUSA01065"]["tab"] == "media", "hidden media app keeps its tab label")
        for u in USERS:
            vis, disp = q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', "CUSA01065")[0]
            check(vis == 0 and disp == (0xBC & ~6), f"user {u}: media app hidden ({disp:#x})")
        check(q(appdb, "SELECT count(*) FROM tbl_contentinfo")[0][0] == len(APPS), "nothing deleted")
        j = launch("PPSA01325")
        check(j["result"] == "ok", "hidden game can still be launched")
        check(any("would start PPSA01325" in l for l in s.out), "launch reached the system call")
        code, r = http("POST", "/api/launch?id=bad")
        check(code == 400, "launch with bad id rejected")
        for tid in ("PPSA01325", "CUSA01065"):
            http("POST", "/api/unhide?id=" + tid)
        check(dump(appdb) == before_games, "game + app restored exactly")

        print("hide PS Store")
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 200 and r["ok"], "hide store ok")
        for u in USERS:
            vis, disp = q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', STORE)[0]
            check(vis == 0 and disp == (0x8A & ~6), f"user {u}: visible=0, tab bits cleared ({disp:#x})")
            check(q(appdb, f'SELECT count(*) FROM "tbl_group_{u}" WHERE itemId=? AND groupName=\'MyHome\'', STORE)[0][0] == 0,
                  f"user {u}: MyHome pin removed")
            check(q(appdb, f'SELECT count(*) FROM "tbl_group_{u}" WHERE itemId=? AND groupName=\'Collection_1\'', STORE)[0][0] == 1,
                  f"user {u}: other groups untouched")
        after = dump(appdb)
        for name in original:
            if not name.startswith(("tbl_iconinfo_", "tbl_group_")):
                check(after[name] == original[name], f"{name} untouched")
        others = [r for r in after[f"tbl_iconinfo_{USERS[0]}"] if r[0] != STORE]
        check(others == [r for r in original[f"tbl_iconinfo_{USERS[0]}"] if r[0] != STORE], "other tiles untouched")
        code, st = http("GET", "/api/status")
        check(tile(st, STORE)["hidden"] and tile(st, STORE)["on_list"], "status reports store hidden")

        print("hide again is harmless")
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 200, "second hide ok")
        check(dump(appdb) == after, "second hide changed nothing")

        print("show PS Store restores the exact original")
        code, r = http("POST", "/api/unhide?id=" + STORE)
        check(code == 200 and r["ok"], "unhide ok")
        check(dump(appdb) == original, "database identical to original after hide+show")
        code, st = http("GET", "/api/status")
        check(not tile(st, STORE)["hidden"] and not tile(st, STORE)["on_list"], "store visible again")

        print("Media Gallery (no MyHome pin)")
        code, r = http("POST", "/api/hide?id=" + GALLERY)
        check(code == 200, "hide gallery ok")
        check(all(q(appdb, f'SELECT visible FROM "tbl_iconinfo_{u}" WHERE titleId=?', GALLERY)[0][0] == 0 for u in USERS),
              "gallery visible=0 for all users")
        code, st = http("GET", "/api/status")
        check(tile(st, GALLERY)["hidden"], "status reports gallery hidden")

        print("errors")
        code, r = http("POST", "/api/hide?id=NPXS99999")
        check(code == 409 and "not on this console" in r["error"], "unknown title rejected")
        code, r = http("POST", "/api/hide?id=../../etc")
        check(code == 400, "malformed id rejected")
        code, r = http("GET", "/api/hide?id=" + STORE)
        check(code == 405, "GET on action rejected")
        code, r = http("GET", "/api/icon?id=" + STORE, raw=True)
        check(code == 404, "missing icon -> 404")
        code, r = http("GET", "/nope", raw=True)
        check(code == 404, "unknown path -> 404")
        evil = urllib.request.Request(BASE + "/api/unhide?id=" + GALLERY, method="POST", data=b"",
                                      headers={"Origin": "http://evil.example"})
        try:
            urllib.request.urlopen(evil, timeout=5)
            code = 200
        except urllib.error.HTTPError as e:
            code = e.code
        check(code == 403, "request from another web site rejected")
        code, st = http("GET", "/api/status")
        check(tile(st, GALLERY)["hidden"], "rejected request changed nothing")
        same = urllib.request.Request(BASE + "/api/settings?reapply=1", method="POST", data=b"",
                                      headers={"Origin": BASE})
        check(urllib.request.urlopen(same, timeout=5).status == 200, "same-origin request (the menu) allowed")

        print("busy database (another process holds a write lock for 1.5 s)")
        holder = sqlite3.connect(appdb, isolation_level=None, check_same_thread=False)
        holder.execute("BEGIN IMMEDIATE")
        threading.Timer(1.5, lambda: holder.execute("COMMIT")).start()
        t0 = time.time()
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 200 and time.time() - t0 >= 1.2, "waited for lock, then succeeded")
        holder.close()

        print("diagnostics")
        code, d = http("GET", "/api/diag", raw=True)
        d = d.decode()
        check(code == 200 and "tbl_iconinfo_1eb71bbd" in d and "MyHome" in d, "diagnostics lists tables and rows")

        print("settings")
        code, r = http("POST", "/api/settings?open_browser=0")
        check(code == 200, "settings saved")
        s.stop()

        print("system brings tiles back -> re-hidden at next launch")
        con = sqlite3.connect(appdb)
        for u in USERS:
            con.execute(f'UPDATE "tbl_iconinfo_{u}" SET visible=1, dispLocation=138 WHERE titleId=?', (STORE,))
            con.execute(f'INSERT INTO "tbl_group_{u}"(itemId, groupName, type, idx) VALUES(?, \'MyHome\', \'title\', 9)', (STORE,))
        con.commit()
        con.close()
        s = Server(appdb, data)
        check("would open" not in "".join(s.out), "browser not opened when setting is off")
        code, st = http("GET", "/api/status")
        check(st["reapplied_at_launch"] == 1, "one tile re-hidden at launch (gallery was still hidden)")
        check(tile(st, STORE)["hidden"] and tile(st, GALLERY)["hidden"], "both hidden after re-apply")
        check(not st["settings"]["open_browser"], "setting persisted")

        print("show everything")
        code, r = http("POST", "/api/unhide_all")
        check(code == 200 and r["note"].startswith("2"), "unhide_all showed 2 tiles")
        final = dump(appdb)
        for u in USERS:
            check(final[f"tbl_iconinfo_{u}"] == original[f"tbl_iconinfo_{u}"], f"user {u}: icon table back to original")
            check(final[f"tbl_group_{u}"] == original[f"tbl_group_{u}"], f"user {u}: MyHome back to original pin")

        print("second launch replaces the first instance")
        env = dict(os.environ, HH_APP_DB=appdb, HH_DATA_DIR=data)
        p2 = subprocess.Popen([BIN], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        s.p.wait(timeout=10)
        check(s.p.returncode == 0, "first instance exited cleanly")
        time.sleep(0.5)
        code, st = http("GET", "/api/status")
        check(code == 200, "second instance serving")
        http("POST", "/api/quit")
        p2.wait(timeout=10)

        print("restore original backup")
        s = Server(appdb, data)
        http("POST", "/api/hide?id=" + STORE)
        code, r = http("POST", "/api/restore")
        check(code == 200 and r["ok"], "restore ok")
        check(dump(appdb) == original, "app.db equals original backup")
        code, st = http("GET", "/api/status")
        check(not tile(st, STORE)["on_list"], "hide list cleared after restore")
        s.stop()

        print("unknown database layout (other firmware)")
        odd = os.path.join(tmp, "odd.db")
        make_db(odd, layout="unknown")
        odd_before = dump(odd)
        s = Server(odd, os.path.join(tmp, "data2"))
        code, st = http("GET", "/api/status")
        check(not st["db"]["ok"] and "layout" in st["db"]["error"], "status explains unknown layout")
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 409 and "Diagnostics" in r["error"], "hide refused safely")
        check(dump(odd) == odd_before, "odd db untouched")
        s.stop()

        print("missing app.db")
        s = Server(os.path.join(tmp, "missing.db"), os.path.join(tmp, "data3"))
        code, st = http("GET", "/api/status")
        check(not st["db"]["ok"] and "not found" in st["db"]["error"], "status explains missing db")
        check(not os.path.exists(os.path.join(tmp, "missing.db")), "app.db never created")
        s.stop()

        locked_migration_tests(tmp)
        folder_tests(tmp)
        picture_tests(tmp)
        launch_tests(tmp)
        startup_mode_tests(tmp)
        custom_tests(tmp)
        background_tests(tmp)
        live_tests(tmp)
        updater_tests(tmp)
        print("\nALL %d CHECKS PASSED" % passed)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
