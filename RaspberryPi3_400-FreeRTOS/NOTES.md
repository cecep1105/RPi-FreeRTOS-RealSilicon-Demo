# Pi 3 (AArch64) FreeRTOS base - verified tick

Base: eggman/FreeRTOS-raspi3 (FreeRTOS V10.0.1 + ARM_CA53_64_RaspberryPi3 port).
Status: **two-task tick test passes in QEMU** - generic timer at a clean 1000 Hz,
scheduler + context switch + tick ISR all confirmed. Same proof step we did on Pi 1.

Covers BOTH Pi 2 v1.2 and Pi 3 (same Cortex-A53 / BCM2837, base 0x3F000000).

## Fixes applied to the upstream base (it was QEMU-6.1-only, untested on hardware)

1. **Boot EL dispatch (Demo/startup.S)** - THE important one.
   Upstream startup begins with `msr scr_el3,...`, assuming entry at EL3.
   QEMU 8.2 *and real Pi 3 firmware* enter kernel8.img at **EL2**, where that
   instruction is illegal -> it faulted on the first instruction and hung silently.
   Now it reads `CurrentEL` and runs the EL3 stage only if actually at EL3,
   else enters at EL2 (or EL1). This is required for real hardware.

2. **Link flags (Makefile)** - dropped `-fpic`, added `-no-pie`.
   A fixed-address bare-metal image can't take the ABS32 relocation against
   `vApplicationIRQHandler` under PIC (also tripped a binutils ld bug).

3. **UART -> PL011 (Demo/uart.c)** - upstream used the mini-UART, whose QEMU model
   is incomplete (drops TX, "AUX_MU_BAUD_REG unsupported") and which also behaves
   differently on real hardware. Switched to the PL011 (UART0 @ 0x3F201000),
   which QEMU emulates faithfully AND matches your real-hardware setup
   (IBRD=26/FBRD=3 for 115200 @ 48 MHz UARTCLK). Polling TX/RX, no IRQ needed.

4. **Makefile**: objdump-listing line made non-fatal (an Ubuntu binutils 2.42
   disassembler bug; your aarch64-none-elf objdump is fine). `make run` serial
   order set to stdio-first (PL011 = QEMU serial0), machine name -> raspi3b.

## Build

    make CROSS=aarch64-none-elf          # your MSYS2/UCRT64 toolchain
    # QEMU smoke test (if installed):
    make run CROSS=aarch64-none-elf      # expect hello world + counter +0x1F4/print

## Run on real Pi 3 / Pi 2 v1.2

    aarch64-none-elf-objcopy kernel8.elf -O binary kernel8.img

SD card config.txt:

    arm_64bit=1
    enable_uart=1
    dtoverlay=disable-bt        # frees PL011 onto GPIO14/15 (else it's on Bluetooth)
    init_uart_clock=48000000    # makes IBRD=26/FBRD=3 land on 115200

SD card also needs the usual firmware: start.elf, fixup.dat,
bcm2710-rpi-3-b.dtb, plus kernel8.img.

## Tick details (for when you port the rest)

- Tick = ARM **virtual** generic timer (CNTV_*_EL0), freq read from CNTFRQ_EL0
  (19.2 MHz on real Pi 3, auto-handled - do not hardcode).
- IRQ routed via BCM2837 **local controller**: write (1<<3) to 0x40000040
  (Core0 timer IRQ control); core IRQ source read from 0x40000060.
- Dispatch in vApplicationIRQHandler (Demo/FreeRTOS_tick_config.c):
  bit3 = generic timer -> FreeRTOS_Tick_Handler; bit8 = GPU peripherals -> irq_handler.
- This local-controller path is the same on QEMU and real Pi 2/3.
  Pi 400 differs here: GIC-400 instead of the local controller.

## Next steps

1. Bring your drivers into this tree (PERIPHERAL_BASE = 0x3F000000):
   gpio / uart / spi / tm1637 / max7219 / timer / font / display port directly.
2. Switch heap_1.c -> heap_4.c (your app creates several tasks/queues and the
   HDMI task does vTaskDelete; heap_1 cannot free).
3. **MMU**: add identity-map MMU + caches BEFORE bringing fb.c (HDMI). Uncached
   framebuffer is unusably slow on a 1.2 GHz A53, and fb.c's "caches off" assumption
   breaks. Map RAM = Normal cacheable, peripherals (0x3F00_0000 + 0x4000_0000) =
   Device-nGnRnE; mark the framebuffer non-cacheable/write-combined. (TImada Pi4
   port is the MMU reference.)
4. Drop in your milestone-5 main.c tasks.
5. Pi 400: same tree, base 0xFE000000, swap local-controller tick routing for GIC-400.

---

# Milestone: drivers ported (gpio / uart / spi / tm1637 / max7219 / timer)

Status: **all six drivers compile, link, init, and run under FreeRTOS on the
Pi 3 AArch64 tree** - verified in QEMU (1000 Hz tick, sys_us live, PL011 output,
4 tasks scheduled: heartbeat / sweep / tm1637 / matrix).

Drivers live in `drivers/`, built with `-DPERIPHERAL_BASE=0x3F000000UL`.
Demo/main.c is now a driver smoke test (one task per peripheral).

## Changes made bringing the drivers over

1. **`-mgeneral-regs-only` (CFLAGS) - the critical one.**
   At -O2, gcc auto-vectorises byte-array init (e.g. `uint8_t buf[8]`, `fb[32]`)
   into NEON/SIMD. The FPU/SIMD unit is OFF out of reset and nothing enabled it,
   so the first SIMD op took an Undefined-Instruction trap - and because it
   happened before the scheduler installs the FreeRTOS vector table (VBAR still 0),
   it spun in unmapped memory. This project is all-integer, so forbidding FP/SIMD
   is the clean fix (also means no FP context to save across task switches).
   If you ever add genuine floating point, instead enable CPACR_EL1.FPEN in
   startup.S AND use an FP-saving port.

2. **`drivers/timer.c`: added `delay_us()`** - microsecond busy-wait off the
   1 MHz system timer (`sys_us()`), clock-independent. The old `delay()` is a
   NOP loop calibrated for a 700 MHz ARM11; at 1.2 GHz it is far too short.
   tm1637 bit-bang and the GPIO pull settle now use `delay_us()` so timing is
   correct regardless of core clock. (`delay_us` also has a fallback iteration
   cap so a dead system timer can't hang it.)

3. **`drivers/tm1637.c`**: `tm_delay()` -> `delay_us(TM_US)` (TM_US default 5).
4. **`drivers/gpio.c`**: legacy pull `delay(150)` -> `delay_us(5)`.
   (Pi 3 uses the legacy GPPUD path; the BCM2711/Pi400 reversed-pull path is the
   other branch, selected automatically by PERIPHERAL_BASE.)
5. **`drivers/spi.c`**: bounded the three busy-waits (TXD / RXD-drain / DONE).
   Guards are sized for real-hardware completion (TXD/RXD assert ~immediately for
   <=16-byte writes to the write-only MAX7219; DONE covers the transfer time) and
   are a no-op there - they only stop a non-responding SPI (e.g. QEMU's stub) from
   wedging a task.

## Driver notes for real hardware

- UART = PL011 on GPIO14/15 (needs `dtoverlay=disable-bt`, `init_uart_clock=48000000`).
- SPI0 SCLK = core_clk/256 (~1-1.5 MHz on Pi 3) - well within MAX7219's 10 MHz.
- TM1637 on GPIO2/3 (bit-banged, open-drain via INPUT/OUTPUT toggle + pull-ups).
- Sweep pins {4,17,18,27,22,23,24,25}, all on the 40-pin header.
- QEMU can't exercise SPI/MAX7219 or TM1637 (device models are stubs); those are
  validated on real silicon. UART + tick + scheduler + GPIO are fully exercised.

## Next

- heap_1 -> heap_4 before the real main.c (vTaskDelete + many tasks/queues).
- MMU + caches before fb.c (HDMI) - see earlier note.
- Then drop in your milestone-5 task set (UART command parser, clock owner, etc.).

---

# Milestone: real-hardware boot fixed (rebased on your proven layout)

Symptom: built kernel8.img booted fine in QEMU but did NOTHING on a real Pi 3
(no LED, no UART), while your own pre-FreeRTOS `make pi3-64` kernel worked on the
same card + config.txt. Root cause was the eggman **linker layout**, not our code:

- eggman's `linker` put `.vectors` FIRST, so `_boot` landed at 0x80f88 and the
  startup set SP to that address: only 8-byte aligned (AArch64 needs 16) and
  sitting *inside* the vector table. QEMU tolerates it; real silicon faults
  before the first instruction of real startup runs.

Fix: reorder the linker so `.text.boot` is FIRST (matches your `linker64.ld`):

    .text : { KEEP(*(.text.boot)) KEEP(*(.vectors)) *(.text .text.*) }

Now `_boot` is at 0x80000 (firmware jumps straight to real startup) and SP =
0x80000 — 16-byte aligned, growing down *below* the image, exactly like your
working bare-metal kernel. The raw image now starts with `mrs x1, mpidr_el1`
(real startup), not a branch into the vector table.

Notes:
- The upstream startup already drops EL3->EL2->EL1 (the FreeRTOS Cortex-A53 port
  runs at EL1) and clears BSS with full 64-bit stores. We kept it as-is; a
  hand-rewrite of it regressed the timer, so the proven startup stays.
- Runs MMU/caches OFF at EL1, same as your bare-metal kernel runs MMU-off at EL2.
- config.txt unchanged: arm_64bit=1, enable_uart=1, dtoverlay=disable-bt,
  init_uart_clock=48000000.

## Flash order
1. kernel8_DIAG.img  -> rename kernel8.img : blinks the 8 sweep LEDs 12x then
   spams "DIAG ... PL011 UART works". Confirms boot + UART on real hardware.
2. kernel8_DRIVERS.img -> rename kernel8.img : the full driver smoke test
   (heartbeat over UART at 1000 Hz tick + sweep + tm1637 + matrix).

Both are raw binaries (~28 KB). Build your own with:
    make CROSS=aarch64-none-elf
    aarch64-none-elf-objcopy -O binary kernel8.elf kernel8.img
