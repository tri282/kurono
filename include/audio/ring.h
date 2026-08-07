#ifndef KURONO_AUDIO_RING_H
#define KURONO_AUDIO_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Lock-free single-producer/single-consumer byte ring.
 *
 * SPSC ASSUMPTION (binding on every caller): at any instant there is
 * exactly one thread that may call ring_write()/ring_free_space() (the
 * producer) and exactly one thread that may call ring_read()/
 * ring_read_latest()/ring_fill() (the consumer). The producer role and
 * the consumer role may migrate between threads over the ring's
 * lifetime (DESIGN.md IV.4 rule 2 -- e.g. pcm_ring's consumer is T1
 * during ma_device_start's pre-fill and T3 thereafter), but the two
 * roles are never held concurrently by more than one thread each, and
 * every producer/consumer handoff must carry its own happens-before
 * edge from outside this structure (ring.c supplies none). Adding a
 * second concurrent producer or consumer breaks this silently.
 */
typedef struct {
  _Alignas(64) atomic_size_t write_pos;   /* producer only writes */
  _Alignas(64) atomic_size_t read_pos;    /* consumer only writes */
  _Alignas(64) uint8_t *     buf;
  size_t                     cap;         /* power of two */
} Ring;

int    ring_init(Ring * r, size_t cap_bytes);   /* rounds up to a power of two */
void   ring_free(Ring * r);
size_t ring_write(Ring * r, const void * src, size_t n);
size_t ring_read(Ring * r, void * dst, size_t n);
size_t ring_read_latest(Ring * r, void * dst, size_t n);
void   ring_reset(Ring * r);
size_t ring_fill(const Ring * r);
size_t ring_free_space(const Ring * r);

#endif // KURONO_AUDIO_RING_H
