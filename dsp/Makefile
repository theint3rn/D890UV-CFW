# cfw/dsp host tests.  make test  -> BER / decode tables in ber_table.md (also on stdout)
#                      make waves -> emulator ADC sample files (../build_streams/*.adc16 and *.adc8)
CC ?= cc
C  := ..
P  := $(C)/proto
B  := $(C)/build_streams
CFLAGS := -std=gnu11 -O2 -w -DCFW_HOST $(DSPFLAGS) -I. -Itest -I$(C)/hal -I$(C)/voc -I$(C)/voc/imbe -I$(C)/app/protos \
          -I$(P)/dmr_nxdn/common -I$(P)/dmr_nxdn/dmr -I$(P)/dmr_nxdn/nxdn -I$(P)/p25/include \
          -I$(P)/dstar_ysf/common -I$(P)/dstar_ysf/dstar -I$(P)/dstar_ysf/ysf
LIBS := $(C)/app/protos/*.c $(P)/dmr_nxdn/common/*.c $(P)/dmr_nxdn/dmr/*.c $(P)/dmr_nxdn/nxdn/*.c \
        $(P)/p25/src/*.c $(P)/dstar_ysf/common/*.c $(P)/dstar_ysf/dstar/*.c $(P)/dstar_ysf/ysf/*.c

$(B)/test_dsp: dsp_demod.c dsp_demod.h test/*.c test/*.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) dsp_demod.c test/chansim.c test/test_dsp.c $(LIBS) -lm -o $@

$(B)/mkwave: dsp_demod.c test/chansim.c test/mkwave.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) dsp_demod.c test/chansim.c test/mkwave.c -lm -o $@

$(B)/dmr_call.sym:
	$(C)/tools/make_streams.sh

$(B)/test_tx: dsp_demod.c dsp_mod.c dsp_mod.h test/*.c test/*.h $(C)/voc/*.c $(C)/voc/imbe/voc_imbe.c $(C)/voc/imbe/imbe_dec.c $(C)/voc/imbe/imbe_enc.c $(C)/voc/imbe/imbe_tables.c $(C)/hal/voice.c $(C)/app/protos/*.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) dsp_demod.c dsp_mod.c test/chansim.c test/test_tx.c test/host_stubs.c $(C)/voc/*.c $(C)/voc/imbe/voc_imbe.c $(C)/voc/imbe/imbe_dec.c $(C)/voc/imbe/imbe_enc.c $(C)/voc/imbe/imbe_tables.c $(C)/hal/voice.c $(LIBS) -lm -o $@

$(B)/capdecode: dsp_demod.c test/capdecode.c test/host_stubs.c $(C)/voc/*.c $(C)/voc/imbe/voc_imbe.c $(C)/voc/imbe/imbe_dec.c $(C)/voc/imbe/imbe_enc.c $(C)/voc/imbe/imbe_tables.c $(C)/hal/voice.c $(C)/app/protos/*.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) dsp_demod.c test/capdecode.c test/host_stubs.c $(C)/voc/*.c $(C)/voc/imbe/voc_imbe.c $(C)/voc/imbe/imbe_dec.c $(C)/voc/imbe/imbe_enc.c $(C)/voc/imbe/imbe_tables.c $(C)/hal/voice.c $(LIBS) -lm -o $@

test: $(B)/capdecode $(B)/test_dsp $(B)/test_tx $(B)/dmr_call.sym
	$(B)/test_dsp $(B) | tee ber_table.md
	$(B)/test_tx | tee tx_table.md

waves: $(B)/mkwave $(B)/dmr_call.sym
	$(B)/mkwave $(B)

.PHONY: test waves
