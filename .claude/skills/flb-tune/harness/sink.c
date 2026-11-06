/*
 * Fake forward backend. Speaks enough of the Fluentd forward protocol to count
 * records exactly, answer require_ack_response and model a slow engine.
 *
 *   sink <port> <stats.json> <sample.bin> <delay_ms>
 *
 * Handles Message, Forward, PackedForward and CompressedPackedForward. Records
 * inside compressed payloads are counted from the "size" option, because
 * inflating them here would steal CPU from the system under test.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUF_START (1 << 20)
#define MAX_DEPTH 32

static atomic_ullong g_records, g_bytes, g_msgs, g_acks, g_unknown, g_conns;
static atomic_int g_sampled;
static const char *g_stats, *g_sample;
static int g_delay_ms;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t) ((p[0] << 8) | p[1]);
}

/* Reads a container or blob header. Returns bytes consumed, 0 if incomplete. */
static size_t header(const uint8_t *p, const uint8_t *e, int *kind, uint64_t *n)
{
    size_t avail = (size_t) (e - p);
    uint8_t c;

    if (avail < 1) {
        return 0;
    }
    c = p[0];
    *n = 0;

    if (c <= 0x7f || c >= 0xe0 || c == 0xc0 || c == 0xc2 || c == 0xc3) {
        *kind = 'i';
        return 1;
    }
    if (c >= 0x80 && c <= 0x8f) { *kind = 'm'; *n = c & 0x0f; return 1; }
    if (c >= 0x90 && c <= 0x9f) { *kind = 'a'; *n = c & 0x0f; return 1; }
    if (c >= 0xa0 && c <= 0xbf) { *kind = 's'; *n = c & 0x1f; return 1; }

    switch (c) {
    case 0xc4: case 0xd9:
        if (avail < 2) return 0;
        *kind = c == 0xc4 ? 'b' : 's'; *n = p[1]; return 2;
    case 0xc5: case 0xda:
        if (avail < 3) return 0;
        *kind = c == 0xc5 ? 'b' : 's'; *n = be16(p + 1); return 3;
    case 0xc6: case 0xdb:
        if (avail < 5) return 0;
        *kind = c == 0xc6 ? 'b' : 's'; *n = be32(p + 1); return 5;
    case 0xc7:
        if (avail < 3) return 0;
        *kind = 'x'; *n = (uint64_t) p[1] + 1; return 2;
    case 0xc8:
        if (avail < 4) return 0;
        *kind = 'x'; *n = (uint64_t) be16(p + 1) + 1; return 3;
    case 0xc9:
        if (avail < 6) return 0;
        *kind = 'x'; *n = (uint64_t) be32(p + 1) + 1; return 5;
    case 0xca: *kind = 'f'; *n = 4; return 1;
    case 0xcb: *kind = 'f'; *n = 8; return 1;
    case 0xcc: case 0xd0: *kind = 'f'; *n = 1; return 1;
    case 0xcd: case 0xd1: *kind = 'f'; *n = 2; return 1;
    case 0xce: case 0xd2: *kind = 'f'; *n = 4; return 1;
    case 0xcf: case 0xd3: *kind = 'f'; *n = 8; return 1;
    case 0xd4: *kind = 'x'; *n = 2; return 1;
    case 0xd5: *kind = 'x'; *n = 3; return 1;
    case 0xd6: *kind = 'x'; *n = 5; return 1;
    case 0xd7: *kind = 'x'; *n = 9; return 1;
    case 0xd8: *kind = 'x'; *n = 17; return 1;
    case 0xdc:
        if (avail < 3) return 0;
        *kind = 'a'; *n = be16(p + 1); return 3;
    case 0xdd:
        if (avail < 5) return 0;
        *kind = 'a'; *n = be32(p + 1); return 5;
    case 0xde:
        if (avail < 3) return 0;
        *kind = 'm'; *n = be16(p + 1); return 3;
    case 0xdf:
        if (avail < 5) return 0;
        *kind = 'm'; *n = be32(p + 1); return 5;
    }
    *kind = '?';
    return 0;
}

/* Skips one object. Returns the next position, NULL if the input is incomplete. */
static const uint8_t *skip(const uint8_t *p, const uint8_t *e, int depth)
{
    int kind = 0;
    uint64_t n = 0, i, items;
    size_t h;

    if (depth > MAX_DEPTH) {
        return NULL;
    }
    h = header(p, e, &kind, &n);
    if (h == 0) {
        return NULL;
    }
    p += h;

    if (kind == 'i') {
        return p;
    }
    if (kind == 'a' || kind == 'm') {
        items = kind == 'm' ? n * 2 : n;
        for (i = 0; i < items; i++) {
            p = skip(p, e, depth + 1);
            if (p == NULL) {
                return NULL;
            }
        }
        return p;
    }
    if ((uint64_t) (e - p) < n) {
        return NULL;
    }
    return p + n;
}

static void save_sample(const uint8_t *p, size_t len)
{
    int expected = 0;
    FILE *f;

    if (!atomic_compare_exchange_strong(&g_sampled, &expected, 1)) {
        return;
    }
    f = fopen(g_sample, "wb");
    if (f != NULL) {
        fwrite(p, 1, len, f);
        fclose(f);
    }
}

/* Reads the option map: chunk token, "size" and whether the payload is gzip. */
static void read_options(const uint8_t *p, const uint8_t *e, const uint8_t **chunk,
                         size_t *chunk_len, long long *size, int *compressed)
{
    int kind = 0, vkind = 0;
    uint64_t n = 0, kn = 0, vn = 0, i;
    size_t h;
    const uint8_t *key;

    h = header(p, e, &kind, &n);
    if (h == 0 || kind != 'm') {
        return;
    }
    p += h;
    for (i = 0; i < n; i++) {
        h = header(p, e, &kind, &kn);
        if (h == 0) {
            return;
        }
        key = p + h;
        p = skip(p, e, 0);
        if (p == NULL) {
            return;
        }
        if (kind == 's' && kn == 5 && memcmp(key, "chunk", 5) == 0) {
            h = header(p, e, &vkind, &vn);
            if (h != 0 && vkind == 's') {
                *chunk = p + h;
                *chunk_len = (size_t) vn;
            }
        }
        else if (kind == 's' && kn == 10 && memcmp(key, "compressed", 10) == 0) {
            *compressed = 1;
        }
        else if (kind == 's' && kn == 4 && memcmp(key, "size", 4) == 0) {
            uint8_t c = p[0];
            if (c <= 0x7f) *size = c;
            else if (c == 0xcc) *size = p[1];
            else if (c == 0xcd) *size = be16(p + 1);
            else if (c == 0xce) *size = be32(p + 1);
        }
        p = skip(p, e, 0);
        if (p == NULL) {
            return;
        }
    }
}

static void send_ack(int fd, const uint8_t *chunk, size_t len)
{
    uint8_t out[512];
    size_t o = 0;

    if (len > sizeof(out) - 16) {
        return;
    }
    out[o++] = 0x81;
    out[o++] = 0xa3;
    memcpy(out + o, "ack", 3);
    o += 3;
    if (len < 32) {
        out[o++] = (uint8_t) (0xa0 | len);
    }
    else {
        out[o++] = 0xd9;
        out[o++] = (uint8_t) len;
    }
    memcpy(out + o, chunk, len);
    o += len;
    if (write(fd, out, o) == (ssize_t) o) {
        atomic_fetch_add(&g_acks, 1);
    }
}

/* Handles one full top-level message. Returns its end, NULL when incomplete. */
static const uint8_t *message(int fd, const uint8_t *p, const uint8_t *e)
{
    int kind = 0, k2 = 0;
    uint64_t n = 0, n2 = 0;
    size_t h, chunk_len = 0;
    long long size = -1, count = 0;
    int compressed = 0;
    const uint8_t *end, *q, *body, *chunk = NULL, *r, *be;
    struct timespec ts;

    end = skip(p, e, 0);
    if (end == NULL) {
        return NULL;
    }

    h = header(p, e, &kind, &n);
    if (h == 0 || kind != 'a' || n < 2) {
        return end;
    }
    q = skip(p + h, e, 0);            /* tag */
    if (q == NULL) {
        return end;
    }
    h = header(q, e, &k2, &n2);
    body = q + h;
    r = skip(q, e, 0);                /* entries, or time in Message mode */
    if (r == NULL) {
        return end;
    }

    if (k2 == 'a') {                  /* Forward */
        if (n >= 3) {
            read_options(r, end, &chunk, &chunk_len, &size, &compressed);
        }
        count = (long long) n2;
        if (n2 > 0) {
            be = skip(body, end, 0);
            if (be != NULL) {
                save_sample(body, (size_t) (be - body));
            }
        }
    }
    else if (k2 == 'b' || k2 == 's') { /* PackedForward */
        if (n >= 3) {
            read_options(r, end, &chunk, &chunk_len, &size, &compressed);
        }
        if (compressed) {
            count = size;
            if (count < 0) {
                atomic_fetch_add(&g_unknown, 1);
                count = 0;
            }
        }
        else if (size >= 0) {
            count = size;
            be = skip(body, r, 0);
            if (be != NULL) {
                save_sample(body, (size_t) (be - body));
            }
        }
        else {
            for (q = body; q < r; ) {
                be = skip(q, r, 0);
                if (be == NULL) {
                    atomic_fetch_add(&g_unknown, 1);
                    break;
                }
                if (count == 0) {
                    save_sample(q, (size_t) (be - q));
                }
                q = be;
                count++;
            }
        }
    }
    else {                             /* Message */
        count = 1;
        if (n >= 4) {
            q = skip(r, end, 0);
            if (q != NULL) {
                read_options(q, end, &chunk, &chunk_len, &size, &compressed);
            }
        }
    }

    atomic_fetch_add(&g_records, (unsigned long long) count);
    atomic_fetch_add(&g_msgs, 1);

    if (g_delay_ms > 0) {
        ts.tv_sec = g_delay_ms / 1000;
        ts.tv_nsec = (long) (g_delay_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    if (chunk != NULL) {
        send_ack(fd, chunk, chunk_len);
    }
    return end;
}

static void *conn_thread(void *arg)
{
    int fd = (int) (intptr_t) arg;
    size_t cap = BUF_START, len = 0;
    uint8_t *buf = malloc(cap);
    const uint8_t *p, *n;
    ssize_t r;

    atomic_fetch_add(&g_conns, 1);
    while (buf != NULL) {
        if (cap - len < 65536) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (nb == NULL) {
                break;
            }
            buf = nb;
            cap *= 2;
        }
        r = read(fd, buf + len, cap - len);
        if (r <= 0) {
            break;
        }
        atomic_fetch_add(&g_bytes, (unsigned long long) r);
        len += (size_t) r;

        p = buf;
        while ((n = message(fd, p, buf + len)) != NULL) {
            p = n;
            if (p == buf + len) {
                break;
            }
        }
        len -= (size_t) (p - buf);
        if (len > 0 && p != buf) {
            memmove(buf, p, len);
        }
    }
    free(buf);
    close(fd);
    return NULL;
}

static void *stats_thread(void *arg)
{
    char tmp[512];
    FILE *f;
    struct timespec ts = {0, 200000000L};

    (void) arg;
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_stats);
    for (;;) {
        f = fopen(tmp, "w");
        if (f != NULL) {
            fprintf(f, "{\"records\":%llu,\"bytes\":%llu,\"msgs\":%llu,\"acks\":%llu,"
                       "\"unknown\":%llu,\"conns\":%llu}\n",
                    (unsigned long long) g_records, (unsigned long long) g_bytes,
                    (unsigned long long) g_msgs, (unsigned long long) g_acks,
                    (unsigned long long) g_unknown, (unsigned long long) g_conns);
            fclose(f);
            rename(tmp, g_stats);
        }
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int s, one = 1;
    struct sockaddr_in a;
    pthread_t t;

    if (argc < 5) {
        fprintf(stderr, "usage: sink port stats.json sample.bin delay_ms\n");
        return 2;
    }
    g_stats = argv[2];
    g_sample = argv[3];
    g_delay_ms = atoi(argv[4]);

    s = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) atoi(argv[1]));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (void *) &a, sizeof(a)) != 0 || listen(s, 1024) != 0) {
        perror("bind/listen");
        return 1;
    }
    pthread_create(&t, NULL, stats_thread, NULL);
    pthread_detach(t);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_create(&t, NULL, conn_thread, (void *) (intptr_t) fd);
        pthread_detach(t);
    }
}
