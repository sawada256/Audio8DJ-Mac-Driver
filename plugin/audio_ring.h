// Per-frame publication for the two realtime audio rings.
// No spinning or blocking: a reader/writer loses a contended frame rather than
// stall an audio thread. Normally the configured lead separates both cursors.
#ifndef A8DJ_AUDIO_RING_H
#define A8DJ_AUDIO_RING_H
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    atomic_flag busy;
    uint64_t frame;
    uint64_t generation;
} A8DJ_RingSlot;

static inline int A8DJ_RingWrite(float *samples, A8DJ_RingSlot *slot,
                                 uint64_t frame, uint64_t generation,
                                 const float *source, size_t channels)
{
    if (atomic_flag_test_and_set_explicit(&slot->busy, memory_order_acquire)) return 0;
    int newer = generation > slot->generation ||
        (generation == slot->generation && frame >= slot->frame);
    if (newer) {
        memcpy(samples, source, channels * sizeof(float));
        slot->frame = frame;
        slot->generation = generation;
    }
    atomic_flag_clear_explicit(&slot->busy, memory_order_release);
    return newer;
}

static inline int A8DJ_RingRead(const float *samples, A8DJ_RingSlot *slot,
                                uint64_t frame, uint64_t generation,
                                float *destination, size_t channels)
{
    if (atomic_flag_test_and_set_explicit(&slot->busy, memory_order_acquire)) {
        memset(destination, 0, channels * sizeof(float));
        return 0;
    }
    int valid = generation != 0 && slot->generation == generation && slot->frame == frame;
    if (valid) memcpy(destination, samples, channels * sizeof(float));
    else memset(destination, 0, channels * sizeof(float));
    atomic_flag_clear_explicit(&slot->busy, memory_order_release);
    return valid;
}
#endif
