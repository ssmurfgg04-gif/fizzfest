/*
 * fizzbuzz-arena load generator
 *
 * Fixed connection count, fixed duration. Every connection runs a strict
 * request / response loop over HTTP/1.1 keep-alive:
 *
 *   GET /N HTTP/1.1\r\nHost: fizz.local\r\n\r\n      (N random in [1, 2^31-1])
 *
 * A response only counts if the status is 200, Content-Length matches the
 * body length and the body is the EXACT expected bytes for N:
 *   fizz | buzz | fizzbuzz | decimal digits of N
 *
 * Output: JSON on stdout and to the file given with -o.
 *
 * Build:  cc -O2 -o loadgen loadgen.c -lpthread
 * Usage:  ./loadgen -h 127.0.0.1 -p 8080 -c 5000 -t 30 -T 2 -o result.json
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>

/* ---------------- config ---------------- */

static const char *g_ip   = "127.0.0.1";
static int         g_port = 8080;
static long        g_conns = 5000;    /* total connections */
static double      g_seconds = 30.0;  /* measured duration */
static int         g_threads = 2;
static double g_timeout_s = 10.0; /* per-response watchdog */
static const char *g_out = NULL;

/* ---------------- stats ---------------- */

#define NBUCK 15
static const double buck_ms[NBUCK] = {
    0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2500, 5000
};

typedef struct {
    long long total;      /* fully verified responses */
    long long incorrect;  /* wrong body/status  */
    long long neterr;     /* connection level errors */
    long long timeouts;
    long long conns_opened;
    double   lat_sum;
    long long hist[NBUCK];
} stat_t;

/* per-thread slice, padded to avoid false sharing */
typedef struct {
    stat_t st;
    char pad[64];
} padded_stat_t;

static padded_stat_t *g_stats;
static volatile int g_running = 1;
static long long g_t0_us;

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void die(const char *m)
{
    fprintf(stderr, "loadgen: %s: %s\n", m, strerror(errno));
    exit(1);
}

/* ---------------- rng ---------------- */

typedef struct { uint64_t s; } rng_t;

static inline uint64_t rnd_next(rng_t *r)
{
    uint64_t x = r->s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    r->s = x;
    return x;
}

/* ---------------- expected body ---------------- */

static inline int expected_body(uint32_t n, char out[16])
{
    if (n % 15 == 0) { memcpy(out, "fizzbuzz", 8); return 8; }
    if (n % 3  == 0) { memcpy(out, "fizz", 4); return 4; }
    if (n % 5  == 0) { memcpy(out, "buzz", 4); return 4; }
    /* decimal digits, no leading zeros, no newline */
    char tmp[10]; int i = 0;
    uint32_t v = n;
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
    return i;
}

/* ---------------- per connection state ---------------- */

#define RBUF 4096

typedef struct {
    int fd;
    int slot;              /* thread index */
    int state;             /* 0 = connecting, 1 = request sent, 2 = reading */
    char req[64];
    int  reqlen;
    uint32_t cur_n;        /* N of the in-flight request */
    long long sent_us;     /* when the request was (re)sent */
    char buf[RBUF];
    int  buflen;
    int  bodylen;          /* from Content-Length, -1 until parsed */
    int  hdr_end;          /* offset of \r\n\r\n */
    rng_t rng;
    long long reconnect_at_us; /* if > 0, sleeping before reconnect */
} conn_t;

typedef struct {
    int epfd;
    conn_t *conns;
    int nconns;
    int next_alloc;
    rng_t rng;
} thr_t;

/* ---------------- http pieces ---------------- */

static int build_request(char *out, uint32_t n)
{
    return sprintf(out, "GET /%u HTTP/1.1\r\nHost: fizz.local\r\n\r\n", n);
}

/*
 * Parse the response in v->buf[0 .. v->buflen).
 * Returns:
 *   1  complete and correct
 *   0  complete but wrong (counted by caller as incorrect)
 *  -1  not complete yet
 *  -2  protocol garbage (caller counts as incorrect and drops conn)
 */
static int check_response(conn_t *v)
{
    char *b = v->buf;
    int len = v->buflen;

    /* status line */
    if (len < 12) return -1;
    if (memcmp(b, "HTTP/1.", 7) != 0) return -2;
    if (b[7] < '0' || b[7] > '9') return -2;
    if (b[8] != ' ') return -1;
    int sc = 0;
    int i = 9;
    for (; i < len && b[i] != ' ' && b[i] != '\r'; i++) {
        if (b[i] < '0' || b[i] > '9') return -2;
        sc = sc * 10 + (b[i] - '0');
    }
    if (i >= len || b[i] != ' ') return -1;
    if (sc != 200) return -2;

    /* find end of headers */
    char *hdr_end = NULL;
    int limit = len < 512 ? len : 512;
    for (int j = 0; j + 3 < limit; j++) {
        if (b[j] == '\r' && b[j+1] == '\n' && b[j+2] == '\r' && b[j+3] == '\n') {
            hdr_end = b + j + 4;
            v->hdr_end = j + 4;
            break;
        }
    }
    if (!hdr_end) return limit == 512 ? -2 : -1;

    /* content-length (case-insensitive, required) */
    long cl = -1;
    char *h = b;
    while (h < hdr_end - 4) {
        char *eol = memchr(h, '\n', (size_t)(hdr_end - h));
        if (!eol) break;
        if (eol - h >= 16 && (memcmp(h, "Content-Length:", 15) == 0 ||
                              memcmp(h, "content-length:", 15) == 0)) {
            cl = strtol(h + 15, NULL, 10);
        }
        h = eol + 1;
    }
    if (cl < 0) return -2;            /* must send Content-Length */
    if (cl > 64) return -2;           /* bodies here are tiny */

    if (len < v->hdr_end + cl) return -1;

    /* exact body */
    char want[16];
    int wantlen = expected_body(v->cur_n, want);
    if (cl != wantlen || memcmp(hdr_end, want, (size_t)cl) != 0) return 0;

    /* keep any pipelined leftovers (should not happen, we never pipeline) */
    int total = v->hdr_end + cl;
    if (total < len) return -2;       /* unexpected extra bytes: garbage */
    return 1;
}

/* ---------------- connection handling ---------------- */

static void conn_start(thr_t *T, conn_t *v);
static void conn_send_request(thr_t *T, conn_t *v);

static void conn_rearm(int epfd, conn_t *v, int want_out, int op)
{
    struct epoll_event ev;
    ev.events = want_out ? EPOLLOUT : EPOLLIN;
    ev.data.ptr = v;
    if (epoll_ctl(epfd, op, v->fd, &ev) < 0) {
        /* MOD on a gone fd etc: ignore, caller will notice on next event */
    }
}

static void conn_start(thr_t *T, conn_t *v)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) die("socket");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)g_port);
    dst.sin_addr.s_addr = inet_addr(g_ip);

    v->fd = fd;
    v->state = 0;
    v->buflen = 0;
    v->bodylen = -1;
    g_stats[v->slot].st.conns_opened++;

    int rc = connect(fd, (struct sockaddr *)&dst, sizeof dst);
    if (rc == 0) {
        conn_rearm(T->epfd, v, 0, EPOLL_CTL_ADD);
        conn_send_request(T, v);
        return;
    }
    if (errno != EINPROGRESS) { close(fd); v->fd = -1; return; }

    conn_rearm(T->epfd, v, 1, EPOLL_CTL_ADD);
}

static void conn_fail(thr_t *T, conn_t *v, const char *why)
{
    (void)why;
    stat_t *s = &g_stats[v->slot].st;
    if (v->state >= 1) s->neterr++;
    epoll_ctl(T->epfd, EPOLL_CTL_DEL, v->fd, NULL);
    close(v->fd);
    v->fd = -1;
    /* brief cooldown then reconnect so pressure stays on */
    v->reconnect_at_us = now_us() + 100000; /* 100ms */
}

static void conn_send_request(thr_t *T, conn_t *v)
{
    uint32_t n = 1 + (uint32_t)(rnd_next(&v->rng) % 2147483647u);
    v->cur_n = n;
    v->reqlen = build_request(v->req, n);
    v->buflen = 0;
    v->bodylen = -1;
    v->sent_us = now_us();

    ssize_t w = write(v->fd, v->req, (size_t)v->reqlen);
    if (w == v->reqlen) {
        v->state = 2;
        conn_rearm(T->epfd, v, 0, EPOLL_CTL_MOD);
    } else if (w >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        conn_fail(T, v, "write");
    } else {
        v->state = 1;               /* partial, finish on EPOLLOUT */
        conn_rearm(T->epfd, v, 1, EPOLL_CTL_MOD);
    }
}

static void conn_on_readable(thr_t *T, conn_t *v)
{
    for (;;) {
        if (v->buflen >= RBUF) { conn_fail(T, v, "overflow"); return; }
        ssize_t r = read(v->fd, v->buf + v->buflen, (size_t)(RBUF - v->buflen));
        if (r > 0) {
            v->buflen += (int)r;
            int rc = check_response(v);
            if (rc == 1) {
                stat_t *s = &g_stats[v->slot].st;
                double ms = (now_us() - v->sent_us) / 1000.0;
                s->total++;
                s->lat_sum += ms;
                for (int i = 0; i < NBUCK; i++) {
                    if (ms <= buck_ms[i]) { s->hist[i]++; break; }
                }
                conn_send_request(T, v);   /* immediately next request */
                return;
            }
            if (rc == 0 || rc == -2) {
                g_stats[v->slot].st.incorrect++;
                conn_fail(T, v, "bad response");
                return;
            }
            continue;                    /* need more bytes */
        }
        if (r == 0) { conn_fail(T, v, "eof"); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn_fail(T, v, "read");
        return;
    }
}

/* ---------------- thread main ---------------- */

static void *thread_main(void *arg)
{
    thr_t *T = arg;

    /* open our connections, gently staggered so we do not SYN storm */
    int burst = T->nconns / 64 + 1;
    for (int i = 0; i < T->nconns && g_running; i++) {
        conn_t *v = &T->conns[i];
        if (v->fd < 0) conn_start(T, v);
        if (i % burst == burst - 1) usleep(2000);
    }

    struct epoll_event evs[512];
    while (g_running) {
        int n = epoll_wait(T->epfd, evs, 512, 50);
        long long now = now_us();
        for (int i = 0; i < n; i++) {
            conn_t *v = evs[i].data.ptr;
            uint32_t e = evs[i].events;
            if (v->fd < 0) continue;
            if (v->state == 0 && (e & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(v->fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err) { conn_fail(T, v, "connect"); continue; }
                v->state = 1;
                conn_send_request(T, v);
                continue;
            }
            if (e & EPOLLOUT && v->state == 1) {
                ssize_t w = write(v->fd, v->req, (size_t)v->reqlen);
                if (w == v->reqlen) {
                    v->state = 2;
                    conn_rearm(T->epfd, v, 0, EPOLL_CTL_MOD);
                } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    conn_fail(T, v, "rewrite");
                }
                continue;
            }
            if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                conn_on_readable(T, v);
                continue;
            }
        }
        /* reconnects + watchdogs */
        for (int i = 0; i < T->nconns; i++) {
            conn_t *v = &T->conns[i];
            if (v->fd >= 0 && v->state == 2 &&
                now - v->sent_us > (long long)(g_timeout_s * 1000000.0)) {
                g_stats[v->slot].st.timeouts++;
                conn_fail(T, v, "timeout");
            } else if (v->fd < 0 && v->reconnect_at_us &&
                       now >= v->reconnect_at_us && g_running) {
                conn_start(T, v);
            }
        }
    }
    return NULL;
}

/* ---------------- main ---------------- */

static double pct_from_hist(const long long *hist, long long total, double p)
{
    long long want = (long long)(total * p / 100.0);
    long long acc = 0;
    for (int i = 0; i < NBUCK; i++) {
        acc += hist[i];
        if (acc >= want && want > 0)
            return buck_ms[i];
    }
    return buck_ms[NBUCK - 1];
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "bad args\n"); return 2; }
        const char *k = argv[i], *val = argv[i+1];
        if (!strcmp(k, "-h")) g_ip = val;
        else if (!strcmp(k, "-p")) g_port = atoi(val);
        else if (!strcmp(k, "-c")) g_conns = atol(val);
        else if (!strcmp(k, "-t")) g_seconds = atof(val);
        else if (!strcmp(k, "-T")) g_threads = atoi(val);
        else if (!strcmp(k, "-o")) g_out = val;
        else { fprintf(stderr, "unknown flag %s\n", k); return 2; }
    }
    if (g_threads < 1) g_threads = 1;
    if (g_conns < g_threads) g_conns = g_threads;

    /* raise fd limit: conns + epoll + slack */
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < (rlim_t)(g_conns + 256)) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    g_stats = calloc((size_t)g_threads, sizeof *g_stats);
    thr_t *ths = calloc((size_t)g_threads, sizeof *ths);

    int per = (int)(g_conns / g_threads);
    int extra = (int)(g_conns % g_threads);

    for (int t = 0; t < g_threads; t++) {
        ths[t].epfd = epoll_create1(0);
        int mine = per + (t < extra ? 1 : 0);
        ths[t].conns = calloc((size_t)mine, sizeof(conn_t));
        ths[t].nconns = mine;
        for (int i = 0; i < mine; i++) {
            conn_t *v = &ths[t].conns[i];
            v->fd = -1;
            v->slot = t;
            v->rng.s = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(t * 100003 + i);
            if (!v->rng.s) v->rng.s = 88172645463325252ULL;
        }
    }

    /* threads open their own connections, we just wait */
    g_t0_us = now_us();

    pthread_t *pids = calloc((size_t)g_threads, sizeof *pids);
    for (int t = 0; t < g_threads; t++)
        pthread_create(&pids[t], NULL, thread_main, t ? &ths[t] : &ths[0]);

    long long t_start = now_us();
    usleep((useconds_t)(g_seconds * 1000000.0));
    g_running = 0;
    long long t_end = now_us();
    double measured = (double)(t_end - t_start) / 1000000.0;

    for (int t = 0; t < g_threads; t++) pthread_join(pids[t], NULL);

    stat_t tot;
    memset(&tot, 0, sizeof tot);
    for (int t = 0; t < g_threads; t++) {
        stat_t *s = &g_stats[t].st;
        tot.total += s->total; tot.incorrect += s->incorrect;
        tot.neterr += s->neterr; tot.timeouts += s->timeouts;
        tot.conns_opened += s->conns_opened; tot.lat_sum += s->lat_sum;
        for (int i = 0; i < NBUCK; i++) tot.hist[i] += s->hist[i];
    }

    double rps = tot.total / measured;
    double p50 = pct_from_hist(tot.hist, tot.total, 50);
    double p95 = pct_from_hist(tot.hist, tot.total, 95);
    double p99 = pct_from_hist(tot.hist, tot.total, 99);
    long long attempts = tot.total + tot.incorrect + tot.neterr + tot.timeouts;
    double err_pct = attempts ? 100.0 * (tot.incorrect + tot.neterr + tot.timeouts) / attempts : 0.0;

    char *json = NULL;
    size_t cap = 0;
    FILE *mem = open_memstream(&json, &cap);
    fprintf(mem,
        "{\n"
        "  \"rps\": %.1f,\n"
        "  \"total\": %lld,\n"
        "  \"incorrect\": %lld,\n"
        "  \"neterr\": %lld,\n"
        "  \"timeouts\": %lld,\n"
        "  \"error_pct\": %.4f,\n"
        "  \"p50_ms\": %.1f,\n"
        "  \"p95_ms\": %.1f,\n"
        "  \"p99_ms\": %.1f,\n"
        "  \"avg_ms\": %.3f,\n"
        "  \"conns\": %ld,\n"
        "  \"duration_s\": %.1f\n"
        "}\n",
        rps, tot.total, tot.incorrect, tot.neterr, tot.timeouts, err_pct,
        p50, p95, p99,
        tot.total ? tot.lat_sum / tot.total : 0.0,
        g_conns, measured);
    fclose(mem);

    printf("%s", json);
    if (g_out) {
        FILE *f = fopen(g_out, "w");
        if (f) { fputs(json, f); fclose(f); }
    }
    free(json);
    return (err_pct > 0.01 || tot.incorrect > 0) ? 3 : 0;
}
