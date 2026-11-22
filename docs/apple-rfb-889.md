# Apple RFB 003.889 in the VNC client: a spec

Status: phases 1–3 implemented and checked against macOS 14.8.9 (see "What was
built"); phase 4, the record layer, is not.

## Goal

Against a Mac, puddlejumper's VNC tab should:

1. share the clipboard's text in both directions, in Unicode;
2. take the Mac's Adaptive picture (encoding `0x3f3`), which costs a fraction of
   ZRLE's bytes on a real network.

Both need Apple's own revision of the protocol, RFB 003.889. Today the client
answers a Mac with 3.8, where the Mac ignores cut-text and offers only the
standard encodings.

Not in this spec: High Performance mode (HEVC and sound over a media stream on
a virtual display), files and images on the pasteboard, choosing among a Mac's
displays, server-side scaling, and login types other than 30.

## Source

Everything about the wire here comes from remotex's write-up,
`docs/apple-vnc-889.md` (github.com/andrewtheguy/remotex, MIT), measured against
macOS 26. Apple documents none of it. The section names below ("Connecting",
"Other messages", …) refer to that document, and its source files
(`src/vnc_apple.rs`, `src/vnc_apple_clipboard.rs`, `frontend/wasm/apple-adaptive`)
settle byte layouts this spec leaves out. Code ported from them keeps the MIT
notice; MIT code can go into this GPL project.

Our test Mac runs macOS 14. Where it differs from macOS 26 is unknown, which is
why phase 0 exists.

## When 889 is spoken

- The server's greeting is `RFB 003.889` → answer `RFB 003.889`. Any other
  server gets what it gets today.
- If the security list that follows has no type 30, reconnect and speak 3.8.
  889 with a VNC password is unmeasured, and 3.8 is known to work.
- `PUDDLEJUMPER_VNC_APPLE=0` keeps 3.8 for a Mac, as the way back if a macOS update
  breaks any of this.

`VncClient` gains `m_apple`, true for an 889 session. Every difference below
hangs off it. The differences stay in `vncclient.cpp`, except the tile decoder
and the pasteboard archive, which get files of their own.

## Session setup

In order, after the type-30 login the client already does:

1. **ClientInit** `0x81` in place of `0x01`.
2. **Extended ServerInit.** The name field is a zero `u16`, a `u32` of flags, a
   16-byte bitmap of the client messages the Mac accepts, then the name. Keep
   the flags and the bitmap. Flag `0x01` (observe only) is shown in the status
   bar.
3. **The prelude:** `ViewerInfo` (`0x21`, 66 bytes, capability bit 20 set so
   the Mac sends `MiscStatus`), `SetMode(control)` (`0x0a`), and
   `AutoPasteboard(start)` (`0x15`). In that order, or the Mac sends no
   pasteboard notifications. A mis-sized `ViewerInfo` hangs the session, so its
   bytes are copied from remotex, not composed.
4. **SetPixelFormat** (32-bit, as today) and **SetEncodings**: the picture's
   codec first (below), then ZRLE, then the cursor cache `0x450`, `CursorPos`
   `0x44c`, `AppleDisplayLayout` `0x451` and `DisplayInfo` `0x44d`.
5. One full **FramebufferUpdateRequest**.
6. On the first display layout, and after every later one,
   **AutoFrameBufferUpdate** (`0x09`) for the whole framebuffer at interval 0.
   From then on the client sends no update request after an update: the Mac
   pushes.

### Encryption: not in the first version

Apple's viewer leaves a session in cleartext after the login unless a
preference asks otherwise, so 889 without the record layer is a path the Mac
supports. It is also what puddlejumper's Mac sessions are today. The first version
does the same.

The record layer (AES-128-CBC records with a SHA-1 check, and a rekey message)
is phase 4. It needs an AES decrypt beside `vnccrypto`'s encrypt, and a layer
between the socket and the parser in both directions. remotex always encrypts,
so its measurements never ran in cleartext: phase 0 checks that the pasteboard
and the Adaptive picture work without it.

## Messages from the Mac

`State::Normal` learns two types, and fails on any other as today:

| Type | | Handling |
|---|---|---|
| `0x14` | `MiscStatus`, 8 bytes | command 2 → fetch the pasteboard; 3 → send our text again; 1 → the Mac's user ended the session, said when the connection then closes; the rest are stepped over |
| `0x1f` | pasteboard contents | below |

`parseRect` learns these, each a rectangle in a framebuffer update:

| Encoding | | Handling |
|---|---|---|
| `0x451` | display layout | resize the framebuffer to the size it gives; arm the pushes again |
| `0x44d` | older display info | step over |
| `0x450` | cursor cache | keep each shape by its index, emit `cursorChanged` |
| `0x44c` | cursor position | no payload |
| `0x453`, `0x455` | vendor keysyms, keyboard source | each starts with a `u16` length: step over |
| `0x3f3` | Adaptive tiles | below |

The layout's length prefix counts what follows it, not itself. An encoding the
client did not list ends the session with its number in the message, decimal
and hex.

## Input

- **Buttons.** On 889 the Mac reads the mask as left, right, middle. Swap the
  mask's `0x02` and `0x04`, or a right-click arrives as a middle-click.
- **Scrolling.** When the ServerInit bitmap lists `0x17`, send the 58-byte
  scroll-wheel event with `QWheelEvent::pixelDelta()` as the point delta (the
  angle delta scaled, where the device gives no pixels), so a trackpad scrolls
  smoothly. `RemoteClient` gains `virtual bool sendScroll(const QPoint &pos,
  const QPointF &delta)` returning false by default, and `VncView::wheelEvent`
  falls back to today's button pulses when it does. Without `0x17`, the pulses
  go one bit at a time, as the Mac takes only a mask of exactly `0x08` or `0x10`.
- **Keys.** Unchanged, on the assumption that the Mac maps keysyms on 889 as it
  does on 3.8. Phase 1 checks Command, Option and a shifted shortcut.

## The pasteboard

Text only. `VncView` already decides when to hand the local clipboard over and
remembers what it sent; nothing changes there.

### From the Mac

1. `MiscStatus` 2 arrives.
2. Send a fetch (`0x0b`) for promises only, bytes 4–7 a counter.
3. The reply lists the flavors with no data. If none is text, stop.
4. Send a full fetch. Replies whose echoed counter is not the latest are read
   and dropped.
5. From the reply's first item take `public.utf8-plain-text`, or failing that
   `public.utf16-plain-text`, and emit `clipboardReceived`, unless it is the text
   the client itself last sent.

A full reply carries every flavor of every item, so one copied word can come in
megabytes of RTF and images. The reply is therefore consumed as it arrives, in
a state of its own (`State::Pasteboard`, counting the compressed bytes left),
inflated in pieces and parsed as a stream, keeping only the wanted flavor.
Limits: text over `kMaxText` is dropped, and a reply declaring more than 100 MiB
uncompressed is read past without inflating.

### To the Mac

`sendClipboard(text)` sends a `0x1f` holding one item with one flavor,
`public.utf8-plain-text`, with its data: no promise, so the Mac never has to
ask. Empty text sends nothing, so an image on the local clipboard does not
clear the Mac's. The archive is deflated at level 9 and ended as remotex ends
it.

### Files

`applepasteboard.cpp/.h`: `QByteArray packText(const QString &)` and a
`PasteboardReader` fed compressed bytes that yields flavor names and the text.
No Qt GUI or socket in it, so it is tested alone.

## The picture

### Which codec

The Mac codes every rectangle in the first of zlib, ZRLE and `0x3f3` that
`SetEncodings` lists. So the list is a quality setting:

| Quality | List starts | |
|---|---|---|
| Adaptive | `0x3f3`, ZRLE | lossy at first, refined when the screen is still |
| Full | zlib, ZRLE | lossless |

Full is the default. **Adaptive Quality (Mac)** is a setting of the tab, in its
context menu beside **Reduced Colour** and saved with it (`"adaptive": true`);
it does nothing to a server that is not a Mac in 889. `0x3f3` decodes to 32-bit
only, so Reduced Colour is not applied while Adaptive is on.
`PUDDLEJUMPER_VNC_ENCODINGS` knows `adaptive`.

Changing the setting connects again, as changing the colour depth does. A new
`SetEncodings` mid-session is not enough: re-listing `0x3f3` after ZRLE does not
start the tiles afresh, and while the Mac pushes updates a request for the whole
picture is not answered with one.

`0x3e8`–`0x3ea` (1-bit, 4-bit grey, RGB555) are not listed, so never sent.

### The decoder

`appleadaptive.cpp/.h`, a port of remotex's `apple-adaptive`:

```cpp
class AppleAdaptive {
public:
    void resize(int width, int height);   // drops tile state, keeps tables and cache
    // One rectangle's body, onto the whole screen in RGB32. False, with why,
    // for one that does not decode.
    bool decode(const QRect &rect, const uchar *body, qsizetype size, QImage *image,
                bool *drew, QString *error);
};
```

- **Framing.** A rectangle is a `u32` length, at most 100,000,000, and a body.
  The body's first byte is 0 (full update), 1 (partial update) or 2 (the two
  64-entry quantization tables, in a 0×0 rectangle).
- **Full update.** A command stream and a data stream, read most significant
  bit first. Commands give runs of 8×8 tiles: white, copy of the previous,
  copy of the one above, two-colour masks, one or two colours, DCT, or a cached
  tile. A DCT tile is Y with its AC coefficients short of their low bits, and
  flat Cb and Cr.
- **Partial update.** A 2-bit code per tile: leave, refine (the missing Y bits,
  and chroma AC coefficients Huffman-coded with JPEG's luminance AC table),
  repeat the full update's copy, or take a cached tile.
- **State.** Per framebuffer tile, the precision and coefficients of its last
  full update, one signed byte each (about 26 MB at 4K). A cache of refined
  tiles, indices 1–64,999 (about 6 MB).
- **Arithmetic.** libjpeg's integer inverse DCT and JPEG's YCbCr conversion,
  written out here: about 80 lines, and no new dependency.

The write-up's "Apple's own framebuffer encodings" section has the bit-level
rules; this spec does not repeat them.

**When a rectangle does not decode** (a stream that does not end on its mark,
most likely), the decoder is out of step with the Mac and every later tile
would be wrong. The client logs it and connects again with zlib first, keeping
the login it has, and stays lossless until the tab is connected anew.

## Phases

| | What | Rough size |
|---|---|---|
| 0 | A probe, by hand: 889 login on the macOS 14 VM, in cleartext. Dump ServerInit, what the prelude draws, a promises-only fetch, and whether listing `0x3f3` produces tiles. | half a day |
| 1 | 889 sessions at today's level: setup, layout, cursor cache, pushes, buttons, scroll. Full quality only. | 1–2 days |
| 2 | The pasteboard, both ways. | 1 day |
| 3 | The Adaptive decoder and the quality setting. | 2–3 days |
| 4 | The record layer, if wanted. | 1 day |

Each phase leaves a client that works. Phase 2 is what fixes pasting.

## Testing

- **Unit tests** (the project has none yet; these would be the first):
  `PasteboardReader` against archives built by hand and against a captured one;
  `AdaptiveDecoder` against captured rectangles.
- **Captures.** `PUDDLEJUMPER_VNC_DUMP=<file>` writes every byte received. The
  probe replays a dump into the client with no server, which turns a session
  into a regression test. For the decoder, the same dump goes through remotex's
  decoder and the two pictures must match exactly.
- **Live**, with `probe-vnc`: text copied on the Mac arrives (including
  non-Latin text); text sent appears on a paste in the Mac; copying an image on
  the Mac changes nothing locally; a right-click opens a context menu; a
  scrolled window moves by the distance sent; a login and logout at the Mac
  leave the picture and the pointer live.

Live tests need a login on the Mac VM, and the login is the account's password.
Either someone types it each run, or the VM gets a throwaway account on Screen
Sharing's allow list whose password can sit in the probe's environment.

## Phase 0 results (2026-11-17, macOS 14.8.9)

A hand probe (Python, not in the repository) logged in over 889 and stayed in
cleartext. Everything below worked without the record layer.

- **Login.** The Mac offers types 30, 33, 36 and 35. Type 30 uses a 128-byte
  group with generator 2 here, not macOS 26's 512-byte one; the client's
  existing login handles both.
- **ServerInit.** Flags `0x52`; the bitmap lists `0x17` (scroll), `0x09`
  (pushes), `0x0b`, `0x15` and `0x1f` (pasteboard), and not `0x1d`, so this
  macOS has no High Performance mode.
- **Metadata.** The layout, vendor keysyms, keyboard source, cursor cache and
  `CursorPos` arrive framed as the write-up says. The layout's flags are `0x1`
  where the write-up has `0x4` for Standard; the client must not depend on them.
- **Pushes.** Armed at 0 after the layout; updates arrive with no request.
- **Pasteboard.** Text sent in a `0x1f` came back intact (`héllo ✓ from linux`)
  from a full fetch. A promises-only reply was 177 bytes: the flavor names with
  no data. The Mac lists the text as `public.utf8-plain-text`,
  `com.apple.traditional-mac-plain-text` and `public.utf16-plain-text`.
- **Echo.** Setting the Mac's pasteboard draws its own `MiscStatus` 2 half a
  second later. The client must not hand that text back to the local clipboard
  as news: it compares against what it last sent.
- **Adaptive.** Listing `0x3f3` first brought one quantization rectangle, one
  full update of the 1920×1080 login window in 6 kB, and 90 partial updates:
  34 kB in all, where ZRLE's first picture was 146 kB and zlib's 172 kB.
  Captured in a dump for the decoder's tests.

Not yet checked: the pasteboard of a logged-in user (the probe saw the login
window's), input, and a Retina display.

## What was built (2026-11-17, macOS 14.8.9)

Checked with `probe-vnc` against the VM's Screen Sharing, logged in as a test
account.

- **Sessions.** 889 handshake, account login, layout, cursor cache, pushed
  updates (hundreds with no request), button swap (a right-click opens a
  context menu), and scroll by distance (200 px sent moved the Mac's view
  200 px). `PUDDLEJUMPER_VNC_APPLE=0` speaks plain 3.8 as before.
- **A session of its own.** Logging in as an account that is not at the Mac's
  console gives that account a virtual session, which stays between
  connections; the console user's screen is not shown.
- **MiscStatus.** Codes 4 (a heartbeat), 11 and 12 arrive beside 1–3 and are
  stepped over.
- **Pasteboard.** Text sent was pasted on the Mac with Cmd+V, non-Latin
  characters intact; text copied on the Mac arrived through a promises fetch
  and then a full one; the echo of our own text was dropped.
- **Adaptive.** Two sessions (518 and 1,986 rectangles, 1,793 of them partial
  updates, over photo thumbnails, scrolling and window changes) decoded with
  no error. The same rectangles through remotex's decoder gave the same
  picture, pixel for pixel. Against zlib's picture of the same screen the
  settled one differs by under half a level on average.
- **Fallback.** A failure forced in a test build connected again with zlib,
  without asking for the login, and the picture then matched zlib's exactly.
  Sending `SetEncodings` and a full request instead left the dock and other
  untouched areas as Adaptive had drawn them, which is why it reconnects.

Not built: unit tests and the byte-level capture replay under "Testing" (the
decoder comparison was done by hand with a throwaway harness), files on the
pasteboard, and the record layer.

## Open questions

1. Answered: a user's own copy in a logged-in session reaches us. What other
   flavors do beside the text (an image, a file) is unchecked.
2. Pushes at interval 0 can leave input unread on the Mac while the client
   falls behind. puddlejumper decodes natively and should keep up; if a slow link
   shows the problem, arm at 33,333 µs instead.
3. `0x3f3` was never captured from a physical Mac or at Retina density.
4. macOS 26 is untested here; the write-up covers it, this work covers 14.
5. A login and logout at the Mac during a session, and modifier keys beyond
   Command, are unchecked.
