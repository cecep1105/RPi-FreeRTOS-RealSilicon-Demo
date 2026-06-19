BOARD ?= pi3
ifeq ($(BOARD),pi400)
  PBASE := 0xFE000000UL
else
  PBASE := 0x3F000000UL
endif
CROSS ?= aarch64-none-elf
CFLAGS =  -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align -fno-pic -ffreestanding -std=gnu99 -O2 -Wall -Wextra -I$(INCLUDEPATH1) -I$(INCLUDEPATH2) -I$(INCLUDEPATH3) -Idrivers -DPERIPHERAL_BASE=$(PBASE)

# LED-sweep output backend:
#   make               -> 74HC595 shift register, 3 GPIOs (DATA=17, CLOCK=27, LATCH=22)  [default]
#   make LEDSWEEP=FULL  -> 8 direct GPIOs (original wiring)
# Switch modes with a clean rebuild:  make clean && make LEDSWEEP=FULL
LEDSWEEP ?= 595
ifeq ($(LEDSWEEP),FULL)
  CFLAGS += -DLEDSWEEP_FULL
endif

ASMFLAGS = -mcpu=cortex-a53

BUILDPATH = build
INCLUDEPATH1 ?= FreeRTOS/Source/include
INCLUDEPATH2 ?= FreeRTOS/Source/portable/GCC/ARM_CA53_64_RaspberryPi3
INCLUDEPATH3 ?= Demo

OBJS = build/startup.o 
OBJS +=build/FreeRTOS_asm_vector.o
OBJS +=build/FreeRTOS_tick_config.o
OBJS +=build/main.o

# ---- application drivers (drivers/) ----
OBJS +=build/timer.o
OBJS +=build/gpio.o
OBJS +=build/uart.o
OBJS +=build/spi.o
OBJS +=build/tm1637.o
OBJS +=build/max7219.o
OBJS +=build/fb.o
OBJS +=build/qrcodegen.o
OBJS +=build/font.o
OBJS +=build/font8x8.o
OBJS +=build/irqctrl.o
OBJS +=build/pwm.o
OBJS +=build/stepper.o
OBJS +=build/kstring.o

OBJS +=build/port.o
OBJS +=build/portASM.o

OBJS +=build/list.o
OBJS +=build/tasks.o
OBJS +=build/queue.o
OBJS +=build/timers.o

OBJS +=build/heap_4.o
OBJS += build/la_render.o
OBJS += build/sc_render.o

.PHONY: all clean run runasm

all : kernel8.img

kernel8.elf : raspberrypi3.ld $(OBJS)
	$(CROSS)-gcc -no-pie -Wl,--build-id=none -std=gnu11 -T raspberrypi3.ld -o $@ -ffreestanding -O2 -nostdlib $(OBJS)
	-$(CROSS)-objdump -D kernel8.elf > kernel8.list 2>/dev/null || true

kernel8.img : kernel8.elf
	$(CROSS)-objcopy $< -O binary $@
	@echo " built $@  (BOARD=$(BOARD))"

build/%.o : Demo/%.S $(BUILDPATH)
	$(CROSS)-as $(ASMFLAGS) -c -o $@ $<
	
build/%.o : Demo/%.c $(BUILDPATH)
	$(CROSS)-gcc $(CFLAGS)  -c -o $@ $<

build/%.o : drivers/%.c $(BUILDPATH)
	$(CROSS)-gcc $(CFLAGS)  -c -o $@ $<

build/%.o : FreeRTOS/Source/%.c
	$(CROSS)-gcc $(CFLAGS)  -c -o $@ $<

build/%.o : FreeRTOS/Source/portable/GCC/ARM_CA53_64_RaspberryPi3/%.c
	$(CROSS)-gcc $(CFLAGS)  -c -o $@ $<

build/%.o : FreeRTOS/Source/portable/GCC/ARM_CA53_64_RaspberryPi3/%.S
	$(CROSS)-as $(ASMFLAGS) -c -o $@ $<

build/%.o : FreeRTOS/Source/portable/MemMang/%.c
	$(CROSS)-gcc $(CFLAGS)  -c -o $@ $<

$(BUILDPATH):
	@if [ ! -d $(BUILDPATH) ]; then \
	  echo " mkdir $(BUILDPATH)"; mkdir $(BUILDPATH); \
	fi

clean :
	rm -f build/*.o
	rm -f *.elf
	rm -f *.list
	rm -f *.img

run :
	$(MAKE) kernel8.elf
	qemu-system-aarch64 -M raspi3b -m 1024 -serial mon:stdio -serial null -nographic -kernel kernel8.elf

runasm :
	$(MAKE) kernel8.elf
	qemu-system-aarch64 -M raspi3b -m 1024 -serial mon:stdio -serial null -nographic -kernel kernel8.elf -d in_asm


