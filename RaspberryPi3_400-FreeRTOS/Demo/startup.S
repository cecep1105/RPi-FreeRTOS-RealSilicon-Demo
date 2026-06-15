.section ".text.boot"
.globl _boot

_boot:
    // Park secondary cores; run on core 0 only.
    mrs   x1, mpidr_el1
    and   x1, x1, #3
    cbnz  x1, hang

    // Stack top (= load address 0x80000) for every EL stage; grows down.
    ldr   x1, =_boot

    // Dispatch on whatever EL the firmware/QEMU entered us at.
    mrs   x0, CurrentEL
    lsr   x0, x0, #2            // CurrentEL[3:2] = current exception level
    cmp   x0, #3
    bne   el2_setup            // entered at EL2 or EL1 -> skip the EL3 stage

    // ---- EL3: drop to EL2 ----
    mov   x2, #0x5b1           // SCR_EL3: RW=1,HCE=1,SMD=1,RES1,NS=1
    msr   scr_el3, x2
    mov   x2, #0x3c9           // SPSR_EL3: DAIF masked, M=EL2h
    msr   spsr_el3, x2
    adr   x2, el2_setup
    msr   elr_el3, x2
    eret

el2_setup:
    mrs   x0, CurrentEL
    lsr   x0, x0, #2
    cmp   x0, #1
    beq   start_el1            // already at EL1 -> skip the EL2 stage

    // ---- EL2: drop to EL1 ----
    msr   sp_el1, x1
    mov   x0, #(1 << 31)       // HCR_EL2.RW = 1 (EL1 is AArch64)
    orr   x0, x0, #(1 << 1)    // SWIO hardwired on Pi3
    msr   hcr_el2, x0
    mov   x2, #0x3c5           // SPSR_EL2: DAIF masked, M=EL1h
    msr   spsr_el2, x2
    adr   x2, start_el1
    msr   elr_el2, x2
    eret

start_el1:
    mov   sp, x1
    // Clear BSS.
    ldr   x1, =__bss_start
    ldr   w2, =__bss_size
1:  cbz   w2, 2f
    str   xzr, [x1], #8
    sub   w2, w2, #1
    cbnz  w2, 1b
2:  bl    main

hang:
    wfi
    b     hang
