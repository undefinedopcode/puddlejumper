# Ghostty snapshot fixtures

The `*.hex` golden fixtures and `snapshot.ksy` (the Kaitai Struct schema of
the format) are copied unchanged from Ghostty,
`src/terminal/snapshot/` at commit
`10473f62e2e5fc60e3098da9f28057e67b696135` (2026-11-24). They are MIT
licensed; see `LICENSE` in this directory.

`test-ghosttysnapshot` decodes each one with `src-widgets/ghosttysnapshot.cpp`
and checks the values Ghostty's own tests put in them.

Each fixture is annotated hexadecimal: two hex digits per byte, whitespace
ignored, and `#` starting a comment. The header comments name the schema type
the bytes hold (`Kaitai type`), its parameters and the offset it starts at.
To update, copy the new files over these and run the test; a fixture whose
name changes version (`-v2`) is a new wire format, not a replacement.
