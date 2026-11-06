/*
 * Load generator.
 *
 *   gen tcp|udp host port json|none sockets total_rate seconds size [sample_file]
 *
 * total_rate 0 sends as fast as possible. Each socket has its own 4-tuple,
 * which is what SO_REUSEPORT hashes on, so `sockets` models the sender count.
 * sample_file replaces the synthetic record with real lines, one per record.
 * Prints one JSON line with the totals.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TCP_LINES 64
#define UDP_BATCH 32

static char **g_lines;
static size_t *g_lens;
static int g_nlines, g_tcp, g_port, g_seconds;
static double g_rate_each;
static const char *g_host;
static atomic_ullong g_sent;
static atomic_int g_failed, g_stalled;

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

static void add_line(const char *s, size_t len)
{
    g_lines = realloc(g_lines, sizeof(char *) * (size_t) (g_nlines + 1));
    g_lens = realloc(g_lens, sizeof(size_t) * (size_t) (g_nlines + 1));
    g_lines[g_nlines] = malloc(len);
    memcpy(g_lines[g_nlines], s, len);
    g_lens[g_nlines] = len;
    g_nlines++;
}

static void synth(int json, int size)
{
    char *b = malloc((size_t) size + 1);
    int n;

    memset(b, 'x', (size_t) size);
    if (json) {
        n = snprintf(b, (size_t) size,
                     "{\"@timestamp\":\"2026-11-06T10:15:30.123Z\",\"host\":\"web-01.example.internal\","
                     "\"app\":\"payment-api\",\"level\":\"INFO\",\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\","
                     "\"span_id\":\"00f067aa0ba902b7\",\"user_id\":123456,\"status\":200,\"latency_ms\":12.7,"
                     "\"path\":\"/api/v1/orders/9876543\",\"msg\":\"");
        b[n] = 'x';
        memcpy(b + size - 3, "\"}\n", 3);
    }
    else {
        n = snprintf(b, (size_t) size,
                     "2026-11-06T10:15:30.123Z web-01 payment-api INFO status=200 latency_ms=12.7 msg=");
        b[n] = 'x';
        b[size - 1] = '\n';
    }
    add_line(b, (size_t) size);
    free(b);
}

static int load(const char *path)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t r;

    if (f == NULL) {
        perror(path);
        return -1;
    }
    while ((r = getline(&line, &cap, f)) > 0 && g_nlines < 4096) {
        if (line[r - 1] != '\n') {
            line = realloc(line, (size_t) r + 2);
            line[r++] = '\n';
        }
        add_line(line, (size_t) r);
    }
    free(line);
    fclose(f);
    return g_nlines > 0 ? 0 : -1;
}

static void pace(double *next, int n)
{
    double d;
    struct timespec t;

    if (g_rate_each <= 0) {
        return;
    }
    *next += (double) n / g_rate_each;
    d = *next - now();
    if (d > 0) {
        t.tv_sec = (time_t) d;
        t.tv_nsec = (long) ((d - (double) t.tv_sec) * 1e9);
        nanosleep(&t, NULL);
    }
}

static void tcp_loop(int fd, double start)
{
    size_t total = 0, off;
    int i;
    char *buf;
    double next = start;
    ssize_t r;

    for (i = 0; i < TCP_LINES; i++) {
        total += g_lens[i % g_nlines];
    }
    buf = malloc(total);
    for (i = 0, off = 0; i < TCP_LINES; i++) {
        memcpy(buf + off, g_lines[i % g_nlines], g_lens[i % g_nlines]);
        off += g_lens[i % g_nlines];
    }
    while (now() - start < g_seconds) {
        for (off = 0; off < total; off += (size_t) r) {
            r = send(fd, buf + off, total - off, MSG_NOSIGNAL);
            if (r <= 0) {
                /* A send timeout is backpressure, anything else a dropped connection. */
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    atomic_fetch_add(&g_stalled, 1);
                }
                else {
                    atomic_fetch_add(&g_failed, 1);
                }
                atomic_fetch_add(&g_sent, off / g_lens[0]);
                free(buf);
                return;
            }
        }
        atomic_fetch_add(&g_sent, TCP_LINES);
        pace(&next, TCP_LINES);
    }
    free(buf);
}

static void udp_loop(int fd, double start)
{
    struct mmsghdr m[UDP_BATCH];
    struct iovec v[UDP_BATCH];
    int i, r;
    double next = start;

    memset(m, 0, sizeof(m));
    for (i = 0; i < UDP_BATCH; i++) {
        v[i].iov_base = g_lines[i % g_nlines];
        v[i].iov_len = g_lens[i % g_nlines];
        m[i].msg_hdr.msg_iov = &v[i];
        m[i].msg_hdr.msg_iovlen = 1;
    }
    while (now() - start < g_seconds) {
        r = sendmmsg(fd, m, UDP_BATCH, 0);
        if (r > 0) {
            atomic_fetch_add(&g_sent, (unsigned long long) r);
        }
        pace(&next, UDP_BATCH);
    }
}

static void *worker(void *arg)
{
    struct sockaddr_in sa;
    struct timeval timeout = {4, 0};
    int fd;
    double start;

    (void) arg;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t) g_port);
    inet_pton(AF_INET, g_host, &sa.sin_addr);
    fd = socket(AF_INET, g_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (void *) &sa, sizeof(sa)) != 0) {
        atomic_fetch_add(&g_failed, 1);
        return NULL;
    }
    start = now();
    if (g_tcp) {
        tcp_loop(fd, start);
    }
    else {
        udp_loop(fd, start);
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int n, i;
    pthread_t *t;
    double rate, s;

    if (argc < 9) {
        fprintf(stderr, "usage: gen tcp|udp host port json|none sockets rate seconds size [sample]\n");
        return 2;
    }
    g_tcp = strcmp(argv[1], "tcp") == 0;
    g_host = argv[2];
    g_port = atoi(argv[3]);
    n = atoi(argv[5]);
    rate = atof(argv[6]);
    g_seconds = atoi(argv[7]);
    g_rate_each = rate > 0 ? rate / n : 0;

    if (argc > 9 && argv[9][0] != '\0') {
        if (load(argv[9]) != 0) {
            return 1;
        }
    }
    else {
        synth(strcmp(argv[4], "json") == 0, atoi(argv[8]));
    }

    t = calloc((size_t) n, sizeof(*t));
    s = now();
    for (i = 0; i < n; i++) {
        pthread_create(&t[i], NULL, worker, NULL);
    }
    for (i = 0; i < n; i++) {
        pthread_join(t[i], NULL);
    }
    printf("{\"sent\":%llu,\"rate\":%.0f,\"failed\":%d,\"stalled\":%d}\n",
           (unsigned long long) g_sent, (double) g_sent / (now() - s), (int) g_failed,
           (int) g_stalled);
    return 0;
}
