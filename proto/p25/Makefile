# P25 protocol library - host build and tests
#   make test        build + run host tests (gcc)
#   make lib         host static library build/libp25.a
#   make arm         cross-compile check for Cortex-M33 (arm-none-eabi-gcc) + size
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS  += -Iinclude
SRC     := $(wildcard src/*.c)
TSRC    := $(wildcard tests/*.c)
B       := build

ARMCC   ?= arm-none-eabi-gcc
ARMFLAGS = -std=c11 -Os -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
           -ffunction-sections -fdata-sections -Wall -Wextra -Iinclude

.PHONY: all test lib arm clean
all: test

$(B)/host/%.o: src/%.c $(wildcard include/*.h) | $(B)/host
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/tests/%.o: tests/%.c $(wildcard include/*.h) tests/tst.h | $(B)/tests
	$(CC) $(CFLAGS) -Wno-conversion -c $< -o $@
$(B)/host $(B)/tests $(B)/arm:
	mkdir -p $@

lib: $(B)/libp25.a
$(B)/libp25.a: $(SRC:src/%.c=$(B)/host/%.o)
	ar rcs $@ $^

$(B)/p25_tests: $(TSRC:tests/%.c=$(B)/tests/%.o) $(B)/libp25.a
	$(CC) $(CFLAGS) $^ -lm -o $@

test: $(B)/p25_tests
	./$(B)/p25_tests

$(B)/arm/%.o: src/%.c $(wildcard include/*.h) | $(B)/arm
	$(ARMCC) $(ARMFLAGS) -c $< -o $@
arm: $(SRC:src/%.c=$(B)/arm/%.o)
	arm-none-eabi-size -t $^

clean:
	rm -rf $(B)
