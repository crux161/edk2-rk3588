#!/usr/bin/env python3
"""Run QEMU for scripts/nimbus-qemu-test.sh and capture the splash. Argument: the output dir."""
import os, shutil, socket, subprocess, sys, time

out = sys.argv[1]
code = "/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
vars_fd = os.path.join(out, "vars.fd")
with open(vars_fd, "wb") as f:
    f.truncate(64 * 1024 * 1024)
serial = os.path.join(out, "serial.log")
mon = os.path.join(out, "mon.sock")
cmd = [
    "qemu-system-aarch64", "-M", "virt", "-accel", "hvf", "-cpu", "host", "-m", "1024",
    "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
    "-drive", f"if=pflash,format=raw,file={vars_fd}",
    "-device", "ramfb", "-display", "none",
    "-drive", f"file=fat:rw:{os.path.join(out, 'esp')},format=raw,if=virtio",
    "-serial", f"file:{serial}", "-monitor", f"unix:{mon},server,nowait", "-no-reboot",
]
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))

def log():
    try:
        return open(serial, "rb").read().decode("latin-1")
    except FileNotFoundError:
        return ""

def wait_for(text, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if text in log():
            return True
        time.sleep(0.05)
    return False

def monitor(command):
    s = socket.socket(socket.AF_UNIX)
    s.connect(mon)
    s.recv(4096)
    s.sendall((command + "\n").encode())
    time.sleep(0.3)
    s.close()

shots = []
try:
    for _ in range(100):
        if os.path.exists(mon):
            break
        time.sleep(0.1)
    if not wait_for("NimbusSplash: epoch", 60):
        print("splash never started; serial tail:\n" + log()[-1500:])
        sys.exit(1)
    t0 = time.time()
    for at in (0.3, 0.8, 1.4, 2.2, 3.4, 4.6):
        time.sleep(max(0, t0 + at - time.time()))
        name = f"splash-{at:.1f}s.ppm"
        monitor(f"screendump {os.path.join(out, name)}")
        shots.append(name)
    if wait_for("this line ends the splash", 20):
        time.sleep(0.6)
        monitor(f"screendump {os.path.join(out, 'after-text.ppm')}")
        shots.append("after-text.ppm")
    wait_for("NimbusSplash: stopped", 5)
finally:
    time.sleep(0.5)
    q.terminate()
    q.wait(timeout=10)

for name in shots:
    src = os.path.join(out, name)
    if os.path.exists(src):
        subprocess.run(["sips", "-s", "format", "png", src, "--out", src[:-4] + ".png"], capture_output=True)
for line in log().splitlines():
    if "NimbusSplash" in line:
        print(line.strip())
print("shots:", ", ".join(s[:-4] + ".png" for s in shots))
