# IMBE 7200x4400 vocoder - host tests and Cortex-M33 size check
#   make test   host tests (gcc), writes build/speech_{in,out}.raw (8 kHz s16le)
#   make arm    cross-compile + size
CC     ?= gcc
P25    := ../../proto/p25
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wdouble-promotion -Wno-unused-parameter
CFLAGS += -I. -I$(P25)/include
SRC    := imbe_dec.c imbe_enc.c imbe_tables.c
B      := build
ARMCC  ?= arm-none-eabi-gcc
ARMFLAGS = -std=c11 -O2 -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -ffunction-sections \
           -fdata-sections -fsingle-precision-constant -fno-math-errno -Wall -Wextra -Wdouble-promotion -I.

.PHONY: test arm clean
test: $(B)/imbe_tests
	./$(B)/imbe_tests

$(B):
	mkdir -p $@
$(B)/imbe_tests: $(SRC) tests/test_imbe.c imbe.h imbe_tables.h | $(B)
	$(CC) $(CFLAGS) -Wno-double-promotion $(SRC) tests/test_imbe.c $(P25)/src/p25_p1.c $(P25)/src/p25_fec.c \
	    $(P25)/src/p25_crypt.c -lm -o $@

arm: | $(B)
	for f in $(SRC); do $(ARMCC) $(ARMFLAGS) -c $$f -o $(B)/$${f%.c}.arm.o || exit 1; done
	arm-none-eabi-size -t $(B)/*.arm.o

clean:
	rm -rf $(B)
