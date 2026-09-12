#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include "../plugin/midi_fifo.h"
#include "../plugin/midi_queue.h"
#include "../plugin/status_file.h"
static A8DJ_MidiQueue q = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static void *ProducePackets(void *context)
{
    uint32_t prefix = (uint32_t)(uintptr_t)context;
    for (uint32_t i = 0; i < 5000; i++) {
        uint32_t word = prefix | i;
        assert(A8DJ_MidiQueuePush(&q, (uint8_t *)&word, sizeof(word)));
    }
    return NULL;
}
int main(void)
{
    uint8_t data[1024], out[1024];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    q.head = q.tail = UINT32_MAX - 100; // wrap both cursor and physical storage
    assert(A8DJ_MidiQueuePush(&q, data, sizeof(data)));
    assert(A8DJ_MidiQueuePeek(&q, out, sizeof(out)) == sizeof(out));
    assert(!memcmp(data, out, sizeof(out)));
    A8DJ_MidiQueueConsume(&q, 7); // emulate a partial FIFO write
    assert(A8DJ_MidiQueuePeek(&q, out, sizeof(out)) == sizeof(data) - 7);
    assert(!memcmp(data + 7, out, sizeof(data) - 7));
    A8DJ_MidiQueueConsume(&q, sizeof(data));
    assert(!A8DJ_MidiQueueSize(&q));
    for (size_t i = 0; i < A8DJ_MIDI_QUEUE_CAPACITY / sizeof(data); i++)
        assert(A8DJ_MidiQueuePush(&q, data, sizeof(data)));
    assert(!A8DJ_MidiQueuePush(&q, data, 1));
    assert(A8DJ_MidiQueueSize(&q) == A8DJ_MIDI_QUEUE_CAPACITY);
    assert(A8DJ_MidiQueuePeek(&q, out, sizeof(out)) == sizeof(out));
    assert(!memcmp(data, out, sizeof(out))); // failed push must not corrupt queued bytes
    A8DJ_MidiQueueConsume(&q, A8DJ_MIDI_QUEUE_CAPACITY);
    pthread_t producers[2];
    assert(!pthread_create(&producers[0], NULL, ProducePackets, (void *)(uintptr_t)0xa0000000));
    assert(!pthread_create(&producers[1], NULL, ProducePackets, (void *)(uintptr_t)0xb0000000));
    pthread_join(producers[0], NULL); pthread_join(producers[1], NULL);
    uint32_t seen[2] = {0,0};
    while (A8DJ_MidiQueueSize(&q)) {
        uint32_t word;
        assert(A8DJ_MidiQueuePeek(&q, (uint8_t *)&word, sizeof(word)) == sizeof(word));
        A8DJ_MidiQueueConsume(&q, sizeof(word));
        unsigned producer = (word >> 28) - 10;
        assert(producer < 2 && (word & 0xfffffff) == seen[producer]++);
    }
    assert(seen[0] == 5000 && seen[1] == 5000);
    char directory[] = "/tmp/a8dj-transport-test-XXXXXX";
    assert(mkdtemp(directory));
    char path[PATH_MAX], target[PATH_MAX];
    snprintf(path, sizeof(path), "%s/fifo", directory);
    snprintf(target, sizeof(target), "%s/target", directory);
    int fd = A8DJ_OpenMidiFifo(path);
    assert(fd >= 0);
    struct stat st;
    assert(!fstat(fd, &st) && S_ISFIFO(st.st_mode));
    assert((fcntl(fd, F_GETFD) & FD_CLOEXEC) && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    assert(write(fd, data, sizeof(data)) == sizeof(data));
    assert(read(fd, out, sizeof(out)) == sizeof(out));
    assert(!memcmp(data, out, sizeof(data)));
    close(fd); unlink(path);
    fd = open(target, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, data, sizeof(data)) == sizeof(data));
    close(fd);
    assert(A8DJ_OpenMidiFifo(target) == -1);
    assert(!stat(target, &st) && (st.st_mode & 0777) == 0600 && st.st_size == sizeof(data));
    assert(!symlink(target, path));
    assert(A8DJ_OpenMidiFifo(path) == -1);
    assert(!A8DJ_OpenStatusFile(path));
    unlink(path);
    assert(!link(target, path));
    assert(!A8DJ_OpenStatusFile(path));
    assert(!stat(target, &st) && st.st_size == sizeof(data));
    unlink(path);
    assert(!mkfifo(path, 0600));
    assert(!A8DJ_OpenStatusFile(path));
    unlink(path);
    FILE *status = A8DJ_OpenStatusFile(path);
    assert(status);
    fputs("status", status); fclose(status);
    assert(!stat(path, &st) && st.st_size == 6);
    unlink(path); unlink(target); rmdir(directory);
    puts("PASS: MIDI partial-write queue, capacity, cursor wrap, safe FIFO/status file handling");
}
