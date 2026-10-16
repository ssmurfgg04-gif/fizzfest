/*
 * fizzbuzz-arena: c-epoll-ssmurfgg04-gif
 *
 * Two threads, two SO_REUSEPORT listeners, epoll, precomputed responses.
 * See README.md in this folder.
 *
 * Build: cc -O3 -march=native -pthread -o server server.c
 * Run:   PORT=8080 ./server
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define THREADS 2
#define MAXEV   512
#define RBUF    1024

static int g_epfds[THREADS];
static int g_port_val = 80;

/* ---------- precomputed responses ---------- */

static const char R_FIZZ[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
    "Content-Length: 4\r\n\r\nfizz";
static const char R_BUZZ[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
    "Content-Length: 4\r\n\r\nbuzz";
static const char R_FIZZBUZZ[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
    "Content-Length: 8\r\n\r\nfizzbuzz";
static const char R_404[] =
    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
static const char R_405[] =
    "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";

/* number responses share a header prefix; the length line varies 1..10 */
static const char R_NUM_HEAD[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";

/* ---------- per connection ---------- */

typedef struct {
    int fd;
    char buf[RBUF];
    int buflen;
} conn_t;

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void drop_conn(int epfd, conn_t *c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

static int try_write(int fd, const char *s, int len)
{
    int off = 0;
    while (off < len) {
        ssize_t w = write(fd, s + off, (size_t)(len - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* small responses on loopback: retry a few times, then drop */
                for (int spin = 0; spin < 1000; spin++) {
                    w = write(fd, s + off, (size_t)(len - off));
                    if (w > 0) break;
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        return -1;
                }
                if (w <= 0) return -1;
            } else {
                return -1;
            }
        }
        off += (int)w;
    }
    return 0;
}

/* word_kind: 0 = plain number, 3 = fizz, 5 = buzz, 15 = fizzbuzz */
static int respond(conn_t *c, uint32_t n, int word_kind)
{
    if (word_kind == 15) return try_write(c->fd, R_FIZZBUZZ, (int)sizeof R_FIZZBUZZ - 1);
    if (word_kind == 3)  return try_write(c->fd, R_FIZZ,     (int)sizeof R_FIZZ - 1);
    if (word_kind == 5)  return try_write(c->fd, R_BUZZ,     (int)sizeof R_BUZZ - 1);

    char out[128];
    char digs[10];
    int nd = 0;
    uint32_t v = n;
    while (v) { digs[nd++] = (char)('0' + v % 10); v /= 10; }

    int hl = (int)sizeof R_NUM_HEAD - 1;
    memcpy(out, R_NUM_HEAD, (size_t)hl);
    /* Content-Length: 1 or 2 digits (max 10), then \r\n\r\n */
    if (nd >= 10) {
        out[hl] = '1'; out[hl+1] = '0'; out[hl+2] = '\r'; out[hl+3] = '\n';
        out[hl+4] = '\r'; out[hl+5] = '\n';
        hl += 6;
    } else {
        out[hl] = (char)('0' + nd); out[hl+1] = '\r'; out[hl+2] = '\n';
        out[hl+3] = '\r'; out[hl+4] = '\n';
        hl += 5;
    }
    for (int i = 0; i < nd; i++) out[hl + i] = digs[nd - 1 - i];
    return try_write(c->fd, out, hl + nd);
}

static void handle_read(int epfd, conn_t *c)
{
    for (;;) {
        if (c->buflen >= RBUF) { drop_conn(epfd, c); return; }
        ssize_t r = read(c->fd, c->buf + c->buflen, (size_t)(RBUF - c->buflen));
        if (r == 0) { drop_conn(epfd, c); return; }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            drop_conn(epfd, c); return;
        }
        c->buflen += (int)r;

        for (;;) {
            /* complete request? (ends with \r\n\r\n) */
            char *end = NULL;
            int limit = c->buflen < 512 ? c->buflen : 512;
            for (int j = 0; j + 3 < limit; j++) {
                if (c->buf[j] == '\r' && c->buf[j+1] == '\n' &&
                    c->buf[j+2] == '\r' && c->buf[j+3] == '\n') {
                    end = c->buf + j + 4;
                    break;
                }
            }
            if (!end) {
                if (limit == 512) { drop_conn(epfd, c); return; }
                break;
            }
            char *line_end = memchr(c->buf, '\n', (size_t)(end - c->buf));
            if (!line_end || line_end == c->buf || *(line_end - 1) != '\r') {
                drop_conn(epfd, c); return;
            }

            int verdict = 0; /* 0 = answered, 1 = bad path, 2 = bad method */

            if (c->buflen >= 14 && memcmp(c->buf, "GET /", 5) == 0) {
                const char *p = c->buf + 5;
                const char *q = p;
                uint64_t n = 0;
                int overflow = 0;
                while (q < line_end - 1 && *q >= '0' && *q <= '9') {
                    if (n <= 2147483647ULL) n = n * 10 + (uint64_t)(*q - '0');
                    else overflow = 1;
                    q++;
                }
                int ok = (q != p) && (q + 10 == line_end) &&
                         (q[0] == ' ') && (memcmp(q + 1, "HTTP/1.", 7) == 0) &&
                         (q[8] >= '0') && (q[8] <= '9') &&
                         !overflow && (n <= 2147483647ULL) && (n != 0);
                if (!ok) {
                    verdict = 1;
                } else {
                    uint32_t u = (uint32_t)n;
                    int wk = 0;
                    if (u % 15 == 0) wk = 15;
                    else if (u % 3 == 0) wk = 3;
                    else if (u % 5 == 0) wk = 5;
                    if (respond(c, u, wk) < 0) { drop_conn(epfd, c); return; }
                }
            } else if (c->buflen >= 5 && memcmp(c->buf, "GET ", 4) != 0) {
                verdict = 2;
            } else {
                verdict = 1;
            }

            if (verdict == 1 && try_write(c->fd, R_404, (int)sizeof R_404 - 1) < 0) {
                drop_conn(epfd, c); return;
            }
            if (verdict == 2 && try_write(c->fd, R_405, (int)sizeof R_405 - 1) < 0) {
                drop_conn(epfd, c); return;
            }

            int consumed = (int)(end - c->buf);
            memmove(c->buf, c->buf + consumed, (size_t)(c->buflen - consumed));
            c->buflen -= consumed;
        }
    }
}

static void *worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    int epfd = g_epfds[idx];

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)g_port_val);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        return NULL;
    }
    listen(lfd, 8192);
    set_nonblock(lfd);

    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    struct epoll_event evs[MAXEV];
    for (;;) {
        int n = epoll_wait(epfd, evs, MAXEV, -1);
        for (int i = 0; i < n; i++) {
            if (evs[i].data.ptr == NULL) {
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
                    if (cfd < 0) break;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    conn_t *c = calloc(1, sizeof *c);
                    if (!c) { close(cfd); continue; }
                    c->fd = cfd;
                    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = c };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                handle_read(epfd, evs[i].data.ptr);
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *pe = getenv("PORT");
    if (pe && atoi(pe) > 0) g_port_val = atoi(pe);
    if (argc > 1 && atoi(argv[1]) > 0) g_port_val = atoi(argv[1]);

    for (int t = 0; t < THREADS; t++) g_epfds[t] = epoll_create1(0);

    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++)
        pthread_create(&th[t], NULL, worker, (void *)(intptr_t)t);
    for (int t = 0; t < THREADS; t++)
        pthread_join(th[t], NULL);
    return 0;
}
