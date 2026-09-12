// midi.h — FIFO transport between the HAL plug-in and a8dj-midid.
//
// MIDI cannot come from an AudioServerPlugIn on its own: MIDI on macOS is
// CoreMIDI, a separate driver model. And the USB interface can only be opened
// by one process, which is coreaudiod because that is where the audio engine
// lives. The per-user a8dj-midid agent creates the virtual CoreMIDI endpoints;
// the plug-in transports their bytes over the EP1 pipes it already owns.

#ifndef A8DJ_MIDI_H
#define A8DJ_MIDI_H

#include <stdint.h>

// Opens the FIFOs to a8dj-midid and starts the host->device pump. Safe to
// call repeatedly. MIDI simply stays unavailable on failure; audio is
// unaffected either way.
int  A8DJ_MidiInit(void);
void A8DJ_MidiTeardown(void);

// Human readable state for the status file.
const char *A8DJ_MidiStatus(void);

// Raw bytes lifted out of a MIDI_READ packet, in arrival order. The device
// hands over arbitrary chunks of the UART stream with no regard for message
// boundaries, so these go through a parser before reaching CoreMIDI.
void A8DJ_MidiFromDevice(const uint8_t *bytes, int count);

// Implemented by the USB engine: wraps bytes in a MIDI_WRITE and puts them on
// EP 0x01. Returns 0 on success.
int  A8DJ_UsbMidiSend(const uint8_t *bytes, int count);

// Implemented by the USB engine: drains bytes the EP1 completion queued up.
// That completion runs on the time-constraint streaming thread, which must not
// touch the filesystem, so it only fills a ring and the MIDI thread writes it
// to the FIFO. Returns the number of bytes copied.
int  A8DJ_UsbMidiReceive(uint8_t *buf, int cap);

#endif
