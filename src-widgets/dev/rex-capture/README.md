# Rex terminal stream captures

Captured from tuxosaurus (github.com/wesleygrimes/tuxosaurus, a Rex-compatible
server whose protocol suite also runs against Rex Beta 0.1.0) on loopback, by a
small probe client: a `/bin/sh` session, 80x24.

- `shell-welcome.bin`: the 32-byte data stream WELCOME (81, reconnect token,
  3 reserved bytes, grace ms u32 LE, nonce u64 LE).
- `shell-stream.bin`: every payload byte after it. The snapshot (60 lines of
  history, colours, wide characters, a combining accent, a wrapped line, a
  title, an SGR pen left on), a resize marker, then live output, a resize to
  70x20 and back to 80x24 (a marker each), more output.
- `shell-screen.txt`: the server's own text of the terminal afterwards
  (com.superlogical.terminal.format with trim), scrollback included.

Used by test-rexterminalstream.
