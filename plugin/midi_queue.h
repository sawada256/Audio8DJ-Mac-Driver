// Byte queue between CoreMIDI producers and the agent's ordinary IO thread.
// Only memory copies occur under the mutex; FIFO IO never holds it.
#ifndef A8DJ_MIDI_QUEUE_H
#define A8DJ_MIDI_QUEUE_H
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define A8DJ_MIDI_QUEUE_CAPACITY 131072u

typedef struct {
    pthread_mutex_t mutex;
    uint32_t head, tail;
    uint8_t bytes[A8DJ_MIDI_QUEUE_CAPACITY];
} A8DJ_MidiQueue;

static inline size_t A8DJ_MidiQueueSize(A8DJ_MidiQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    size_t size = (uint32_t)(q->head - q->tail);
    pthread_mutex_unlock(&q->mutex);
    return size;
}
static inline int A8DJ_MidiQueuePush(A8DJ_MidiQueue *q, const uint8_t *bytes, size_t count)
{
    if (!bytes || !count || count > A8DJ_MIDI_QUEUE_CAPACITY) return 0;
    pthread_mutex_lock(&q->mutex);
    size_t free = A8DJ_MIDI_QUEUE_CAPACITY - (uint32_t)(q->head - q->tail);
    if (count > free) { pthread_mutex_unlock(&q->mutex); return 0; }
    size_t offset = q->head % A8DJ_MIDI_QUEUE_CAPACITY;
    size_t first = A8DJ_MIDI_QUEUE_CAPACITY - offset;
    if (first > count) first = count;
    memcpy(q->bytes + offset, bytes, first);
    memcpy(q->bytes, bytes + first, count - first);
    q->head += (uint32_t)count;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}
// Only the one consumer calls Peek/Consume. A short write consumes only the
// bytes actually accepted, leaving the suffix in the queue for a later retry.
static inline size_t A8DJ_MidiQueuePeek(A8DJ_MidiQueue *q, uint8_t *bytes, size_t cap)
{
    if (!bytes) return 0;
    pthread_mutex_lock(&q->mutex);
    size_t count = (uint32_t)(q->head - q->tail);
    if (count > cap) count = cap;
    size_t offset = q->tail % A8DJ_MIDI_QUEUE_CAPACITY;
    size_t first = A8DJ_MIDI_QUEUE_CAPACITY - offset;
    if (first > count) first = count;
    memcpy(bytes, q->bytes + offset, first);
    memcpy(bytes + first, q->bytes, count - first);
    pthread_mutex_unlock(&q->mutex);
    return count;
}
static inline void A8DJ_MidiQueueConsume(A8DJ_MidiQueue *q, size_t count)
{
    pthread_mutex_lock(&q->mutex);
    size_t available = (uint32_t)(q->head - q->tail);
    if (count > available) count = available;
    q->tail += (uint32_t)count;
    pthread_mutex_unlock(&q->mutex);
}
#endif
