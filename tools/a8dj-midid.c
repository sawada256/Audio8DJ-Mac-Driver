// Per-user CoreMIDI agent. The HAL plug-in owns USB; FIFOs carry MIDI bytes.

#include <CoreMIDI/CoreMIDI.h>
#include <stdatomic.h>
#include "midi_fifo.h"
#include "midi_queue.h"
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIFO_TO_HOST "/tmp/a8dj-midi-tohost"
#define FIFO_TO_DEV  "/tmp/a8dj-midi-todev"

static MIDIClientRef   gClient;
static MIDIEndpointRef gSource;      // device -> host
static MIDIEndpointRef gDest;        // host -> device
static int             gToHostFd = -1;
static int             gToDevFd  = -1;
static int             gVerbose;
static unsigned long gInBytes;
static _Atomic unsigned long gOutBytes;
static _Atomic unsigned long gDroppedPackets;
static A8DJ_MidiQueue gOutputQueue = { .mutex = PTHREAD_MUTEX_INITIALIZER };
// Host time at which the bytes being parsed were read from the FIFO.
static MIDITimeStamp gStamp;
static int gWake[2] = {-1, -1};

// ---------------------------------------------------------------- parser
static void Emit(const uint8_t *bytes, int n)
{
    if (!gSource || n <= 0) return;
    union { max_align_t alignment; Byte bytes[4096 + 64]; } storage;
    MIDIPacketList *list = (MIDIPacketList *)storage.bytes;
    MIDIPacket *pkt = MIDIPacketListInit(list);
    // A real host time, never 0. MIDISend treats 0 as "now", but a source's
    // MIDIReceived passes the value through to every client untouched, and
    // DAWs place a recorded event by it: Cubase put every note of a take at
    // the same instant (the start), and notes still held at stop became long
    // notes up to the stop point.
    pkt = MIDIPacketListAdd(list, sizeof(storage), pkt, gStamp ? gStamp : mach_absolute_time(),
                            (ByteCount)n, bytes);
    if (pkt) MIDIReceived(gSource, list);
    if (gVerbose) {
        printf("  -> host :");
        for (int i = 0; i < n && i < 16; i++) printf(" %02X", bytes[i]);
        printf("%s\n", n > 16 ? " ..." : "");
        fflush(stdout);
    }
}

#define EMIT_FN Emit
#include "midi_parser_inc.h"

// ------------------------------------------------------------ CoreMIDI out
static void DestReadProc(const MIDIPacketList *pktlist, void *a, void *b)
{
    (void)a; (void)b;
    if (!pktlist) return;
    const MIDIPacket *pkt = &pktlist->packet[0];
    for (unsigned i = 0; i < pktlist->numPackets; i++) {
        if (gToDevFd >= 0 && pkt->length) {
            if (A8DJ_MidiQueuePush(&gOutputQueue, pkt->data, pkt->length)) {
                uint8_t wake = 1;
                // A full wake pipe is already readable; no additional wake is needed.
                (void)write(gWake[1], &wake, 1);
            } else gDroppedPackets++;
            if (gVerbose) {
                printf("  <- host :");
                for (unsigned k = 0; k < pkt->length && k < 16; k++)
                    printf(" %02X", pkt->data[k]);
                printf("%s\n", pkt->length > 16 ? " ..." : "");
                fflush(stdout);
            }
        }
        pkt = MIDIPacketNext(pkt);
    }
}

// ---------------------------------------------------------------- plumbing
static int OpenFifo(const char *path)
{
    int fd = A8DJ_OpenMidiFifo(path);
    if (fd < 0) fprintf(stderr, "open FIFO %s: %s\n", path, strerror(errno));
    return fd;
}

static volatile sig_atomic_t gStop;
static void OnSignal(int s) { (void)s; gStop = 1; }

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v")) gVerbose = 1;

    signal(SIGINT,  OnSignal);
    signal(SIGTERM, OnSignal);
    signal(SIGPIPE, SIG_IGN);

    gToHostFd = OpenFifo(FIFO_TO_HOST);
    gToDevFd  = OpenFifo(FIFO_TO_DEV);
    if (gToHostFd < 0 || gToDevFd < 0) return 1;

    if (pipe(gWake) != 0) { perror("MIDI wake pipe"); return 1; }
    for (int i = 0; i < 2; i++) {
        if (fcntl(gWake[i], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(gWake[i], F_SETFD, FD_CLOEXEC) < 0) return 1;
    }

    OSStatus e = MIDIClientCreate(CFSTR("Audio 8 DJ"), NULL, NULL, &gClient);
    if (e != noErr) {
        fprintf(stderr, "MIDIClientCreate 失敗: %d\n", (int)e);
        return 1;
    }
    e = MIDISourceCreate(gClient, CFSTR("Audio 8 DJ MIDI IN"), &gSource);
    if (e != noErr) fprintf(stderr, "MIDISourceCreate 失敗: %d\n", (int)e);
    e = MIDIDestinationCreate(gClient, CFSTR("Audio 8 DJ MIDI OUT"),
                              DestReadProc, NULL, &gDest);
    if (e != noErr) fprintf(stderr, "MIDIDestinationCreate 失敗: %d\n", (int)e);
    if (!gSource && !gDest) return 1;

    ParserReset();
    printf("Audio 8 DJ MIDI エージェント起動\n");
    printf("  仮想ポート : Audio 8 DJ MIDI IN / Audio 8 DJ MIDI OUT\n");
    printf("  FIFO       : %s (受信) / %s (送信)\n", FIFO_TO_HOST, FIFO_TO_DEV);
    fflush(stdout);

    // CoreMIDI callbacks enqueue whole packets. This ordinary thread retries
    // FIFO backpressure/partial writes instead of silently dropping a suffix.
    while (!gStop) {
        struct pollfd fds[3] = {
            { .fd = gToHostFd, .events = POLLIN },
            { .fd = gWake[0], .events = POLLIN },
            { .fd = gToDevFd, .events = A8DJ_MidiQueueSize(&gOutputQueue) ? POLLOUT : 0 },
        };
        int r = poll(fds, 3, 500);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents & POLLIN) {
            uint8_t wake[256];
            while (read(gWake[0], wake, sizeof(wake)) > 0) {}
        }
        uint8_t outgoing[512];
        size_t count = A8DJ_MidiQueuePeek(&gOutputQueue, outgoing, sizeof(outgoing));
        if (count) {
            ssize_t written = write(gToDevFd, outgoing, count);
            if (written > 0) {
                A8DJ_MidiQueueConsume(&gOutputQueue, (size_t)written);
                gOutBytes += (unsigned long)written;
            } else if (written < 0 && errno != EAGAIN && errno != EINTR) break;
        }
        if (fds[0].revents & (POLLERR | POLLNVAL)) break;
        if (!(fds[0].revents & POLLIN)) continue;
        uint8_t buf[512];
        ssize_t n = read(gToHostFd, buf, sizeof(buf));
        if (n > 0) {
            gInBytes += (unsigned long)n;
            gStamp = mach_absolute_time();
            ParseBytes(buf, (int)n);
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
    }

    printf("\n終了 (受信 %lu バイト / 送信 %lu バイト / 送信欠落 %lu パケット)\n",
           gInBytes, (unsigned long)gOutBytes, (unsigned long)gDroppedPackets);
    if (gSource) MIDIEndpointDispose(gSource);
    if (gDest)   MIDIEndpointDispose(gDest);
    if (gClient) MIDIClientDispose(gClient);
    close(gWake[0]); close(gWake[1]);
    close(gToHostFd); close(gToDevFd);
    return 0;
}
