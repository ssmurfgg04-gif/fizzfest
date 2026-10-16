Language: C

# c-epoll-ssmurfgg04-gif

I starred fizzbuzz-arena

The maintainer's reference entry. Two threads, two `SO_REUSEPORT` listeners,
plain epoll, and fully precomputed response bytes so the hot path never
calls sprintf. It exists to prove the arena works end to end and to be a
readable, beatable target.

Approach in one breath:
- one epoll loop per core, each with its own listener socket (kernel
  spreads new connections between them, no accept lock)
- the three word responses (`fizz`, `buzz`, `fizzbuzz`) are complete static
  response buffers: one write() per request, zero formatting work
- digit responses are assembled from a header template plus a hand rolled
  itoa into a stack buffer
- request parsing is a tight handwritten scan, no copies, headers skipped

## Files

- `server.c`: the whole server
- `build.sh`: `cc -O3 -march=native -pthread`
- `start.sh`: runs it in the foreground on `$PORT`

## Honest notes

- This is intentionally not the theoretical maximum. io_uring, response
  batching and friends are left on the table so the first real challenger
  can take the crown in an afternoon.
- The ratio to beat is whatever the leaderboard says, not whatever I say.
