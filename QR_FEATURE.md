# Feature: QR code on HDMI via UART command

Adds QR commands to the existing line protocol. The Pi encodes the payload and
paints a scannable QR on the HDMI screen. Two display modes plus a clear:

| Command | Effect |
|---------|--------|
| `qrfull <text>` | Full-screen QR (clears the screen), centered, with a caption. |
| `qr <text>` | Alias for `qrfull` (kept for back-compat + the ESP32 hook). |
| `qrsmall <text>` | Small QR in the empty band **between the colour bar and the marquee** — the clock dashboard keeps running around it. |
| `qr off` / `qrfull off` / `qrsmall off` | Hide the QR and restore the normal view (`clear` works too). |

Works on Pi 1, Pi 3, and Pi 400 from the same sources.

```
qrfull HELLO PI400        # full-screen QR                  -> "ok qr"
qrsmall https://site      # small QR, clock still visible    -> "ok qr"
qr off                    # hide either QR, back to clock     -> "ok qr off"
```

If the text is too long for the configured size the Pi prints
`qr: encode failed (text too long?)` and the screen is left unchanged.

### Layout of the small QR

The HDMI dashboard is laid out as: big clock (top), 8-cell colour sweep bar
(~y260-348), scrolling marquee (y600+). `qrsmall` draws in the empty band
`QR_BAND_TOP..QR_BAND_BOT` (default y360-590), centered horizontally, scaled to
fit. Nothing else draws there, so the code persists while the clock, bar, and
marquee keep updating. `qrsmall off` just clears that band.

### From the ESP32 (RemoteXY)

No GUI rebuild needed: type into the existing **Message Text** box and send. If
the first word is `qr`, `qrfull`, or `qrsmall` (case-insensitive, `:` or space
separator), the ESP32 forwards it to the Pi verbatim instead of as a marquee
message:

```
QR:https://site     ->  pi: qr https://site        (full screen)
qrsmall HELLO       ->  pi: qrsmall HELLO           (small, on dashboard)
qrfull off          ->  pi: qrfull off
```

Everything else in the Message box still goes out as a normal `msg`.

## How it renders

Black modules on a white field with a 4-module quiet zone, scaled to the
largest integer pixels-per-module that fits. Drawn once per request (static,
flicker-free). Verified end-to-end: the encoder output decodes correctly with a
standard scanner.

Internals: `qr_encode()` runs the encoder once; `qr_blit()` paints modules;
`qr_paint_full()` and `qr_paint_small()` choose geometry; `qr_band_clear()`
erases the small-QR band. Leaving full-screen mode calls `fb_clock_reset()`
(in `fb.c`) so the cached clock fully repaints instead of leaving stale dots.

## What changed

| File | Change |
|------|--------|
| `*/drivers/qrcodegen.c`, `qrcodegen.h` | Nayuki QR-Code-generator (C, MIT). `NDEBUG` forced on so `assert()` is a no-op on bare metal. |
| `RaspberryPi3_400-FreeRTOS/drivers/kstring.c` | Added `memmove` + `abs` (encoder needs them on the `-nostdlib` build; `memcpy` is already in `port.c`). |
| `*/drivers/fb.c`, `fb.h` | Added `fb_clock_reset()` to force a full clock repaint after the screen is cleared. |
| `RaspberryPi3_400-FreeRTOS/Makefile` | Added `OBJS += build/qrcodegen.o`. |
| `.../Demo/main.c` (Pi 3/400) and `.../ARM6_BCM2835/main.c` (Pi 1) | Include `qrcodegen.h`; QR state; `qr_encode`/`qr_blit`/`qr_paint_full`/`qr_paint_small`/`qr_band_clear`; `qr`/`qrfull`/`qrsmall` in parser + help; show/clear handling in `vHdmi`. |
| `ESP32S3_RaspberyPi/src/main.cpp` | Forward `qr`/`qrfull`/`qrsmall` text from the Message box as a command. |

The Pi 1 build auto-globs `drivers/*.c`, so it needs no Makefile change. Pi 1
links newlib, so it already has `memmove`/`abs`.

## Tuning (in each `main.c`)

- `QR_MAXVER` (default 10) - max QR version; higher = more capacity but larger
  static buffers (`qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAXVER)` bytes x2).
- `QR_QUIET` (default 4) - quiet-zone width in modules (4 = spec minimum).
- `QR_BAND_TOP` / `QR_BAND_BOT` - vertical band used by `qrsmall`. Adjust if you
  change the bar (`BAR_Y`/`CELL_H`) or marquee Y in `vHdmi`.
- Error-correction is `qrcodegen_Ecc_MEDIUM` in `qr_encode()`; raise to
  `_QUARTILE`/`_HIGH` for more robustness at the cost of capacity.
