#!/usr/bin/env python3
# kitty-helper: the program probe-kitty runs in a Rex block.
#
# The terminal is put in raw mode, then commands arrive as typed lines. Each
# one that asks the terminal something ends its question with a primary
# device attributes request (CSI c), which every terminal answers, and
# reports what came back before that answer. Every line it writes starts at
# column 0 and ends with CR LF.
#
#   K              kitty's support query (a=q); "KQ <reply>"
#   T <id>         a 4x2 RGBA picture sent and shown at the cursor in one
#                  command (a=T), 8 cells by 2; "TX <id> <reply>"
#   B <id> <w> <h> a w x h RGBA picture sent in 4096-byte chunks (m=1) and
#                  shown; "BIG <id> <sha256 of the pixels, 16 hex digits>"
#                  then "TX <id> <reply>"
#   D <id>         delete the picture and its data (a=d,d=I); "DEL <id> <reply>"
#   A / P          enter / leave the alternate screen; "ALT" / "PRIMARY"
#   Q              exit
# A reply shows ESC as ^[ and BEL as ^G; "none" when nothing came before
# the device attributes.

import base64
import hashlib
import os
import re
import select
import sys
import termios
import time
import tty

DA1 = re.compile(rb"\x1b\[\?[0-9;]*c")


def write(data):
    out = sys.stdout.buffer
    out.write(data)
    out.flush()


def line(text):
    write(("\r" + text + "\r\n").encode())


def shown(reply):
    if not reply:
        return "none"
    return reply.decode("latin-1").replace("\x1b", "^[").replace("\x07", "^G")


def ask(fd, question, timeout=5.0):
    """Write question then CSI c; return what came back before the DA1 answer."""
    write(question + b"\x1b[c")
    got = b""
    end = time.monotonic() + timeout
    while True:
        m = DA1.search(got)
        if m:
            return got[: m.start()]
        left = end - time.monotonic()
        if left <= 0:
            return got + b" (no device attributes: timed out)"
        r, _, _ = select.select([fd], [], [], left)
        if r:
            got += os.read(fd, 65536)


def picture(w, h):
    # A pattern that changes with every pixel, so any damage shows in the hash.
    px = bytearray(w * h * 4)
    for i in range(w * h):
        px[i * 4] = (i * 7) & 255
        px[i * 4 + 1] = (i * 13 + 64) & 255
        px[i * 4 + 2] = (i * 29 + 128) & 255
        px[i * 4 + 3] = 255
    return bytes(px)


def send_small(fd, ident):
    data = base64.b64encode(picture(4, 2))
    keys = b"a=T,f=32,s=4,v=2,c=8,r=2,i=%d" % ident
    return ask(fd, b"\x1b_G" + keys + b";" + data + b"\x1b\\")


def send_big(fd, ident, w, h):
    px = picture(w, h)
    line("BIG %d %s" % (ident, hashlib.sha256(px).hexdigest()[:16]))
    data = base64.b64encode(px)
    chunks = [data[i:i + 4096] for i in range(0, len(data), 4096)]
    out = b""
    for n, chunk in enumerate(chunks):
        more = b"1" if n < len(chunks) - 1 else b"0"
        if n == 0:
            keys = b"a=T,f=32,s=%d,v=%d,c=20,r=8,i=%d,m=%s" % (w, h, ident, more)
        else:
            keys = b"m=" + more
        out += b"\x1b_G" + keys + b";" + chunk + b"\x1b\\"
    return ask(fd, out, timeout=30.0)


def main():
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    tty.setraw(fd)
    try:
        line("READY")
        typed = b""
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                return
            typed += chunk
            while b"\r" in typed:
                cmd, typed = typed.split(b"\r", 1)
                w = cmd.decode(errors="replace").split()
                if not w:
                    continue
                if w[0] == "Q":
                    return
                if w[0] == "K":
                    reply = ask(fd, b"\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\")
                    line("KQ " + shown(reply))
                elif w[0] == "T" and len(w) == 2:
                    line("TX %s %s" % (w[1], shown(send_small(fd, int(w[1])))))
                elif w[0] == "B" and len(w) == 4:
                    reply = send_big(fd, int(w[1]), int(w[2]), int(w[3]))
                    line("TX %s %s" % (w[1], shown(reply)))
                elif w[0] == "D" and len(w) == 2:
                    reply = ask(fd, b"\x1b_Ga=d,d=I,i=%d\x1b\\" % int(w[1]))
                    line("DEL %s %s" % (w[1], shown(reply)))
                elif w[0] == "A":
                    write(b"\x1b[?1049h\x1b[H")
                    line("ALT")
                elif w[0] == "P":
                    write(b"\x1b[?1049l")
                    line("PRIMARY")
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)


if __name__ == "__main__":
    main()
