.org 0
.text

	.globl _freertos_vector_table

.section .vectors
.balign 2048
_vector_table:

.set VBAR, _vector_table

.org VBAR
	b	_boot
.org (VBAR + 0x80)
	b	.
.org (VBAR + 0x100)
	b	.
.org (VBAR + 0x180)
	b	SError_Handler          /* Current EL SP0 : SError */

.org (VBAR + 0x200)
	b	.
.org (VBAR + 0x280)
	b	.
.org (VBAR + 0x300)
	b	.
.org (VBAR + 0x380)
	b	SError_Handler          /* Current EL SPx : SError */

.org (VBAR + 0x400)
	b	.
.org (VBAR + 0x480)
	b	.
.org (VBAR + 0x500)
	b	.
.org (VBAR + 0x580)
	b	.

.org (VBAR + 0x600)
	b	.
.org (VBAR + 0x680)
	b	.
.org (VBAR + 0x700)
	b	.
.org (VBAR + 0x780)
	b	.

/******************************************************************************
 * Vector table to use when FreeRTOS is running.
 *****************************************************************************/
.set	FREERTOS_VBAR, (VBAR+0x0800)

.org(FREERTOS_VBAR)
_freertos_vector_table:
	b	FreeRTOS_SWI_Handler
.org (FREERTOS_VBAR + 0x80)
	b	FreeRTOS_IRQ_Handler
.org (FREERTOS_VBAR + 0x100)
	b	.
.org (FREERTOS_VBAR + 0x180)
	b	SError_Handler          /* Current EL SP0 : SError */

.org (FREERTOS_VBAR + 0x200)
	b	FreeRTOS_SWI_Handler
.org (FREERTOS_VBAR + 0x280)
	b	FreeRTOS_IRQ_Handler
.org (FREERTOS_VBAR + 0x300)
	b	.
.org (FREERTOS_VBAR + 0x380)
	b	SError_Handler          /* Current EL SPx : SError */

.org (FREERTOS_VBAR + 0x400)
	b	.
.org (FREERTOS_VBAR + 0x480)
	b	.
.org (FREERTOS_VBAR + 0x500)
	b	.
.org (FREERTOS_VBAR + 0x580)
	b	.

.org (FREERTOS_VBAR + 0x600)
	b	.
.org (FREERTOS_VBAR + 0x680)
	b	.
.org (FREERTOS_VBAR + 0x700)
	b	.
.org (FREERTOS_VBAR + 0x780)
	b	.

/******************************************************************************
 * SError handler: report syndrome via C, then ERET to recover.
 * Saves caller-saved x0-x18 + x30 (callee-saved are preserved by the C fn).
 *****************************************************************************/
.text
.balign 4
.globl SError_Handler
SError_Handler:
	sub	sp, sp, #160
	stp	x0,  x1,  [sp, #(16*0)]
	stp	x2,  x3,  [sp, #(16*1)]
	stp	x4,  x5,  [sp, #(16*2)]
	stp	x6,  x7,  [sp, #(16*3)]
	stp	x8,  x9,  [sp, #(16*4)]
	stp	x10, x11, [sp, #(16*5)]
	stp	x12, x13, [sp, #(16*6)]
	stp	x14, x15, [sp, #(16*7)]
	stp	x16, x17, [sp, #(16*8)]
	stp	x18, x30, [sp, #(16*9)]

	mrs	x0, esr_el1
	mrs	x1, far_el1
	mrs	x2, elr_el1
	bl	serror_report

	ldp	x18, x30, [sp, #(16*9)]
	ldp	x16, x17, [sp, #(16*8)]
	ldp	x14, x15, [sp, #(16*7)]
	ldp	x12, x13, [sp, #(16*6)]
	ldp	x10, x11, [sp, #(16*5)]
	ldp	x8,  x9,  [sp, #(16*4)]
	ldp	x6,  x7,  [sp, #(16*3)]
	ldp	x4,  x5,  [sp, #(16*2)]
	ldp	x2,  x3,  [sp, #(16*1)]
	ldp	x0,  x1,  [sp, #(16*0)]
	add	sp, sp, #160
	eret
