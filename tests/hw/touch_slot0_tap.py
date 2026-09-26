# Touch restart seed gate helper (runs on unit A, python3): a finger as the
# GT9895 driver reports one - multitouch slot 0, ABS_MT_* only, no ABS_X/ABS_Y
# - so the input core filters it exactly as it filters a real finger.
# (The RIFT/Zabbix gates' rift_tap.py also writes ABS_X/ABS_Y and lands in
# whatever slot the driver left current, which on this driver is slot 9.)
# See docs/hardware/TOUCH_RESTART_SEED_GATE.md.
#   touch_slot0_tap.py X Y          tap the logical point of the current display
#   touch_slot0_tap.py raw RX RY    tap raw controller values
#   touch_slot0_tap.py where RX RY  print the logical point raw RX RY maps to now
import os, re, struct, sys, time

TOUCH = "/dev/input/event1"
LOG = "/var/lib/pocketos/log/shell.log"
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH = 0x14A
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2F, 0x35, 0x36, 0x39


def calibration():
    line = None
    with open(LOG, encoding="utf-8", errors="replace") as f:
        for l in f:
            if "touch raw" in l and "calibration" in l:
                line = l
    m = re.search(r"rotation (\d+): swap (\d), calibration (-?\d+),(-?\d+),(-?\d+),(-?\d+) onto (\d+)x(\d+)", line)
    rot, swap, x1, y1, x2, y2, w, h = (int(v) for v in m.groups())
    return {"swap": swap, "cal": (x1, y1, x2, y2), "w": w, "h": h}


def forward(v, a, b, out_max):
    # LVGL _evdev_calibrate with out_min 0
    if a != b:
        v = int((v - a) * out_max / (b - a))
    return max(0, min(out_max, v))


def logical(rx, ry, cal):
    x1, y1, x2, y2 = cal["cal"]
    sx, sy = (ry, rx) if cal["swap"] else (rx, ry)
    return forward(sx, x1, x2, cal["w"] - 1), forward(sy, y1, y2, cal["h"] - 1)


def raw_for(lx, ly, cal):
    x1, y1, x2, y2 = cal["cal"]

    def invert(target, a, b, out_max):
        best = None
        for v in range(min(a, b), max(a, b) + 1):
            got = forward(v, a, b, out_max)
            if got == target:
                return v
            if best is None or abs(got - target) < abs(forward(best, a, b, out_max) - target):
                best = v
        return best

    first = invert(lx, x1, x2, cal["w"] - 1)
    second = invert(ly, y1, y2, cal["h"] - 1)
    return (second, first) if cal["swap"] else (first, second)


def ev(fd, t, c, v):
    os.write(fd, struct.pack("qqHHi", 0, 0, t, c, v))


def tap(rx, ry):
    fd = os.open(TOUCH, os.O_WRONLY)
    tid = 400 + int(time.time()) % 1000
    ev(fd, EV_ABS, ABS_MT_SLOT, 0)
    ev(fd, EV_ABS, ABS_MT_TRACKING_ID, tid)
    ev(fd, EV_ABS, ABS_MT_POSITION_X, rx)
    ev(fd, EV_ABS, ABS_MT_POSITION_Y, ry)
    ev(fd, EV_KEY, BTN_TOUCH, 1)
    ev(fd, EV_SYN, 0, 0)
    time.sleep(0.15)
    ev(fd, EV_ABS, ABS_MT_SLOT, 0)
    ev(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
    ev(fd, EV_KEY, BTN_TOUCH, 0)
    ev(fd, EV_SYN, 0, 0)
    os.close(fd)
    time.sleep(0.4)


def main():
    cal = calibration()
    if sys.argv[1] == "where":
        rx, ry = int(sys.argv[2]), int(sys.argv[3])
        print("%d %d" % logical(rx, ry, cal))
        return
    if sys.argv[1] == "raw":
        rx, ry = int(sys.argv[2]), int(sys.argv[3])
    else:
        rx, ry = raw_for(int(sys.argv[1]), int(sys.argv[2]), cal)
    tap(rx, ry)
    lx, ly = logical(rx, ry, cal)
    print("TAP raw %d,%d -> logical %d,%d on %dx%d" % (rx, ry, lx, ly, cal["w"], cal["h"]))


main()
