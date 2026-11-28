#!/usr/bin/env python3
# osc6522-helper: the program probe-osc6522 runs in a Rex block.
#
# The terminal is put in raw mode, then commands arrive as typed lines:
#   E <n> st|bel   write an OSC 6522 sequence with <n> bytes of data, ended
#                  with ST or BEL, then "EMITTED <n> <terminator>"
#   Q              exit
# Any OSC 6522 sequence that arrives as input is answered with
# "GOT <length of its body> <first 16 hex digits of the body's SHA-256>".
# Every line it writes starts at column 0 and ends with CR LF.

import hashlib
import os
import sys
import termios
import tty

ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def pattern(n):
    # The same characters probe-osc6522 expects: a run it can check byte by byte.
    return "".join(ALPHABET[(i * 7 + n) % 64] for i in range(n))


def write(data):
    out = sys.stdout.buffer
    out.write(data)
    out.flush()


def line(text):
    write(("\r" + text + "\r\n").encode())


def emit(n, terminator):
    body = "op=probe:n=%d:data=%s" % (n, pattern(n))
    end = b"\x1b\\" if terminator == "st" else b"\x07"
    write(b"\x1b]6522;" + body.encode() + end)
    line("EMITTED %d %s" % (n, terminator))


def main():
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    tty.setraw(fd)
    try:
        line("READY")
        typed = b""      # a command line being typed
        seq = None       # the body of an OSC 6522 sequence being received
        pending = b""    # bytes that may start a sequence or end one
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                return
            data = pending + chunk
            pending = b""
            i = 0
            while i < len(data):
                if seq is not None:
                    bel = data.find(b"\x07", i)
                    st = data.find(b"\x1b\\", i)
                    ends = [e for e in (bel, st) if e >= 0]
                    if not ends:
                        # Keep a trailing ESC: it may be half of ST.
                        if data.endswith(b"\x1b"):
                            seq += data[i:-1]
                            pending = b"\x1b"
                        else:
                            seq += data[i:]
                        i = len(data)
                        break
                    end = min(ends)
                    seq += data[i:end]
                    line("GOT %d %s" % (len(seq), hashlib.sha256(seq).hexdigest()[:16]))
                    seq = None
                    i = end + (1 if end == bel else 2)
                    continue
                start = data.find(b"\x1b]6522;", i)
                if start < 0:
                    # A partial opener at the end waits for the next read.
                    tail = data[i:]
                    for k in range(min(len(tail), 7), 0, -1):
                        if b"\x1b]6522;".startswith(tail[-k:]):
                            pending = tail[-k:]
                            tail = tail[:-k]
                            break
                    typed += tail
                    i = len(data)
                else:
                    typed += data[i:start]
                    seq = b""
                    i = start + len(b"\x1b]6522;")
                # Commands end with CR.
                while b"\r" in typed:
                    cmd, typed = typed.split(b"\r", 1)
                    words = cmd.decode(errors="replace").split()
                    if words[:1] == ["Q"]:
                        return
                    if len(words) == 3 and words[0] == "E":
                        emit(int(words[1]), words[2])
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)


if __name__ == "__main__":
    main()
