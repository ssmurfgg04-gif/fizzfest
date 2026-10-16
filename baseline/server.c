/*
 * fizzbuzz-arena baseline server
 *
 * The yardstick. One thread, plain epoll, one read and one write syscall
 * per request, sprintf for the answer. It is honest, boring, and beatable.
 *
 * Protocol (see README.md):
 *   GET /N        200, body = fizz | buzz | fizzbuzz | N  (exact bytes)
 *   GET anything  404
 *   other methods 405
 *   HTTP/1.1 keep-alive, always sends Content-Length.
 *
 * Build:  cc -O2 -o server server.c
 * Usage:  ./server 8080
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAXEV 1024
#define RBUF  4096

typedef struct {
    int fd;
    char buf[RBUF];
    int buflen;
} conn_t;

static int lfd;

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void make_response(int hit, uint32_t n, char *out, int *outlen)
{
    const char *body;
    char numbuf[11];

    if (hit) {
        if (n % 15 == 0) body = "fizzbuzz";
        else if (n % 3 == 0) body = "fizz";
        else body = "buzz";
    } else {
        snprintf(numbuf, sizeof numbuf, "%u", n);
        body = numbuf;
    }
    *outlen = sprintf(out,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        strlen(body), body);
}

static void handle_read(int epfd, conn_t *c)
{
    char resp[256];

    for (;;) {
        if (c->buflen >= RBUF) goto drop;
        ssize_t r = read(c->fd, c->buf + c->buflen, (size_t)(RBUF - c->buflen));
        if (r == 0) goto drop;
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            goto drop;
        }
        c->buflen += (int)r;

        /* process every complete request in the buffer */
        for (;;) {
            /* need the full header block: ends with \r\n\r\n */
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
                if (limit == 512) goto drop;   /* header monster, bye */
                break;                          /* need more bytes */
            }

            char *line_end = memchr(c->buf, '\n', (size_t)(end - c->buf));
            if (!line_end || line_end == c->buf || *(line_end - 1) != '\r')
                goto drop;

            if (c->buflen >= 14 && memcmp(c->buf, "GET /", 5) == 0) {
                char *p = c->buf + 5;
                uint64_t n = 0;
                char *q = p;
                int overflow = 0;
                while (q < line_end - 1 && *q >= '0' && *q <= '9') {
                    if (n <= 2147483647ULL) {
                        n = n * 10 + (uint64_t)(*q - '0');
                    } else {
                        overflow = 1;
                    }
                    q++;
                }
                /* digits then " HTTP/1.x" up to the end of the request line */
                int ok = (q != p) && (q + 10 == line_end) &&
                         (q[0] == ' ') && (memcmp(q + 1, "HTTP/1.", 7) == 0) &&
                         (q[8] >= '0') && (q[8] <= '9') &&
                         !overflow && (n <= 2147483647ULL) && (n != 0);
                if (ok) {
                    int hit = (n % 3 == 0 || n % 5 == 0);
                    int len;
                    make_response(hit, (uint32_t)n, resp, &len);
                    if (write(c->fd, resp, (size_t)len) < 0) goto drop;
                } else {
                    int len = sprintf(resp,
                        "HTTP/1.1 404 Not Found\r\n"
                        "Content-Length: 0\r\n\r\n");
                    if (write(c->fd, resp, (size_t)len) < 0) goto drop;
                }
            } else if (c->buflen >= 5 && memcmp(c->buf, "GET ", 4) != 0) {
                /* not a GET at all */
                int len = sprintf(resp,
                    "HTTP/1.1 405 Method Not Allowed\r\n"
                    "Content-Length: 0\r\n\r\n");
                if (write(c->fd, resp, (size_t)len) < 0) goto drop;
            } else {
                /* GET of a junk path */
                int len = sprintf(resp,
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 0\r\n\r\n");
                if (write(c->fd, resp, (size_t)len) < 0) goto drop;
            }
            /* consume the whole request (request line + headers) */
            int consumed = (int)(end - c->buf);
            memmove(c->buf, c->buf + consumed, (size_t)(c->buflen - consumed));
            c->buflen -= consumed;
        }
    }
    return;

drop:
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        return 1;
    }
    listen(lfd, 4096);
    set_nonblock(lfd);

    int epfd = epoll_create1(0);
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
                    c->fd = cfd;
                    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = c };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                handle_read(epfd, evs[i].data.ptr);
            }
        }
    }
}
