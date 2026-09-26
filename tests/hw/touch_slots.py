# Read-only: what the input core holds for the touch node (touch restart seed).
#   touch_slots.py [/dev/input/event1]
import fcntl, struct, sys

dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/input/event1"
fd = open(dev, "rb", buffering=0)


def ioc(nr, size):
    return (2 << 30) | (size << 16) | (ord("E") << 8) | nr


def absinfo(code):
    b = fcntl.ioctl(fd, ioc(0x40 + code, 24), bytes(24))
    return dict(zip(("value", "min", "max", "fuzz", "flat", "res"), struct.unpack("6i", b)))


def slots(code, n=10):
    b = fcntl.ioctl(fd, ioc(0x0A, 4 * (n + 1)), struct.pack("I", code) + bytes(4 * n))
    return list(struct.unpack("%di" % n, b[4:]))


for name, code in (("ABS_X", 0x00), ("ABS_Y", 0x01), ("ABS_MT_SLOT", 0x2F),
                   ("ABS_MT_POSITION_X", 0x35), ("ABS_MT_POSITION_Y", 0x36)):
    try:
        print("%-18s %s" % (name, absinfo(code)))
    except OSError as e:
        print("%-18s EVIOCGABS failed: %s" % (name, e))
for name, code in (("slots X", 0x35), ("slots Y", 0x36), ("slots id", 0x39)):
    try:
        print("%-18s %s" % (name, slots(code)))
    except OSError as e:
        print("%-18s EVIOCGMTSLOTS failed: %s" % (name, e))
