# pack_test : byte-exact check of the OUT encoder against vectors captured from
#             the genuine Windows driver while it was actually playing audio.
# midi_test : the MIDI byte-stream parser, fed the exact chunking the hardware
#             produces over the MIDI loopback cable.

all: test

pack_test: pack_test.c
	$(CC) -O2 -Wall -Wextra -o $@ pack_test.c

midi_test: midi_test.c ../plugin/midi_parser_inc.h
	$(CC) -O2 -Wall -Wextra -I../plugin -o $@ midi_test.c

lifecycle_test: lifecycle_test.c ../plugin/Audio8DJ.c ../plugin/usb_engine.h
	$(CC) -O2 -Wall -Wextra -Werror -framework CoreAudio -framework CoreFoundation -o $@ lifecycle_test.c

usb_lifecycle_test: ../firmware/audio8dj_firmware.h usb_lifecycle_test.c ../plugin/usb_engine.c ../plugin/midi.c ../plugin/audio_ring.h ../plugin/midi_fifo.h ../plugin/status_file.h
	$(CC) -O2 -Wall -Wextra -Werror -framework CoreAudio -framework CoreFoundation -framework IOKit -o $@ usb_lifecycle_test.c ../plugin/midi.c

audio_ring_test: audio_ring_test.c ../plugin/audio_ring.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ audio_ring_test.c

midi_transport_test: midi_transport_test.c ../plugin/midi_fifo.h ../plugin/midi_queue.h ../plugin/status_file.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ midi_transport_test.c

test: pack_test midi_test lifecycle_test usb_lifecycle_test audio_ring_test midi_transport_test
	./pack_test
	@echo
	./midi_test
	./lifecycle_test
	./usb_lifecycle_test
	./audio_ring_test
	./midi_transport_test

clean:
	rm -f pack_test midi_test lifecycle_test usb_lifecycle_test audio_ring_test midi_transport_test

.PHONY: all test clean
