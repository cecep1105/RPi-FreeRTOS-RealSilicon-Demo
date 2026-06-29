# RPi Bare-Metal + FreeRTOS + Networking — with an ESP32-S3 Instrument Bridge

A from-scratch, hardware-confirmed embedded platform running the **same application** across
three Raspberry Pi SoC generations — **no Linux, no vendor BSP, no pre-built driver stack**.
Boot, FreeRTOS scheduler, USB host, Ethernet MAC, TCP/IP glue, HDMI, and peripherals were
each brought up on real silicon and verified on the wire and on the screen. A companion
**ESP32-S3** turns the rig into a networked **logic analyzer and oscilloscope**.

> Companion repo (ESP32-S3): `github.com/cecep1105/ESP32_FreeRTOS_Demo`
> Full technical writeup: **[PROJECT_SUMMARY.md](PROJECT_SUMMARY.md)**

---

## Targets

| Target | SoC | Core / ISA | Ethernet | Key constraint |
|---|---|---|---|---|
| **Pi 1 (B / B+)** | BCM2835 | ARM1176 / ARMv6 (32-bit) | USB (SMSC9512) | No MMU, no HW unaligned access |
| **Pi 3** | BCM2837 | Cortex-A53 / AArch64 | USB (LAN9514) | DMA via uncached alias |
| **Pi 400** | BCM2711 | Cortex-A72 / AArch64 | **Native GENET** | Pi 4 family, on-die gigabit MAC |
| **ESP32-S3** | ESP32-S3 | Xtensa LX7 | Wi-Fi / BLE | Arduino companion, USB host |

The two AArch64 boards share one codebase; the ARMv6 board shares the **same driver sources**
behind a few `#if defined(__aarch64__)` guards (DMA coherence, memory barriers, unaligned
access, stack sizes).

---

## Build

**Prerequisites:** `arm-none-eabi-gcc` (Pi 1), an `aarch64` bare-metal GCC (Pi 3/400),
and PlatformIO (ESP32-S3).

```bash
# Pi 1 (ARMv6) — auto-globs drivers/*.c
cd RaspberryPi1-FreeRTOS/FreeRTOS/Demo/ARM6_BCM2835
make clean && make          # CFLAGS: -march=armv6z -mno-unaligned-access (Pi 1-only)

# Pi 3 / Pi 400 (AArch64)
cd RaspberryPi3_400-FreeRTOS
make clean && make          # produces kernel8.img

# ESP32-S3 companion
cd ESP32_FreeRTOS_Demo
pio run -t upload           # esp32-s3-devkitc-1
```

Copy the resulting kernel image to the SD card (with `bootcode.bin`/`start.elf` firmware).

> **Pi 1 note:** the unaligned-access fix is two parts and **both are required** — the
> `-mno-unaligned-access` flag (stops GCC emitting inlined unaligned loads) and
> `drivers/aligned_mem.c` (an alignment-safe `memcpy`/`memmove` overriding newlib's). Do not
> add either to the AArch64 builds.

---

## Features

**Networking** — Mongoose TCP/IP (integrated **unmodified**, identical on all three boards):
DHCP lease with static-IP fallback, NTP sync, HTTP dashboard behind HTTP Basic Auth, and a
WebSocket client that relays pushed events to the marquee.

**USB Ethernet from scratch** (Pi 1 / Pi 3) — DWC2 host controller, LAN9514/9512 hub, and
SMSC9512 NIC, written from the register level and wired into Mongoose. Pi 400 uses native
GENET.

**Display & UI** — HDMI framebuffer clock, scrolling marquee (MAX7219 + HDMI), on-screen QR
codes, and HDMI **logic-analyzer / oscilloscope** views fed by the ESP32. A UART command
console (`krclock>`): `set`, `time`, `msg`, `qr`, `la`/`sc`, LED sweep, servo, and more.

**ESP32-S3 instrument bridge** — USB-host capture of a **BitScope BS05U** logic analyzer via
the BitScope VM protocol, an ADC oscilloscope path, a `siggen` self-test generator, a
synthetic source for path validation, RemoteXY (BLE) mobile control, and the WebSocket relay.
Streams `la …` / `sc …` / `msg …` lines to the Pi over UART.

---

## The interesting bug (short version)

DHCP refused to lease on Pi 1 while everything else (NTP, ARP, HTTP) worked. Root cause: the
DHCP transaction ID was built with `memcpy(&xid, mac+2, 4)` from an unaligned source, and
newlib's ARMv6 `memcpy` (plus GCC's inlined copies) assumed hardware unaligned access the
Pi 1 doesn't have with the MMU off — so the word load **rotated** (`EB91 10CD` → `EB91 B827`)
and every OFFER was dropped on the xid check. Mongoose survived everywhere else because it
reads headers byte-wise. Fixed with `aligned_mem.c` + `-mno-unaligned-access`, Mongoose left
stock. Full trail in [PROJECT_SUMMARY.md](PROJECT_SUMMARY.md) §3.2.

---

## Status

Confirmed on real hardware across Pi 1 / 3 / 400: USB/native Ethernet up, **DHCP leasing on
every board**, NTP, HTTP dashboard + Basic Auth, WebSocket relay, HDMI clock/marquee/QR, and
the ESP32-fed instrument views. Shared driver synced across all three targets from a single
design.

---

## Layout

```
RaspberryPi1-FreeRTOS/   FreeRTOS/Demo/ARM6_BCM2835/  — Pi 1 (ARMv6) build + drivers/
RaspberryPi3_400-FreeRTOS/  Demo/ + drivers/          — Pi 3 / 400 (AArch64) build
PROJECT_SUMMARY.md                                    — full technical writeup
```
