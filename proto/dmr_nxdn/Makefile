# DMR / NXDN protocol libraries (host build + tests, optional Cortex-M33 size check)
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
B       := build
LIBSRC  := $(wildcard common/*.c) $(wildcard dmr/*.c) $(wildcard nxdn/*.c)
TESTSRC := $(wildcard tests/*.c)
LIBOBJ  := $(patsubst %.c,$(B)/%.o,$(LIBSRC))
TESTOBJ := $(patsubst %.c,$(B)/%.o,$(TESTSRC))

ARM_CC  ?= arm-none-eabi-gcc
ARM_FLAGS := -std=c11 -Os -mcpu=cortex-m33 -mthumb -ffunction-sections -fdata-sections -Wall -Wextra -Werror

.PHONY: all test clean arm-size
all: $(B)/libdmrnxdn.a

$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(B)/libdmrnxdn.a: $(LIBOBJ)
	ar rcs $@ $^

$(B)/run_tests: $(TESTOBJ) $(B)/libdmrnxdn.a
	$(CC) $(CFLAGS) $(TESTOBJ) $(B)/libdmrnxdn.a -o $@

test: $(B)/run_tests
	./$(B)/run_tests

# code size on the target MCU (no libc calls other than memset/memcpy/memcmp)
arm-size:
	@mkdir -p $(B)/arm
	@for f in $(LIBSRC); do $(ARM_CC) $(ARM_FLAGS) -c $$f -o $(B)/arm/$$(basename $$f .c).o || exit 1; done
	arm-none-eabi-size -t $(B)/arm/*.o

clean:
	rm -rf $(B)

-include $(LIBOBJ:.o=.d) $(TESTOBJ:.o=.d)
