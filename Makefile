# D890UV custom firmware (STM32U575, runs under the stock bootloader at 0x0800C000)
#   make              -> build/cfw.elf, build/cfw.bin (load at 0x0800C000), build/cfw.map
#   make LOOPBACK=1   -> modem loopback backend (proto TX symbols fed back to its RX, for testing)
#   make HW=1         -> build for a real radio (build_hw/): settings never written, digital TX compiled out,
#                        TX at Low until a power level is picked, 180 s TX hard cap, fault handlers reset with TX off
#   make HW=1 HW_DTX=1 -> ... with digital TX (TIM4_CH4 PWM on PB9 = stock MCU TX audio pin, unverified, DUMMY LOAD ONLY)
#   make HW=1 HW_NOTX=1 -> ... every TX refused ("TX inhibited"): first smoke test on a real radio
#   make HW=1 HW_SAVE=1 -> ... codeplug loader: CPS link on the programming pins PA9/PA10 + codeplug/settings flash
#                        writes (range-checked). ONLY AFTER THE OWNER OKs FLASH WRITES. PA10 is also PTT: no TX.
#   make KEEP_SWD=1   -> do not use PA14 (SWCLK) as the PF2 key input, keeps the debugger usable
#   make DEBUG=1      -> debug build in <build>_dbg/: KEEP_SWD=1 + SEGGER RTT log (hal/dbg.h, read with tools/hw/hw.py log)
#   make gdb          -> gdb-multiarch on the ELF via tools/hw/gdb/<radio>.gdb (needs a running OpenOCD or JLinkGDBServer)
#   make emu          -> build + 10 s in the Renode emulator (../emu/run.py), screen in ../emu/out/cfw/lcd.png
#   make EMU=1        -> emulator test build in build_emu/: symbol-FIFO modem backend (hal/emu_syms.c), so the
#                        decoders run on `syms Load @file rate` streams. Never flash this build.
#
# Protocol libraries: any cfw/proto/<name>/glue/*.c (or proto/<name>/cfw_*.c) is compiled in and found through
# CFW_PROTO_REGISTER(); see README.md "Plugging in a protocol".

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

BUILD   := build
ifeq ($(EMU),1)
BUILD   := build_emu
endif
ifeq ($(HW),1)
BUILD   := build_hw
endif
# one directory per variant: objects built with different flags must not mix (dependencies do not track CFLAGS)
BUILD   := $(BUILD)$(if $(filter 1,$(HW_DTX)),_dtx)$(if $(filter 1,$(HW_NOTX)),_notx)$(if $(filter 1,$(HW_SAVE)),_save)
ifeq ($(DEBUG),1)
BUILD   := $(BUILD)_dbg
KEEP_SWD := 1
endif
# BUILD_SUFFIX=_x: own build dir (build_x/...) when several people build at once
BUILD   := $(BUILD)$(BUILD_SUFFIX)
TARGET  := $(BUILD)/cfw

SRCS := startup/startup.c \
        $(wildcard drivers/*.c) \
        $(wildcard hal/*.c) \
        $(wildcard gfx/*.c) \
        $(wildcard app/*.c) \
        $(wildcard dsp/*.c) \
        $(wildcard voc/*.c) voc/imbe/voc_imbe.c voc/imbe/imbe_dec.c voc/imbe/imbe_enc.c voc/imbe/imbe_tables.c

# protocol libraries owned by the cfw/proto/* sessions. A library that ships its own proto/<name>/cfw.mk
# (PROTO_SRCS / PROTO_INC + its own glue) is used as is; otherwise its sources are compiled here and the
# platform glue in app/protos/ registers it.
-include $(wildcard proto/*/cfw.mk)
SRCS += $(wildcard app/protos/*.c)
ifeq ($(wildcard proto/dmr_nxdn/cfw.mk),)
PROTO_SRCS += $(wildcard proto/dmr_nxdn/common/*.c proto/dmr_nxdn/dmr/*.c proto/dmr_nxdn/nxdn/*.c)
PROTO_INC  += -Iproto/dmr_nxdn/common -Iproto/dmr_nxdn/dmr -Iproto/dmr_nxdn/nxdn
endif
ifeq ($(wildcard proto/p25/cfw.mk),)
PROTO_SRCS += $(wildcard proto/p25/src/*.c)
PROTO_INC  += -Iproto/p25/include
endif
ifeq ($(wildcard proto/dstar_ysf/cfw.mk),)
PROTO_SRCS += $(wildcard proto/dstar_ysf/common/*.c proto/dstar_ysf/dstar/*.c proto/dstar_ysf/ysf/*.c)
PROTO_INC  += -Iproto/dstar_ysf/common -Iproto/dstar_ysf/dstar -Iproto/dstar_ysf/ysf
endif
SRCS += $(PROTO_SRCS)

INC  := -Iinclude -Ihal -Igfx -Iapp -Idsp -Ivoc -Ivoc/imbe $(PROTO_INC)

CPU    := -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard
CFLAGS := $(CPU) -std=gnu11 -Os -g3 -Wall -Wextra -Wno-override-init -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-common -ffreestanding $(INC)
LDFLAGS := $(CPU) -T ld/stm32u575_cfw.ld -nostartfiles -Wl,--gc-sections -Wl,-Map=$(TARGET).map \
           -Wl,--print-memory-usage --specs=nano.specs --specs=nosys.specs

ifeq ($(LOOPBACK),1)
CFLAGS += -DCFW_LOOPBACK
endif
ifeq ($(EMU),1)
CFLAGS += -DCFW_EMU
endif
# MCU TX audio = TIM4_CH4 PWM on PB9 (docs/AUDIO_PATHS.md): the analog FM test tone may use it on every build;
# digital TX (4FSK/C4FM/GMSK baseband stream) is on in emulator builds, a HW=1 build has it only with HW_DTX=1
CFLAGS += -DCFW_MCU_FM_TX
ifeq ($(HW),1)
CFLAGS += -DCFW_HW_BUILD
ifeq ($(HW_SAVE),1)
# HW_SAVE=1 (pre-flash audit F10): CPS link + codeplug / settings flash writes on a real radio, still range-checked.
# ONLY AFTER THE OWNER OKs FLASH WRITES on that radio.
CFLAGS += -DCFW_PROG_HW
# D890UV: the stock programming pins are PA9 TX / PA10 RX, and PA10 is also PTT -> this build reads no PTT, no TX
CFLAGS += -DCFW_PROG_PA9 -DCFW_HW_NOTX
else
CFLAGS += -DCFW_HW_NO_SAVE
endif
ifeq ($(HW_DTX),1)
CFLAGS += -DCFW_DIGITAL_TX
endif
else
CFLAGS += -DCFW_DIGITAL_TX
endif
# HW_NOTX=1 (F7): every TX refused ("TX inhibited"), for the first smoke test on a real radio
ifeq ($(HW_NOTX),1)
CFLAGS += -DCFW_HW_NOTX
endif
# F8: version / build string (boot screen, RTT boot line, CPS link info block)
CFW_VER  := 0.6
CFW_KIND := $(if $(filter 1,$(HW)),HW,$(if $(filter 1,$(EMU)),EMU,SIM))$(if $(filter 1,$(HW_DTX)),+DTX)$(if $(filter 1,$(HW_NOTX)),+NOTX)$(if $(filter 1,$(HW_SAVE)),+SAVE)$(if $(filter 1,$(DEBUG)),+DBG)
CFW_DATE := $(shell date +%y%m%d)
CFLAGS += -DVERSION='"$(CFW_VER)"' -DCFW_BUILD='"D890 $(CFW_KIND) $(CFW_DATE)"'

ifeq ($(KEEP_SWD),1)
CFLAGS += -DCFW_KEEP_SWD
endif
ifeq ($(DEBUG),1)
CFLAGS += -DCFW_RTT -DCFW_DEBUG
endif

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
# the DSP code wants the FPU builtins (fabsf, sqrtf -> VABS / VSQRT) despite -ffreestanding
$(BUILD)/dsp/%.o: CFLAGS += -fbuiltin -fno-math-errno -fsingle-precision-constant
$(BUILD)/voc/%.o: CFLAGS += -fbuiltin -fno-math-errno -fsingle-precision-constant
$(BUILD)/hal/dsp_backend.o: CFLAGS += -fbuiltin -fno-math-errno -fsingle-precision-constant
$(BUILD)/hal/voice.o: CFLAGS += -fbuiltin -fno-math-errno -fsingle-precision-constant
DEPS := $(OBJS:.o=.d)

all: $(TARGET).elf $(TARGET).bin

$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TARGET).elf: $(OBJS) ld/stm32u575_cfw.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@ -lm -lgcc
	$(SIZE) $@
	@$(CROSS)nm $@ | grep -q " FUN_0811ece0$$" || (echo "missing delay_us alias FUN_0811ece0" && false)
	@$(CROSS)nm $@ | grep -q " FUN_080e2da4$$" || (echo "missing RF read alias FUN_080e2da4" && false)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

emu: all
	cd .. && python3 emu/run.py --elf cfw/$(TARGET).elf --seconds 10 --name cfw

GDB ?= gdb-multiarch
GDBPORT ?= 3333
gdb: $(TARGET).elf
	$(GDB) -x $(firstword $(wildcard tools/hw) ../tools/hw)/gdb/d890.gdb -ex 'target extended-remote :$(GDBPORT)' $(TARGET).elf

clean:
	rm -rf $(BUILD)

# host tests of the shared libraries + the NXDN glue loopback (no target build needed); dsp has its own
# `make -C dsp test` (slow, rewrites dsp/ber_table.md + tx_table.md)
LIBDIR ?= .
test-libs:
	$(MAKE) -C $(LIBDIR)/proto/dmr_nxdn test
	$(MAKE) -C $(LIBDIR)/proto/p25 test
	$(MAKE) -C $(LIBDIR)/proto/dstar_ysf test
	$(MAKE) -C $(LIBDIR)/voc/imbe test
	$(LIBDIR)/tools/efr_loop.sh
	$(LIBDIR)/tools/bb_allow_test.sh
	python3 $(LIBDIR)/tools/cfwcp_host_test.py
	python3 $(LIBDIR)/tools/stockcp_host_test.py
	python3 $(LIBDIR)/tools/ta_host_test.py

-include $(DEPS)
.PHONY: all clean emu gdb test-libs
