# Host build + tests for the D-STAR / YSF protocol libraries.
#   make test     build and run all host tests
#   make lib      build libdstar_ysf.a (host)
#   make CC=arm-none-eabi-gcc CFLAGS_EXTRA="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard" lib
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror
CFLAGS  += $(CFLAGS_EXTRA)
BUILD   ?= build

SRCS := common/dv_common.c \
        dstar/dstar_header.c dstar/dstar_rx.c dstar/dstar_tx.c dstar/dstar_gmsk.c \
        ysf/ysf_fec.c ysf/ysf_frame.c ysf/ysf_rx.c ysf/ysf_tx.c ysf/ysf_c4fm.c
OBJS := $(SRCS:%.c=$(BUILD)/%.o)
TESTS := test/test_main.c test/test_common.c test/test_dstar.c test/test_ysf.c

.PHONY: all lib test clean
all: lib

lib: $(BUILD)/libdstar_ysf.a

$(BUILD)/libdstar_ysf.a: $(OBJS)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c $(wildcard */*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_runner: $(TESTS) $(BUILD)/libdstar_ysf.a test/test.h
	$(CC) $(CFLAGS) $(TESTS) $(BUILD)/libdstar_ysf.a -lm -o $@

test: $(BUILD)/test_runner
	./$(BUILD)/test_runner

clean:
	rm -rf $(BUILD)
