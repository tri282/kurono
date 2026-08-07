/* Lock-free SPSC byte ring. See ../../include/audio/ring.h for the binding
   single-producer/single-consumer assumption and the struct layout.

   Memory-order rationale (DESIGN.md IV.4 rule 3 -- fixed, not a matter of
   taste):

   - Each side's load of its OWN cursor is memory_order_relaxed: that
     cursor is written only by this same thread, so there is no other
     writer to synchronize with -- the compiler/CPU cannot reorder a
     thread's own prior store past its own later load of the same
     object regardless of memory order.
   - Each side's load of the OTHER side's cursor is memory_order_acquire:
     it must observe, and happen-after, everything the other thread did
     before its matching release-store of that cursor -- specifically
     the memcpy into/out of buf. This is what makes it safe to touch the
     bytes that store implies are now ours.
   - Each side's store of its OWN cursor, after finishing its memcpy, is
     memory_order_release: it publishes both the new cursor value and
     the memcpy that precedes it as a single happens-before edge to
     whichever thread next acquire-loads this cursor.
   - relaxed on a cursor STORE (as opposed to the same-thread load above)
     would be a defect: it would let the memcpy that precedes it become
     visible to the other thread out of order with the cursor update.
     x86-64's TSO hides this almost perfectly, which is exactly why
     DESIGN.md XVI risk 1 requires TSan evidence and not code review. */

#include "../../include/audio/ring.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* round_up_pow2: return the smallest power of two >= n, or 0 if n is 0
   or the result would overflow size_t. Pure, setup-time helper -- never
   called from the audio callback. */
static size_t
round_up_pow2(size_t n)
{
  if (n == 0) {
    return 0;
  }
  size_t p = 1;
  while (p < n) {
    if (p > (SIZE_MAX >> 1)) {
      return 0; /* would overflow on the next shift */
    }
    p <<= 1;
  }
  return p;
} /* round_up_pow2 */

/* ring_init: allocate r->buf and set r->cap to the smallest power of two
   >= cap_bytes. Returns 0 on success, -1 on invalid size or allocation
   failure. Setup-time only -- malloc is fine here; this function must
   never be called from T3. */
int
ring_init(Ring * r, size_t cap_bytes)
{
  size_t cap = round_up_pow2(cap_bytes);
  if (cap == 0) {
    return -1;
  }

  uint8_t * buf = malloc(cap);
  if (buf == NULL) {
    return -1;
  }

  r->buf = buf;
  r->cap = cap;
  atomic_store_explicit(&r->write_pos, 0, memory_order_relaxed);
  atomic_store_explicit(&r->read_pos, 0, memory_order_relaxed);
  return 0;
} /* ring_init */

/* ring_free: release the backing buffer. Setup/teardown-time only, never
   called from T3, and only once no producer or consumer can still be
   touching r. */
void
ring_free(Ring * r)
{
  free(r->buf);
  r->buf = NULL;
  r->cap = 0;
} /* ring_free */

/* ring_write: copy up to n bytes from src into r, returning the number of
   bytes actually written (n, or less if the ring does not have that much
   free space). Producer side only. Never blocks, never allocates, bounded
   by n -- safe to call from T2 (pcm_ring producer) and legal to call from
   T1 during ma_device_start's synchronous pre-fill (viz_ring producer is
   always T1). Not on the callback (T3) path. */
size_t
ring_write(Ring * r, const void * src, size_t n)
{
  size_t w  = atomic_load_explicit(&r->write_pos, memory_order_relaxed);
  size_t rd = atomic_load_explicit(&r->read_pos, memory_order_acquire);

  size_t used       = w - rd; /* unsigned wraparound-safe: see rule 1 */
  size_t free_space = r->cap - used;

  if (n > free_space) {
    n = free_space;
  }
  if (n == 0) {
    return 0;
  }

  size_t mask   = r->cap - 1;
  size_t start  = w & mask;
  size_t first  = r->cap - start; /* bytes to the end of the buffer */
  if (first > n) {
    first = n;
  }
  size_t second = n - first;

  memcpy(r->buf + start, src, first);
  if (second > 0) {
    memcpy(r->buf, (const uint8_t *) src + first, second);
  }

  atomic_store_explicit(&r->write_pos, w + n, memory_order_release);
  return n;
} /* ring_write */

/* ring_read: copy up to n bytes out of r into dst, returning the number of
   bytes actually read (n, or less if the ring does not have that much
   data). Consumer side only. Never blocks, never allocates, bounded by n
   -- this is the function T3's data callback calls on pcm_ring, so it
   must stay free of malloc/free, locks, syscalls, and unbounded loops;
   it is. */
size_t
ring_read(Ring * r, void * dst, size_t n)
{
  size_t rd = atomic_load_explicit(&r->read_pos, memory_order_relaxed);
  size_t w  = atomic_load_explicit(&r->write_pos, memory_order_acquire);

  size_t avail = w - rd; /* unsigned wraparound-safe: see rule 1 */
  if (n > avail) {
    n = avail;
  }
  if (n == 0) {
    return 0;
  }

  size_t mask   = r->cap - 1;
  size_t start  = rd & mask;
  size_t first  = r->cap - start;
  if (first > n) {
    first = n;
  }
  size_t second = n - first;

  memcpy(dst, r->buf + start, first);
  if (second > 0) {
    memcpy((uint8_t *) dst + first, r->buf, second);
  }

  atomic_store_explicit(&r->read_pos, rd + n, memory_order_release);
  return n;
} /* ring_read */

/* ring_read_latest: discard everything older than the last n bytes of the
   stream, then read up to n bytes into dst, returning the number of bytes
   actually read. Consumer side only. Legal exclusively because viz_ring
   has a single consumer thread (T1, DESIGN.md IV.4 rule 7) that owns
   read_pos outright -- jumping read_pos forward to drop stale frames is
   not a race because nothing else ever reads or writes it. Not called
   from any v1 code path (the spectrum visualiser is post-v1); kept so the
   consumer-side API does not change when it lands. */
size_t
ring_read_latest(Ring * r, void * dst, size_t n)
{
  size_t rd = atomic_load_explicit(&r->read_pos, memory_order_relaxed);
  size_t w  = atomic_load_explicit(&r->write_pos, memory_order_acquire);

  size_t avail = w - rd;
  if (avail > n) {
    /* Skip the stale prefix. This advance is not itself a "read" of
       those bytes -- it is safe to publish immediately, and doing so
       before the memcpy costs nothing since no other thread ever
       inspects read_pos of viz_ring. */
    size_t skip = avail - n;
    rd += skip;
    avail = n;
  }
  size_t to_read = (avail < n) ? avail : n;
  if (to_read == 0) {
    atomic_store_explicit(&r->read_pos, rd, memory_order_release);
    return 0;
  }

  size_t mask  = r->cap - 1;
  size_t start = rd & mask;
  size_t first = r->cap - start;
  if (first > to_read) {
    first = to_read;
  }
  size_t second = to_read - first;

  memcpy(dst, r->buf + start, first);
  if (second > 0) {
    memcpy((uint8_t *) dst + first, r->buf, second);
  }

  atomic_store_explicit(&r->read_pos, rd + to_read, memory_order_release);
  return to_read;
} /* ring_read_latest */

/* ring_reset: rewind both cursors to zero, discarding all buffered data.
   May only be called when the consumer is provably not running -- there
   are exactly two legal call sites in the whole tree: the seek/flush
   handshake (DESIGN.md IV.7) and the device-reopen ladder (IV.8.b step
   5). Both call sites MUST immediately follow this with a frame-counter
   resync (T2's monotonic write counter and g_frames_consumed set to the
   same value) as a single logical operation -- a ring_reset without that
   resync silently breaks every subsequent track-boundary timestamp.
   Only T2 calls this function; T1 never does, not even under a mutex,
   because T3 does not take one. Release order on both stores even though
   the consumer is idle: cheap, and it means whichever thread resumes as
   consumer next is guaranteed to see the zeroed cursors without relying
   on happens-before edges supplied elsewhere. */
void
ring_reset(Ring * r)
{
  atomic_store_explicit(&r->read_pos, 0, memory_order_relaxed);
  atomic_store_explicit(&r->write_pos, 0, memory_order_release);
} /* ring_reset */

/* ring_fill: return a snapshot of the number of bytes currently buffered.
   Callable from either the producer or the consumer thread (or a third,
   purely observing thread such as T1 polling for a status display); it
   is advisory only -- by the time the caller reads the result, the true
   fill level may have already changed. Uses acquire on both loads so the
   snapshot at least reflects a consistent past state of each side, never
   a torn one. */
size_t
ring_fill(const Ring * r)
{
  size_t w  = atomic_load_explicit(&r->write_pos, memory_order_acquire);
  size_t rd = atomic_load_explicit(&r->read_pos, memory_order_acquire);
  return w - rd; /* unsigned wraparound-safe: see rule 1 */
} /* ring_fill */

/* ring_free_space: return a snapshot of the number of bytes currently
   free. Same advisory caveat as ring_fill. */
size_t
ring_free_space(const Ring * r)
{
  return r->cap - ring_fill(r);
} /* ring_free_space */
