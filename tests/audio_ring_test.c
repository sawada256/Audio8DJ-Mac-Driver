#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../plugin/audio_ring.h"
#define CAPACITY 64
#define CHANNELS 8
static A8DJ_RingSlot slots[CAPACITY];
static float audio[CAPACITY][CHANNELS];
static _Atomic uint64_t latest;
static _Atomic int done;
static void *Producer(void *unused)
{
    (void)unused;
    for (uint64_t frame = 1; frame <= 200000; frame++) {
        float input[CHANNELS];
        for (int c = 0; c < CHANNELS; c++) input[c] = (float)frame;
        size_t index = frame % CAPACITY;
        if (A8DJ_RingWrite(audio[index], &slots[index], frame, 1, input, CHANNELS))
            latest = frame;
    }
    done = 1;
    return NULL;
}
int main(void)
{
    for (size_t i = 0; i < CAPACITY; i++) atomic_flag_clear(&slots[i].busy);
    float input[CHANNELS] = { 1, 2, 3, 4, 5, 6, 7, 8 }, out[CHANNELS];
    assert(!A8DJ_RingRead(audio[0], &slots[0], 0, 0, out, CHANNELS));
    assert(A8DJ_RingWrite(audio[0], &slots[0], 0, 1, input, CHANNELS));
    assert(A8DJ_RingRead(audio[0], &slots[0], 0, 1, out, CHANNELS));
    assert(!memcmp(input, out, sizeof(input)));
    assert(!A8DJ_RingRead(audio[0], &slots[0], CAPACITY, 1, out, CHANNELS));
    assert(!A8DJ_RingRead(audio[0], &slots[0], 0, 2, out, CHANNELS));
    assert(A8DJ_RingWrite(audio[0], &slots[0], CAPACITY, 1, input, CHANNELS));
    assert(!A8DJ_RingRead(audio[0], &slots[0], 0, 1, out, CHANNELS));
    assert(!A8DJ_RingWrite(audio[0], &slots[0], 0, 1, input, CHANNELS));
    assert(A8DJ_RingWrite(audio[0], &slots[0], 0, 2, input, CHANNELS));
    assert(!A8DJ_RingWrite(audio[0], &slots[0], CAPACITY, 1, input, CHANNELS));
    assert(!atomic_flag_test_and_set(&slots[0].busy));
    assert(!A8DJ_RingRead(audio[0], &slots[0], 0, 2, out, CHANNELS));
    assert(!A8DJ_RingWrite(audio[0], &slots[0], 0, 2, input, CHANNELS));
    for (int c = 0; c < CHANNELS; c++) assert(out[c] == 0);
    atomic_flag_clear(&slots[0].busy);
    slots[0].generation = 0;
    pthread_t producer;
    assert(pthread_create(&producer, NULL, Producer, NULL) == 0);
    unsigned reads = 0;
    do {
        uint64_t frame = latest;
        size_t index = frame % CAPACITY;
        if (frame && A8DJ_RingRead(audio[index], &slots[index], frame, 1, out, CHANNELS)) {
            for (int c = 0; c < CHANNELS; c++) assert(out[c] == (float)frame);
            reads++;
        }
    } while (!done);
    pthread_join(producer, NULL);
    uint64_t final = latest;
    assert(A8DJ_RingRead(audio[final % CAPACITY], &slots[final % CAPACITY], final, 1, out, CHANNELS));
    for (int c = 0; c < CHANNELS; c++) assert(out[c] == (float)final);
    printf("PASS: stale/overwritten/contended frames and 200000 concurrent writes (%u reads)\n", reads);
}
