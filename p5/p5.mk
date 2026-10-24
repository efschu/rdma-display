# P5 audio (PipeWire + libsamplerate); included from the top-level Makefile
ifeq ($(shell pkg-config --exists libpipewire-0.3 samplerate && echo 1),1)
P5_CF := $(shell pkg-config --cflags libpipewire-0.3 samplerate)
P5_LD := $(shell pkg-config --libs libpipewire-0.3 samplerate) -lm -lpthread
BIN += p5/rdx_audio_tx p5/rdx_audio_rx

p5/rdx_audio_tx: p5/rdx_audio_tx.c p5/rdx_audio_pw.h p5/rdx_audio_mic.h common/rdx_audio.h $(COMMON)
	$(CC) $(CFLAGS) $(P5_CF) -o $@ p5/rdx_audio_tx.c common/rdx_common.c -libverbs $(P5_LD)

p5/rdx_audio_rx: p5/rdx_audio_rx.c p5/rdx_audio_pw.h p5/rdx_audio_mic.h common/rdx_audio.h $(COMMON)
	$(CC) $(CFLAGS) $(P5_CF) -o $@ p5/rdx_audio_rx.c common/rdx_common.c -libverbs $(P5_LD)
endif
