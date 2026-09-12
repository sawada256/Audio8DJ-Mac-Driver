// FIFO transport between the HAL USB owner and the per-user CoreMIDI agent.

#include "midi.h"
#include "midi_fifo.h"
#include <stdatomic.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIFO_TO_HOST "/tmp/a8dj-midi-tohost"
#define FIFO_TO_DEV  "/tmp/a8dj-midi-todev"

static _Atomic int  gToHostFd = -1;
static int          gToDevFd  = -1;
static pthread_t    gThread;
static _Atomic int  gThreadValid;
static _Atomic int gRun;
static _Atomic unsigned gDropped;
static pthread_mutex_t gLifecycleMutex = PTHREAD_MUTEX_INITIALIZER;

// O_RDWR rather than O_WRONLY/O_RDONLY so the FIFO always has both ends open:
// no ENXIO when the agent is not running yet, and no SIGPIPE when it exits.
// Changing this process's SIGPIPE disposition is not an option, it is shared
// with everything else Apple's driver host does.
static int OpenFifo(const char *path)
{
    return A8DJ_OpenMidiFifo(path);
}

// Host -> device. Blocking USB writes are fine here: this is an ordinary
// thread, deliberately not the time-constraint streaming one.
static void *MidiThread(void *arg)
{
    (void)arg;
    pthread_setname_np("Audio8DJ midi");

    while (gRun) {
        if (gToDevFd < 0) {
            gToDevFd = OpenFifo(FIFO_TO_DEV);
            if (gToDevFd < 0) { usleep(500 * 1000); continue; }
        }
        int busy = 0;

        uint8_t buf[256];
        ssize_t n = read(gToDevFd, buf, sizeof(buf));
        if (n > 0) {
            int off = 0;
            while (off < n) {
                int chunk = (int)n - off;
                if (chunk > 60) chunk = 60;    // only 40-byte writes are verified
                if (A8DJ_UsbMidiSend(buf + off, chunk) != 0) gDropped++;
                off += chunk;
            }
            busy = 1;
        }

        // Device -> host. The EP1 completion runs on the time-constraint
        // streaming thread and only fills a ring; the write to the FIFO has to
        // happen here, where blocking on the filesystem is allowed.
        uint8_t rx[512];
        int got = A8DJ_UsbMidiReceive(rx, (int)sizeof(rx));
        if (got > 0) { A8DJ_MidiFromDevice(rx, got); busy = 1; }

        if (!busy) usleep(2 * 1000);
    }
    return NULL;
}

int A8DJ_MidiInit(void)
{
    pthread_mutex_lock(&gLifecycleMutex);
    if (gToHostFd < 0) gToHostFd = OpenFifo(FIFO_TO_HOST);
    if (!gThreadValid) {
        gRun = 1;
        if (pthread_create(&gThread, NULL, MidiThread, NULL) == 0) gThreadValid = 1;
        else gRun = 0;
    }
    int result = (gToHostFd >= 0 && gThreadValid) ? 0 : -1;
    pthread_mutex_unlock(&gLifecycleMutex);
    return result;
}

void A8DJ_MidiTeardown(void)
{
    pthread_mutex_lock(&gLifecycleMutex);
    gRun = 0;
    if (gThreadValid) { pthread_join(gThread, NULL); gThreadValid = 0; }
    if (gToHostFd >= 0) { close(gToHostFd); gToHostFd = -1; }
    if (gToDevFd  >= 0) { close(gToDevFd);  gToDevFd  = -1; }
    pthread_mutex_unlock(&gLifecycleMutex);
}

const char *A8DJ_MidiStatus(void)
{
    static _Thread_local char buf[128];
    snprintf(buf, sizeof(buf), "%s%s  (a8dj-midid が仮想ポートを作ります)",
             (gToHostFd >= 0 && gThreadValid) ? "FIFO ok" : "FIFO 失敗",
             gDropped ? " 取りこぼしあり" : "");
    return buf;
}

// Device -> host. Called only by MidiThread after draining the USB ring.
// Teardown joins that thread before closing the descriptors.
void A8DJ_MidiFromDevice(const uint8_t *bytes, int count)
{
    if (!bytes || gToHostFd < 0 || count <= 0) return;
    ssize_t w = write(gToHostFd, bytes, (size_t)count);
    if (w != count) gDropped++;
}
