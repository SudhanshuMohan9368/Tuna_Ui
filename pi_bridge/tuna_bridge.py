#!/usr/bin/env python3
"""TUNA display <-> Klipper (Moonraker) + Wi-Fi bridge. Runs on the Raspberry Pi.

The ESP32 display is a USB serial device; this script answers its CMD|... lines,
pushes run status while Klipper prints, and pushes Wi-Fi status every few seconds.
Message format: README.md ("Serial protocol").

  python3 tuna_bridge.py                 # auto-detect display, Moonraker on localhost
  python3 tuna_bridge.py --port /dev/ttyACM0 -v
"""
import argparse
import glob
import importlib
import json
import logging
import re
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

import serial  # apt install python3-serial

import protocol_config  # same folder - edit this file to set each test's time

log = logging.getLogger("tuna")

MAX_FILES_LINE = 700      # display's receive buffer is 768 bytes
STATUS_PERIOD_S = 1.0
WIFI_PERIOD_S = 5.0
RECOVER_TIMEOUT_S = 25    # firmware_restart -> Klipper "ready" again; display waits 30s


def enc(s):
    """Percent-encode a field so | ; , can't break the line format."""
    return urllib.parse.quote(str(s), safe=" ")


# ----------------------------------------------------------------------------- Moonraker
class Moonraker:
    def __init__(self, url):
        self.url = url.rstrip("/")

    def _call(self, method, path, params=None, timeout=5):
        query = "?" + urllib.parse.urlencode(params) if params else ""
        data = b"" if method == "POST" else None
        req = urllib.request.Request(self.url + path + query, data=data, method=method)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())["result"]

    def get(self, path, **params):
        return self._call("GET", path, params)

    def post(self, path, **params):
        return self._call("POST", path, params)

    def read_gcode(self, filename, limit=2_000_000):
        url = f"{self.url}/server/files/gcodes/{urllib.parse.quote(filename)}"
        with urllib.request.urlopen(url, timeout=10) as r:
            return r.read(limit).decode(errors="replace")


def error_text(exc):
    """Short, human-readable reason from a Moonraker/HTTP failure."""
    if isinstance(exc, urllib.error.HTTPError):
        try:
            return json.loads(exc.read().decode())["error"]["message"]
        except Exception:
            return f"HTTP {exc.code}"
    if isinstance(exc, urllib.error.URLError):
        return "Klipper not reachable"
    return str(exc)


# ----------------------------------------------------------------------------- G-code metadata
def run_info_from_gcode(text):
    """Header comments drive the run screen:
         ; TUNA_TIME=120                       total seconds (else: sum of G4 dwells)
         ; TUNA_STEPS=Binding,Wash 1,Wash 2    step names
         ; TUNA_DESC=One line shown under the title
       and `M117 STEP 2` inside the file marks the current step."""
    meta = {k.upper(): v for k, v in
            re.findall(r"^;\s*TUNA_(TIME|STEPS|DESC)\s*[=:]\s*(.+?)\s*$", text, re.M | re.I)}
    if "TIME" in meta:
        total = int(float(meta["TIME"]))
    else:
        total_ms = 0.0
        for unit, val in re.findall(r"^\s*G4\s+([PS])\s*([\d.]+)", text, re.M | re.I):
            total_ms += float(val) * (1000 if unit.upper() == "S" else 1)
        total = int(total_ms / 1000)
    steps = [s.strip() for s in meta.get("STEPS", "").split(",") if s.strip()]
    return total, steps, meta.get("DESC", "")


# ----------------------------------------------------------------------------- Wi-Fi (nmcli)
def nmcli(*args, timeout=15):
    return subprocess.run(["nmcli", *args], capture_output=True, text=True, timeout=timeout)


def nm_fields(line):
    """Split `nmcli -t` output: ':' separates fields, '\\:' is a literal colon."""
    fields, cur, esc = [], "", False
    for ch in line:
        if esc:
            cur += ch
            esc = False
        elif ch == "\\":
            esc = True
        elif ch == ":":
            fields.append(cur)
            cur = ""
        else:
            cur += ch
    fields.append(cur)
    return fields


def ip_address():
    out = subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout.split()
    return out[0] if out else ""


def wifi_status():
    r = nmcli("-t", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi", "list", "--rescan", "no")
    for line in r.stdout.splitlines():
        f = nm_fields(line)
        if len(f) >= 3 and f[0] == "yes":
            return True, f[1], int(f[2] or 0), ip_address()
    return False, "", 0, ""


def wifi_scan():
    r = nmcli("-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "--rescan", "yes", timeout=30)
    best = {}
    for line in r.stdout.splitlines():
        f = nm_fields(line)
        if len(f) < 3 or not f[0]:
            continue
        ssid, signal, secure = f[0], int(f[1] or 0), f[2] not in ("", "--")
        if ssid not in best or signal > best[ssid][0]:
            best[ssid] = (signal, secure)
    return sorted(((s, sig, sec) for s, (sig, sec) in best.items()), key=lambda x: -x[1])[:20]


def active_wifi_connection():
    r = nmcli("-t", "-f", "NAME,TYPE", "con", "show", "--active")
    for line in r.stdout.splitlines():
        f = nm_fields(line)
        if len(f) >= 2 and f[1] == "802-11-wireless":
            return f[0]
    return None


# ----------------------------------------------------------------------------- Bridge
class Bridge:
    def __init__(self, moonraker, wifi_enabled):
        self.mr = moonraker
        self.wifi_enabled = wifi_enabled
        self.ser = None
        self.tx_lock = threading.Lock()
        self.run = None            # dict(file, total, steps, desc) for the current print
        self.last_state = None
        self.wifi_busy = False

    # --- serial out
    def send(self, line):
        with self.tx_lock:
            if not self.ser:
                return
            try:
                self.ser.write((line + "\n").encode())
                log.debug(">> %s", line)
            except serial.SerialException as e:
                log.warning("write failed: %s", e)

    def send_files(self, retries=0, retry_delay=2):
        # retries>0 is for the very first push on connect: Klipper/Moonraker
        # can still be starting up right as the bridge and display come up
        # together, and a bare failure here left the display showing its
        # placeholder list with no way to recover short of a reboot.
        for attempt in range(retries + 1):
            try:
                items = self.mr.get("/server/files/list", root="gcodes")
                break
            except Exception as e:
                if attempt < retries:
                    time.sleep(retry_delay)
                    continue
                self.send(f"ERR|{enc(error_text(e))}")
                return
        names = sorted(i["path"] for i in items if i["path"].lower().endswith(".gcode"))
        line = "FILES|"
        for n in names:
            if len(line) + len(n) + 1 > MAX_FILES_LINE:
                log.warning("file list truncated for the display")
                break
            line += ("" if line.endswith("|") else ";") + n
        self.send(line)

    def send_run(self):
        r = self.run
        steps = ";".join(enc(s) for s in r["steps"])
        self.send(f"RUN|{r['file']}|{r['total']}|{steps}|{enc(r['desc'])}")

    def load_run(self, filename):
        # Reloaded every run so editing protocol_config.py takes effect on
        # the next Start run, no service restart needed.
        importlib.reload(protocol_config)
        cfg = protocol_config.PROTOCOLS.get(filename)
        if cfg:
            total = int(cfg.get("time", 0))
            steps = list(cfg.get("steps", []))
            desc = cfg.get("desc", "")
        else:
            try:
                total, steps, desc = run_info_from_gcode(self.mr.read_gcode(filename))
            except Exception as e:
                log.warning("could not read %s for run info: %s", filename, e)
                total, steps, desc = 0, [], ""
        self.run = {"file": filename, "total": total, "steps": steps, "desc": desc}

    # --- serial in
    def handle(self, line):
        if not line.startswith("CMD|"):
            return  # display debug echo (">> ..."), boot messages, noise
        parts = line.split("|")
        cmd = parts[1] if len(parts) > 1 else ""
        log.info("<< %s", "CMD|WIFI_CONNECT|..." if cmd == "WIFI_CONNECT" else line)

        try:
            if cmd == "LIST":
                self.send_files()
                if self.run and self.last_state in ("printing", "paused"):
                    self.send_run()  # display rebooted mid-run
            elif cmd == "PRINT" and len(parts) >= 3:
                self.load_run(parts[2])
                self.send_run()
                self.mr.post("/printer/print/start", filename=parts[2])
                self.send("OK|PRINT")
            elif cmd == "CANCEL":
                self.mr.post("/printer/print/cancel")
                self.send("OK|CANCEL")
            elif cmd == "ESTOP":
                self.mr.post("/printer/emergency_stop")
            elif cmd == "RECOVER":
                threading.Thread(target=self.do_recover, daemon=True).start()
            elif cmd == "WIFI_SCAN" and self.wifi_enabled:
                threading.Thread(target=self.do_wifi_scan, daemon=True).start()
            elif cmd == "WIFI_CONNECT" and self.wifi_enabled and len(parts) >= 3:
                ssid = urllib.parse.unquote(parts[2])
                pw = urllib.parse.unquote(parts[3]) if len(parts) > 3 else ""
                threading.Thread(target=self.do_wifi_connect, args=(ssid, pw), daemon=True).start()
        except Exception as e:
            log.warning("%s failed: %s", cmd, e)
            self.send(f"ERR|{enc(error_text(e))}")

    # After an E-stop (M112), Klipper's MCU connection is in a shutdown
    # state and refuses everything, including G28, until FIRMWARE_RESTART
    # brings it back. Runs off the serial thread since the restart+home
    # can take several seconds.
    def do_recover(self):
        try:
            self.mr.post("/printer/firmware_restart")
        except Exception as e:
            log.warning("firmware_restart failed: %s", e)
            self.send(f"ERR|RECOVER|{enc(error_text(e))}")
            return

        deadline = time.time() + RECOVER_TIMEOUT_S
        ready = False
        while time.time() < deadline:
            time.sleep(1)
            try:
                if self.mr.get("/printer/info").get("state") == "ready":
                    ready = True
                    break
            except Exception:
                continue  # Klipper is mid-restart; keep polling
        if not ready:
            self.send("ERR|RECOVER|Klipper did not come back after restart")
            return

        try:
            self.mr.post("/printer/gcode/script", script="G28")
            self.send("OK|RECOVER")
        except Exception as e:
            log.warning("G28 after recover failed: %s", e)
            self.send(f"ERR|RECOVER|{enc(error_text(e))}")

    # --- Klipper status push
    def status_loop(self):
        offline_logged = False
        while True:
            time.sleep(STATUS_PERIOD_S)
            try:
                q = self.mr.get("/printer/objects/query", print_stats="", display_status="")
                offline_logged = False
            except Exception as e:
                if not offline_logged:
                    log.warning("Moonraker query failed: %s", error_text(e))
                    offline_logged = True
                continue

            ps = q["status"].get("print_stats", {})
            state = ps.get("state", "")
            elapsed = int(ps.get("print_duration", 0))
            msg = (q["status"].get("display_status", {}) or {}).get("message") or ""
            m = re.search(r"STEP\s+(\d+)", msg, re.I)
            step = int(m.group(1)) if m else 0

            if state in ("printing", "paused"):
                fname = ps.get("filename", "")
                if not self.run or self.run["file"] != fname:
                    self.load_run(fname)  # started from Mainsail/Fluidd, not the display
                    self.send_run()
                self.send(f"STATUS|{state}|{elapsed}|{step}")
            elif state != self.last_state and self.last_state in ("printing", "paused"):
                self.send(f"STATUS|{state}|{elapsed}|{step}")  # complete / cancelled / error
                self.run = None
            self.last_state = state

    # --- Wi-Fi
    def wifi_loop(self):
        while True:
            try:
                ok, ssid, signal, ip = wifi_status()
                self.send(f"WIFI|1|{enc(ssid)}|{signal}|{ip}" if ok else "WIFI|0")
            except FileNotFoundError:
                log.warning("nmcli not found - Wi-Fi reporting disabled")
                return
            except Exception as e:
                log.debug("wifi status failed: %s", e)
            time.sleep(WIFI_PERIOD_S)

    def do_wifi_scan(self):
        try:
            nets = wifi_scan()
            self.send("WIFI_LIST|" + ";".join(f"{enc(s)},{sig},{int(sec)}" for s, sig, sec in nets))
        except Exception as e:
            self.send(f"ERR|WIFI|{enc(str(e))}")

    def do_wifi_connect(self, ssid, pw):
        if self.wifi_busy:
            self.send("ERR|WIFI|busy")
            return
        self.wifi_busy = True
        try:
            previous = active_wifi_connection()
            existed = ssid in [nm_fields(l)[0] for l in nmcli("-t", "-f", "NAME", "con", "show").stdout.splitlines()]
            args = ["dev", "wifi", "connect", ssid] + (["password", pw] if pw else [])
            r = nmcli(*args, timeout=60)
            if r.returncode == 0:
                log.info("connected to %s", ssid)
                self.send(f"OK|WIFI|{ip_address()}")
                return
            reason = (r.stderr.strip().splitlines() or ["connection failed"])[-1]
            if "Secrets were required" in reason or "802-1X" in reason:
                reason = "wrong password"
            log.warning("connect to %s failed: %s", ssid, reason)
            self.send(f"ERR|WIFI|{enc(reason)}")
            # Don't leave the machine offline or keep a profile with a bad password.
            if not existed:
                nmcli("con", "delete", ssid)
            if previous and previous != ssid:
                nmcli("con", "up", previous, timeout=60)
        except Exception as e:
            self.send(f"ERR|WIFI|{enc(str(e))}")
        finally:
            self.wifi_busy = False

    # --- main serial loop (reconnects if the display is unplugged)
    def serial_loop(self, port_arg):
        while True:
            port = port_arg or find_display_port()
            if not port:
                time.sleep(2)
                continue
            try:
                ser = serial.Serial()
                ser.port, ser.baudrate, ser.timeout = port, 115200, 1
                ser.dtr = ser.rts = False  # keep the ESP32 from resetting when the port opens
                ser.open()
            except serial.SerialException as e:
                log.warning("cannot open %s: %s", port, e)
                time.sleep(2)
                continue

            log.info("display connected on %s", port)
            with self.tx_lock:
                self.ser = ser
            self.send_files(retries=5, retry_delay=3)  # ride out Moonraker still starting up
            buf = b""
            try:
                while True:
                    buf += ser.read(ser.in_waiting or 1)
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode(errors="replace").strip()
                        if line:
                            self.handle(line)
            except (serial.SerialException, OSError) as e:
                log.warning("display disconnected: %s", e)
            finally:
                with self.tx_lock:
                    self.ser = None
                ser.close()
                time.sleep(2)


def find_display_port():
    for pattern in ("/dev/serial/by-id/*Espressif*", "/dev/ttyACM*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="display serial port (default: auto-detect)")
    ap.add_argument("--moonraker", default="http://127.0.0.1:7125")
    ap.add_argument("--no-wifi", action="store_true", help="don't report or change Wi-Fi")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    bridge = Bridge(Moonraker(args.moonraker), wifi_enabled=not args.no_wifi)
    threading.Thread(target=bridge.status_loop, daemon=True).start()
    if bridge.wifi_enabled:
        threading.Thread(target=bridge.wifi_loop, daemon=True).start()
    bridge.serial_loop(args.port)


if __name__ == "__main__":
    main()
