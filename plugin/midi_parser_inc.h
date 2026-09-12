// midi_parser_inc.h — MIDI byte-stream parser, included by both the driver and
// the unit test.
//
// MIDI_READ packets carry an arbitrary chunk of the device's UART byte stream:
// message boundaries are not preserved. Live on hardware, `90 3C 40` arrives
// as [90 3C] then [40], and a keyboard sends one byte per packet. So the bytes
// have to be reassembled into whole messages before CoreMIDI sees them.
//
// The includer defines TestEmit-style output as EMIT_FN(bytes, count).

#ifndef A8DJ_MIDI_PARSER_INC_H
#define A8DJ_MIDI_PARSER_INC_H

#ifndef EMIT_FN
#error "define EMIT_FN(bytes, count) before including midi_parser_inc.h"
#endif

#ifndef MIDI_SYSEX_MAX
#define MIDI_SYSEX_MAX 4096
#endif
#if MIDI_SYSEX_MAX < 2
#error "MIDI_SYSEX_MAX must be at least two bytes"
#endif

static struct {
    uint8_t status;                    // running status, 0 if none
    uint8_t msg[3];
    int     have;                      // data bytes collected so far
    int     want;                      // data bytes this status needs
    int     inSysex;
    uint8_t sysex[MIDI_SYSEX_MAX];
    int     sysexLen;
} gP;

static void ParserReset(void) { memset(&gP, 0, sizeof(gP)); }

// How many data bytes follow a status byte.
static int MidiDataBytesFor(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;          // program change, channel pressure
    case 0x80: case 0x90: case 0xA0:
    case 0xB0: case 0xE0: return 2;
    default: break;
    }
    switch (status) {
    case 0xF1: return 1;                     // quarter frame
    case 0xF2: return 2;                     // song position
    case 0xF3: return 1;                     // song select
    default:   return 0;                     // F6, F8..FF
    }
}

static void ParseBytes(const uint8_t *bytes, int count)
{
    for (int i = 0; i < count; i++) {
        uint8_t b = bytes[i];

        // Real time bytes may appear anywhere, even mid-message and inside a
        // SysEx, and never disturb running status.
        if (b >= 0xF8) { EMIT_FN(&b, 1); continue; }

        if (gP.inSysex) {
            if (b == 0xF7) {
                if (gP.sysexLen < MIDI_SYSEX_MAX) gP.sysex[gP.sysexLen++] = b;
                EMIT_FN(gP.sysex, gP.sysexLen);
                gP.inSysex = 0;
                gP.sysexLen = 0;
            } else if (b & 0x80) {
                // A status byte inside a SysEx means it was truncated: drop it
                // and reprocess this byte as a fresh status.
                gP.inSysex = 0;
                gP.sysexLen = 0;
                i--;
            } else {
                gP.sysex[gP.sysexLen++] = b;
                // CoreMIDI accepts SysEx fragments. Flush full chunks instead
                // of truncating a long message and losing its terminating F7.
                if (gP.sysexLen == MIDI_SYSEX_MAX) {
                    EMIT_FN(gP.sysex, gP.sysexLen);
                    gP.sysexLen = 0;
                }
            }
            continue;
        }

        if (b & 0x80) {
            if (b == 0xF0) {
                gP.inSysex  = 1;
                gP.sysexLen = 0;
                gP.sysex[gP.sysexLen++] = b;
                gP.status = 0;               // system common clears running status
                gP.want = gP.have = 0;
                continue;
            }
            if (b == 0xF7) {
                // All system-common statuses cancel running/partial status,
                // even an end-of-exclusive without a preceding F0.
                gP.status = 0;
                gP.want = gP.have = 0;
                continue;
            }
            gP.msg[0] = b;
            gP.have   = 0;
            gP.want   = MidiDataBytesFor(b);
            gP.status = (b < 0xF0) ? b : 0;  // only channel messages latch
            if (gP.want == 0) EMIT_FN(gP.msg, 1);
            continue;
        }

        // Data byte.
        if (gP.want == 0) {
            if (!gP.status) continue;        // nothing to attach it to
            gP.msg[0] = gP.status;           // running status resumes
            gP.want   = MidiDataBytesFor(gP.status);
            gP.have   = 0;
        }
        gP.msg[1 + gP.have] = b;
        gP.have++;
        if (gP.have >= gP.want) {
            EMIT_FN(gP.msg, 1 + gP.want);
            gP.have = 0;
            gP.want = 0;                     // next data byte revives running status
        }
    }
}

#endif
