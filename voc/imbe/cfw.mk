# IMBE vocoder for the cfw firmware build (include from cfw/Makefile, paths relative to cfw/)
IMBE_SRCS := voc/imbe/imbe_dec.c voc/imbe/imbe_enc.c voc/imbe/imbe_tables.c
IMBE_INC  := -Ivoc/imbe
# optional cfw backend wrapper (single static instance, cfw_vocoder_ops_t g_voc_imbe): voc/imbe/voc_imbe.c
