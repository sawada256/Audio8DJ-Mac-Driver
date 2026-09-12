// Exercises the MIDI byte-stream parser with the exact chunking the hardware
// produces. MIDI_READ hands over arbitrary pieces of the UART stream, so a
// three byte message routinely arrives as [90 3c] then [40]; that split is
// reproduced verbatim below from a live `a8dj midi` run.
//
// The parser lives in plugin/midi.c. CoreMIDI is stubbed out so this can run
// as a plain command line test.

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

// ---- stub standing in for CoreMIDI -------------------------------------

static uint8_t gEmitted[16384];
static int     gEmittedLen;
static int     gEmitCount;

static void TestEmit(const uint8_t *b, int n)
{
    assert(n > 0 && n <= 4096);
    gEmitCount++;
    for (int i = 0; i < n && gEmittedLen < (int)sizeof(gEmitted); i++)
        gEmitted[gEmittedLen++] = b[i];
}

#define EMIT_FN TestEmit
#include "midi_parser_inc.h"

// ---- test vectors -------------------------------------------------------

struct Chunk { int len; uint8_t b[8]; };

// Captured live over the MIDI loopback cable (a8dj midi). Both the split and
// the byte values are exactly what the device delivered.
static const struct Chunk kLoopback[] = {
    { 2, { 0x90, 0x3C } }, { 1, { 0x40 } },
    { 2, { 0x80, 0x3C } }, { 1, { 0x00 } },
    { 2, { 0x9F, 0x3C } }, { 1, { 0x40 } },
    { 2, { 0xB0, 0x07 } }, { 1, { 0x64 } },
    { 2, { 0xE0, 0x00 } }, { 1, { 0x40 } },
    { 1, { 0xC0 } },       { 1, { 0x05 } },
    { 1, { 0xF8 } },
    { 5, { 0xF0, 0x7E, 0x00, 0x06, 0x01 } }, { 1, { 0xF7 } },
};
static const uint8_t kLoopbackExpect[] = {
    0x90, 0x3C, 0x40, 0x80, 0x3C, 0x00, 0x9F, 0x3C, 0x40,
    0xB0, 0x07, 0x64, 0xE0, 0x00, 0x40, 0xC0, 0x05, 0xF8,
    0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7,
};

// A keyboard delivers one byte per packet; every one of the 2314 packets in
// the Windows capture had len=1.
static const uint8_t kKeyboard[] = { 0x90, 0x4F, 0x57, 0x90, 0x4C, 0x35 };

// Running status: the device passes the UART stream through untouched, so the
// parser has to expand it itself.
static const uint8_t kRunning[] = { 0x90, 0x3C, 0x40, 0x3E, 0x40, 0x40, 0x40 };
static const uint8_t kRunningExpect[] = {
    0x90, 0x3C, 0x40, 0x90, 0x3E, 0x40, 0x90, 0x40, 0x40,
};

// A real time byte landing in the middle of a message must not disturb it.
static const uint8_t kInterleaved[] = { 0x90, 0x3C, 0xF8, 0x40 };
static const uint8_t kInterleavedExpect[] = { 0xF8, 0x90, 0x3C, 0x40 };

static int Check(const char *name, const uint8_t *want, int wantLen)
{
    int ok = (gEmittedLen == wantLen) && !memcmp(gEmitted, want, (size_t)wantLen);
    printf("%-28s : %s", name, ok ? "一致" : "不一致");
    if (!ok) {
        printf("\n  期待:");
        for (int i = 0; i < wantLen; i++) printf(" %02X", want[i]);
        printf("\n  実際:");
        for (int i = 0; i < gEmittedLen; i++) printf(" %02X", gEmitted[i]);
    }
    printf("\n");
    return ok;
}

static void Reset(void) { gEmittedLen = 0; gEmitCount = 0; ParserReset(); }

int main(void)
{
    int ok = 1;

    Reset();
    for (size_t i = 0; i < sizeof(kLoopback)/sizeof(kLoopback[0]); i++)
        ParseBytes(kLoopback[i].b, kLoopback[i].len);
    ok &= Check("実機ループバック(分割あり)", kLoopbackExpect, (int)sizeof(kLoopbackExpect));

    Reset();
    for (size_t i = 0; i < sizeof(kKeyboard); i++) ParseBytes(&kKeyboard[i], 1);
    ok &= Check("1バイトずつ(キーボード)", kKeyboard, (int)sizeof(kKeyboard));

    Reset();
    ParseBytes(kRunning, (int)sizeof(kRunning));
    ok &= Check("ランニングステータス展開", kRunningExpect, (int)sizeof(kRunningExpect));

    Reset();
    ParseBytes(kInterleaved, (int)sizeof(kInterleaved));
    ok &= Check("リアルタイム割り込み", kInterleavedExpect, (int)sizeof(kInterleavedExpect));

    Reset();
    const uint8_t strayEnd[] = {0x90, 60, 0xf7, 100, 61, 100};
    ParseBytes(strayEnd, sizeof(strayEnd));
    ok &= Check("F7で部分メッセージを破棄", strayEnd, 0);
    Reset();
    uint8_t sysex[MIDI_SYSEX_MAX * 2 + 17];
    memset(sysex, 0x23, sizeof(sysex));
    sysex[0] = 0xf0; sysex[sizeof(sysex) - 1] = 0xf7;
    for (size_t i = 0; i < sizeof(sysex); i++) ParseBytes(&sysex[i], 1);
    ok &= Check("長いSysExを欠落なく分割", sysex, sizeof(sysex));
    ok &= gEmitCount == 3;

    Reset();
    uint32_t random = 0x8d1;
    for (int i = 0; i < 1000000; i++) {
        random = random * 1664525u + 1013904223u;
        uint8_t byte = (uint8_t)(random >> 24);
        ParseBytes(&byte, 1);
    }
    printf("ランダム100万バイトの境界検査: 完了\n");
    printf("\n=> MIDI パーサは %s\n", ok ? "全ケース一致" : "不一致あり");
    return ok ? 0 : 1;
}
