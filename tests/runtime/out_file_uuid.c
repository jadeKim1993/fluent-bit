/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "flb_tests_runtime.h"
#include <fluent-bit.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_time.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Test functions */
void flb_test_uuid_file_plain(void);
void flb_test_uuid_file_default_format(void);
void flb_test_uuid_file_prefix_extension(void);
void flb_test_uuid_file_names_sort_by_time(void);
void flb_test_uuid_file_empty_chunk(void);
void flb_test_uuid_file_stale_tmp_cleanup(void);
void flb_test_uuid_file_retry_on_failure(void);
void flb_test_uuid_file_mkdir(void);
void flb_test_uuid_file_invalid_config(void);
void flb_test_uuid_file_multi_process(void);
void flb_test_uuid_file_path_trailing_slash(void);
void flb_test_uuid_file_empty_extension(void);
void flb_test_uuid_file_csv_header_per_file(void);
void flb_test_uuid_file_two_instances_same_dir(void);
void flb_test_uuid_file_workers_concurrent(void);
void flb_test_uuid_file_multi_process_mkdir(void);

/* Test list */
TEST_LIST = {
    {"plain",               flb_test_uuid_file_plain},
    {"default_format",      flb_test_uuid_file_default_format},
    {"prefix_extension",    flb_test_uuid_file_prefix_extension},
    {"names_sort_by_time",  flb_test_uuid_file_names_sort_by_time},
    {"empty_chunk",         flb_test_uuid_file_empty_chunk},
    {"stale_tmp_cleanup",   flb_test_uuid_file_stale_tmp_cleanup},
    {"retry_on_failure",    flb_test_uuid_file_retry_on_failure},
    {"mkdir",               flb_test_uuid_file_mkdir},
    {"invalid_config",      flb_test_uuid_file_invalid_config},
    {"multi_process",       flb_test_uuid_file_multi_process},
    {"path_trailing_slash", flb_test_uuid_file_path_trailing_slash},
    {"empty_extension",     flb_test_uuid_file_empty_extension},
    {"csv_header_per_file", flb_test_uuid_file_csv_header_per_file},
    {"two_instances_same_dir", flb_test_uuid_file_two_instances_same_dir},
    {"workers_concurrent",  flb_test_uuid_file_workers_concurrent},
    {"multi_process_mkdir", flb_test_uuid_file_multi_process_mkdir},
    {NULL, NULL}
};

#define TEST_FLUSH          "0.2"
#define TEST_TIMEOUT_MS     10000
#define TEST_POLL_MS        50
#define UUID_LEN            36
#define MAX_FILES           4096

/* multi process test layout */
#define MP_PROCS            6
#define MP_INPUTS           16
#define MP_ROUNDS           4

struct file_list {
    int count;
    char *names[MAX_FILES];
};

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char * const *) a, *(char * const *) b);
}

static int has_suffix(const char *name, const char *suffix)
{
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix);

    return len >= suffix_len && strcmp(name + len - suffix_len, suffix) == 0;
}

/* sorted list of the entries of 'dir' ending with 'suffix' */
static void list_files(const char *dir, const char *suffix, struct file_list *list)
{
    DIR *d;
    struct dirent *ent;

    list->count = 0;
    d = opendir(dir);
    if (d == NULL) {
        return;
    }

    while ((ent = readdir(d)) != NULL && list->count < MAX_FILES) {
        if (ent->d_name[0] == '.' || !has_suffix(ent->d_name, suffix)) {
            continue;
        }
        list->names[list->count++] = strdup(ent->d_name);
    }
    closedir(d);

    qsort(list->names, list->count, sizeof(char *), cmp_names);
}

static void free_files(struct file_list *list)
{
    int i;

    for (i = 0; i < list->count; i++) {
        free(list->names[i]);
    }
    list->count = 0;
}

static int count_files(const char *dir, const char *suffix)
{
    int count;
    struct file_list list;

    list_files(dir, suffix, &list);
    count = list.count;
    free_files(&list);

    return count;
}

static int wait_for_files(const char *dir, const char *suffix, int expected)
{
    int waited = 0;

    while (waited < TEST_TIMEOUT_MS) {
        if (count_files(dir, suffix) >= expected) {
            return 0;
        }
        flb_time_msleep(TEST_POLL_MS);
        waited += TEST_POLL_MS;
    }

    return -1;
}

static char *read_file(const char *dir, const char *name)
{
    long size;
    char path[PATH_MAX];
    char *buf;
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    buf = calloc(1, size + 1);
    if (buf != NULL && fread(buf, 1, size, fp) != (size_t) size) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);

    return buf;
}

static void remove_dir(const char *dir)
{
    char path[PATH_MAX];
    DIR *d;
    struct dirent *ent;

    d = opendir(dir);
    if (d == NULL) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static void make_test_dir(char *out, size_t size, const char *name)
{
    snprintf(out, size, "/tmp/flb-rt-out-file-uuid-%s-%d", name, (int) getpid());
    remove_dir(out);
    mkdir(out, 0755);
}

/* <prefix><uuidv7><ext>: lowercase hex, version 7, RFC 9562 variant */
static int check_uuid_name(const char *name, const char *prefix, const char *ext)
{
    int i;
    const char *id;
    size_t prefix_len = strlen(prefix);

    if (strncmp(name, prefix, prefix_len) != 0 ||
        strlen(name) != prefix_len + UUID_LEN + strlen(ext) ||
        strcmp(name + prefix_len + UUID_LEN, ext) != 0) {
        return -1;
    }

    id = name + prefix_len;
    for (i = 0; i < UUID_LEN; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') {
                return -1;
            }
        }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return -1;
        }
    }

    if (id[14] != '7' || strchr("89ab", id[19]) == NULL) {
        return -1;
    }

    return 0;
}

static flb_ctx_t *create_ctx(const char *dir, int *in_ffd, int *out_ffd)
{
    flb_ctx_t *ctx;

    ctx = flb_create();
    flb_service_set(ctx, "flush", TEST_FLUSH, "grace", "1",
                    "log_level", "error", NULL);

    *in_ffd = flb_input(ctx, (char *) "lib", NULL);
    flb_input_set(ctx, *in_ffd, "tag", "app.log", NULL);

    *out_ffd = flb_output(ctx, (char *) "file", NULL);
    flb_output_set(ctx, *out_ffd, "match", "*", "path", dir,
                   "uuid_file", "on", NULL);

    return ctx;
}

static int push(flb_ctx_t *ctx, int in_ffd, const char *json)
{
    return flb_lib_push(ctx, in_ffd, (char *) json, strlen(json));
}

void flb_test_uuid_file_plain(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char *content;
    struct file_list list;
    struct stat st;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "plain");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_output_set(ctx, out_ffd, "format", "plain", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    /* both records are pushed in one call, so they share one chunk */
    push(ctx, in_ffd,
         "[1759823400, {\"event\":\"LOGIN\",\"user\":\"kim\"}]"
         "[1759823401, {\"event\":\"PAYMENT\",\"nested\":{\"k\":\"v\"}}]");

    ret = wait_for_files(dir, ".log", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    list_files(dir, ".log", &list);
    TEST_CHECK(list.count == 1);
    TEST_MSG("files: %d", list.count);
    TEST_CHECK(count_files(dir, ".tmp") == 0);

    if (list.count == 1) {
        TEST_CHECK(check_uuid_name(list.names[0], "", ".log") == 0);
        TEST_MSG("name: %s", list.names[0]);

        content = read_file(dir, list.names[0]);
        TEST_CHECK(content != NULL);
        if (content != NULL) {
            TEST_CHECK(strcmp(content,
                              "{\"event\":\"LOGIN\",\"user\":\"kim\"}\n"
                              "{\"event\":\"PAYMENT\",\"nested\":{\"k\":\"v\"}}\n") == 0);
            TEST_MSG("content: %s", content);
            free(content);
        }

        /* published files are never readable by others */
        snprintf(path, sizeof(path), "%s/%s", dir, list.names[0]);
        ret = stat(path, &st);
        TEST_CHECK(ret == 0 && (st.st_mode & 0007) == 0);
    }

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_default_format(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    char *content;
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "default");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {\"msg\":\"hello\"}]");

    ret = wait_for_files(dir, ".log", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    list_files(dir, ".log", &list);
    TEST_CHECK(list.count == 1);
    if (list.count == 1) {
        content = read_file(dir, list.names[0]);
        TEST_CHECK(content != NULL);
        if (content != NULL) {
            TEST_CHECK(strcmp(content,
                              "app.log: [1759823400.000000000, {\"msg\":\"hello\"}]\n") == 0);
            TEST_MSG("content: %s", content);
            free(content);
        }
    }

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_prefix_extension(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "prefix");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    /* the leading dot of the extension is added when missing */
    flb_output_set(ctx, out_ffd, "uuid_file_prefix", "relay-",
                   "uuid_file_extension", "jsonl", "uuid_file_fsync", "off", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {\"msg\":\"hello\"}]");

    ret = wait_for_files(dir, ".jsonl", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    list_files(dir, "", &list);
    TEST_CHECK(list.count == 1);
    if (list.count == 1) {
        TEST_CHECK(check_uuid_name(list.names[0], "relay-", ".jsonl") == 0);
        TEST_MSG("name: %s", list.names[0]);
    }

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_names_sort_by_time(void)
{
    int i;
    int ret;
    int seq;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    char json[128];
    char *content;
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "sort");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_output_set(ctx, out_ffd, "format", "plain", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    /* one flush per record: wait for each file before pushing the next */
    for (i = 0; i < 5; i++) {
        snprintf(json, sizeof(json), "[1759823400, {\"seq\":%d}]", i);
        push(ctx, in_ffd, json);
        ret = wait_for_files(dir, ".log", i + 1);
        TEST_CHECK(ret == 0);
    }

    flb_stop(ctx);
    flb_destroy(ctx);

    /* sorting the names must give the creation order */
    list_files(dir, ".log", &list);
    TEST_CHECK(list.count == 5);
    for (i = 0; i < list.count; i++) {
        content = read_file(dir, list.names[i]);
        TEST_CHECK(content != NULL);
        if (content == NULL) {
            continue;
        }
        seq = -1;
        sscanf(content, "{\"seq\":%d}", &seq);
        TEST_CHECK(seq == i);
        TEST_MSG("file %d (%s) has seq %d", i, list.names[i], seq);
        free(content);
    }

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_empty_chunk(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "empty");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    /* csv writes nothing for an empty record */
    flb_output_set(ctx, out_ffd, "format", "csv", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {}]");
    flb_time_msleep(1500);

    TEST_CHECK(count_files(dir, "") == 0);
    TEST_MSG("files after empty chunk: %d", count_files(dir, ""));

    push(ctx, in_ffd, "[1759823400, {\"msg\":\"hello\"}]");
    ret = wait_for_files(dir, ".log", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    TEST_CHECK(count_files(dir, ".log") == 1);
    TEST_CHECK(count_files(dir, ".tmp") == 0);

    remove_dir(dir);
}

static void create_file(const char *dir, const char *name, time_t age)
{
    char path[PATH_MAX];
    FILE *fp;
    struct timeval times[2];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fp = fopen(path, "w");
    if (fp == NULL) {
        return;
    }
    fputs("partial", fp);
    fclose(fp);

    gettimeofday(&times[0], NULL);
    times[0].tv_sec -= age;
    times[1] = times[0];
    utimes(path, times);
}

static int file_exists(const char *dir, const char *name)
{
    char path[PATH_MAX];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

void flb_test_uuid_file_stale_tmp_cleanup(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    flb_ctx_t *ctx;
    const char *old_tmp = "01890a5d-ac96-774b-bcce-b302099a8057.log.tmp";
    const char *new_tmp = "01890a5d-ac96-774b-bcce-b302099a8058.log.tmp";
    const char *foreign_tmp = "other.tmp";
    const char *complete = "01890a5d-ac96-774b-bcce-b302099a8059.log";

    make_test_dir(dir, sizeof(dir), "stale");

    create_file(dir, old_tmp, 3600);
    create_file(dir, new_tmp, 0);
    create_file(dir, foreign_tmp, 3600);
    create_file(dir, complete, 3600);

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_output_set(ctx, out_ffd, "uuid_file_tmp_max_age", "10m", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    /* old leftovers go, in-flight files of other writers stay */
    TEST_CHECK(!file_exists(dir, old_tmp));
    TEST_CHECK(file_exists(dir, new_tmp));
    TEST_CHECK(file_exists(dir, foreign_tmp));
    TEST_CHECK(file_exists(dir, complete));

    remove_dir(dir);
}

void flb_test_uuid_file_retry_on_failure(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    char *content;
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "retry");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_service_set(ctx, "scheduler.base", "1", "scheduler.cap", "1", NULL);
    flb_output_set(ctx, out_ffd, "format", "plain", "retry_limit", "false", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    /* the directory disappears, writing fails and the chunk is retried */
    rmdir(dir);
    push(ctx, in_ffd, "[1759823400, {\"msg\":\"retried\"}]");
    flb_time_msleep(1500);

    mkdir(dir, 0755);
    ret = wait_for_files(dir, ".log", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    list_files(dir, ".log", &list);
    TEST_CHECK(list.count == 1);
    TEST_CHECK(count_files(dir, ".tmp") == 0);
    if (list.count == 1) {
        content = read_file(dir, list.names[0]);
        TEST_CHECK(content != NULL && strcmp(content, "{\"msg\":\"retried\"}\n") == 0);
        free(content);
    }

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_mkdir(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char base[PATH_MAX];
    char dir[PATH_MAX];
    flb_ctx_t *ctx;

    make_test_dir(base, sizeof(base), "mkdir");
    snprintf(dir, sizeof(dir), "%s/nested", base);

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_output_set(ctx, out_ffd, "mkdir", "on", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {\"msg\":\"hello\"}]");
    ret = wait_for_files(dir, ".log", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    remove_dir(dir);
    remove_dir(base);
}

static int start_with(const char *dir, const char *k1, const char *v1,
                      const char *k2, const char *v2)
{
    int ret;
    int in_ffd;
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = flb_create();
    flb_service_set(ctx, "flush", TEST_FLUSH, "log_level", "off", NULL);

    in_ffd = flb_input(ctx, (char *) "lib", NULL);
    flb_input_set(ctx, in_ffd, "tag", "app.log", NULL);

    out_ffd = flb_output(ctx, (char *) "file", NULL);
    flb_output_set(ctx, out_ffd, "match", "*", "uuid_file", "on", NULL);
    if (dir != NULL) {
        flb_output_set(ctx, out_ffd, "path", dir, NULL);
    }
    if (k1 != NULL) {
        flb_output_set(ctx, out_ffd, k1, v1, NULL);
    }
    if (k2 != NULL) {
        flb_output_set(ctx, out_ffd, k2, v2, NULL);
    }

    ret = flb_start(ctx);
    if (ret == 0) {
        flb_stop(ctx);
    }
    flb_destroy(ctx);

    return ret;
}

void flb_test_uuid_file_invalid_config(void)
{
    char dir[PATH_MAX];
    char file[PATH_MAX];
    char dynamic[PATH_MAX];

    make_test_dir(dir, sizeof(dir), "invalid");
    snprintf(file, sizeof(file), "%s/regular", dir);
    create_file(dir, "regular", 0);
    snprintf(dynamic, sizeof(dynamic), "%s/$stream", dir);

    /* sanity check: the valid configuration starts */
    TEST_CHECK(start_with(dir, NULL, NULL, NULL, NULL) == 0);

    TEST_CHECK(start_with(NULL, NULL, NULL, NULL, NULL) == -1);
    TEST_CHECK(start_with(file, NULL, NULL, NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "file", "out.log", NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "rotate", "on", NULL, NULL) == -1);
    TEST_CHECK(start_with(dynamic, "fallback_file", "fallback.log", NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "uuid_file_prefix", "../x", NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "uuid_file_extension", ".tmp", NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "uuid_file_extension", "log/x", NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "uuid_file_prefix", "a\\b", NULL, NULL) == -1);

    /* time values are parsed with atoi(), typos must not turn into 0 */
    TEST_CHECK(start_with(dir, "uuid_file_tmp_max_age", "off", NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "uuid_file_tmp_max_age", "-1", NULL, NULL) == -1);
    TEST_CHECK(start_with(dir, "uuid_file_tmp_max_age", "0", NULL, NULL) == 0);
    TEST_CHECK(start_with(dir, "uuid_file_tmp_max_age", "1h", NULL, NULL) == 0);

    remove_dir(dir);
}

/*
 * Child body of the multi process test: one Fluent Bit context with many
 * inputs, so every flush round creates many files at the same time.
 */
static void multi_process_child(const char *dir, int worker, struct timeval *start_at)
{
    int i;
    int r;
    int ret;
    int out_ffd;
    int in_ffd[MP_INPUTS];
    char tag[32];
    char json[128];
    flb_ctx_t *ctx;
    struct timeval now;
    long wait_us;

    ctx = flb_create();
    flb_service_set(ctx, "flush", TEST_FLUSH, "grace", "2",
                    "log_level", "error", NULL);

    for (i = 0; i < MP_INPUTS; i++) {
        snprintf(tag, sizeof(tag), "in.%d", i);
        in_ffd[i] = flb_input(ctx, (char *) "lib", NULL);
        flb_input_set(ctx, in_ffd[i], "tag", tag, NULL);
    }

    out_ffd = flb_output(ctx, (char *) "file", NULL);
    flb_output_set(ctx, out_ffd, "match", "*", "path", dir, "uuid_file", "on",
                   "uuid_file_fsync", "off", "format", "plain", "workers", "4", NULL);

    ret = flb_start(ctx);
    if (ret != 0) {
        _exit(2);
    }

    /* start all processes at the same moment to provoke same-ms names */
    gettimeofday(&now, NULL);
    wait_us = (start_at->tv_sec - now.tv_sec) * 1000000L +
              (start_at->tv_usec - now.tv_usec);
    if (wait_us > 0) {
        usleep(wait_us);
    }

    for (r = 0; r < MP_ROUNDS; r++) {
        for (i = 0; i < MP_INPUTS; i++) {
            snprintf(json, sizeof(json),
                     "[1759823400, {\"w\":%d,\"in\":%d,\"r\":%d}]", worker, i, r);
            flb_lib_push(ctx, in_ffd[i], json, strlen(json));
        }
        flb_time_msleep(300);
    }

    flb_stop(ctx);
    flb_destroy(ctx);
    _exit(0);
}

void flb_test_uuid_file_multi_process(void)
{
    int i;
    int w;
    int in;
    int r;
    int file_w;
    int file_in;
    int status;
    int total = 0;
    int corrupted = 0;
    int duplicated = 0;
    int mixed = 0;
    char dir[PATH_MAX];
    char *content;
    char *line;
    char *saveptr;
    pid_t pids[MP_PROCS];
    static unsigned char seen[MP_PROCS][MP_INPUTS][MP_ROUNDS];
    struct file_list list;
    struct timeval start_at;

    make_test_dir(dir, sizeof(dir), "multi");
    memset(seen, 0, sizeof(seen));

    gettimeofday(&start_at, NULL);
    start_at.tv_sec += 1;

    for (i = 0; i < MP_PROCS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            multi_process_child(dir, i, &start_at);
        }
        TEST_CHECK(pids[i] > 0);
    }

    for (i = 0; i < MP_PROCS; i++) {
        if (pids[i] <= 0) {
            continue;
        }
        waitpid(pids[i], &status, 0);
        TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        TEST_MSG("child %d status %d", i, status);
    }

    TEST_CHECK(count_files(dir, ".tmp") == 0);

    /* every record exactly once, every file holds the records of one input */
    list_files(dir, ".log", &list);
    for (i = 0; i < list.count; i++) {
        if (check_uuid_name(list.names[i], "", ".log") != 0) {
            corrupted++;
            continue;
        }

        content = read_file(dir, list.names[i]);
        if (content == NULL) {
            corrupted++;
            continue;
        }

        file_w = -1;
        file_in = -1;
        for (line = strtok_r(content, "\n", &saveptr); line != NULL;
             line = strtok_r(NULL, "\n", &saveptr)) {
            if (sscanf(line, "{\"w\":%d,\"in\":%d,\"r\":%d}", &w, &in, &r) != 3 ||
                w < 0 || w >= MP_PROCS || in < 0 || in >= MP_INPUTS ||
                r < 0 || r >= MP_ROUNDS) {
                corrupted++;
                continue;
            }
            if (file_w == -1) {
                file_w = w;
                file_in = in;
            }
            else if (file_w != w || file_in != in) {
                mixed++;
            }
            if (seen[w][in][r]) {
                duplicated++;
            }
            seen[w][in][r] = 1;
            total++;
        }
        free(content);
    }

    TEST_CHECK(corrupted == 0);
    TEST_CHECK(duplicated == 0);
    TEST_CHECK(mixed == 0);
    TEST_CHECK(total == MP_PROCS * MP_INPUTS * MP_ROUNDS);
    TEST_CHECK(list.count >= MP_PROCS * MP_INPUTS);
    TEST_MSG("files=%d records=%d corrupted=%d duplicated=%d mixed=%d",
             list.count, total, corrupted, duplicated, mixed);

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_path_trailing_slash(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    char slash_dir[PATH_MAX];
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "slash");
    snprintf(slash_dir, sizeof(slash_dir), "%s//", dir);

    ctx = create_ctx(slash_dir, &in_ffd, &out_ffd);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {\"msg\":\"hello\"}]");
    ret = wait_for_files(dir, ".log", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    list_files(dir, ".log", &list);
    TEST_CHECK(list.count == 1);
    if (list.count == 1) {
        TEST_CHECK(check_uuid_name(list.names[0], "", ".log") == 0);
    }

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_empty_extension(void)
{
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "noext");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_output_set(ctx, out_ffd, "uuid_file_extension", "", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {\"msg\":\"hello\"}]");
    ret = wait_for_files(dir, "", 1);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    list_files(dir, "", &list);
    TEST_CHECK(list.count == 1);
    if (list.count == 1) {
        TEST_CHECK(check_uuid_name(list.names[0], "", "") == 0);
        TEST_MSG("name: %s", list.names[0]);
    }

    free_files(&list);
    remove_dir(dir);
}

void flb_test_uuid_file_csv_header_per_file(void)
{
    int i;
    int ret;
    int in_ffd;
    int out_ffd;
    char dir[PATH_MAX];
    char *content;
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "csv");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_output_set(ctx, out_ffd, "format", "csv", "csv_column_names", "on", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {\"a\":1,\"b\":\"x\"}]");
    ret = wait_for_files(dir, ".log", 1);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823401, {\"a\":2,\"b\":\"y\"}]");
    ret = wait_for_files(dir, ".log", 2);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    /* every file is self contained, so every file starts with the header */
    list_files(dir, ".log", &list);
    TEST_CHECK(list.count == 2);
    for (i = 0; i < list.count; i++) {
        content = read_file(dir, list.names[i]);
        TEST_CHECK(content != NULL &&
                   strncmp(content, "timestamp,\"a\",\"b\"\n", 18) == 0);
        TEST_MSG("content: %s", content ? content : "(null)");
        free(content);
    }

    free_files(&list);
    remove_dir(dir);
}

/* two outputs of one process writing into the same directory */
void flb_test_uuid_file_two_instances_same_dir(void)
{
    int i;
    int ret;
    int in_ffd;
    int out_ffd;
    int out2_ffd;
    int count_a = 0;
    char dir[PATH_MAX];
    char *content;
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "twoinst");

    ctx = create_ctx(dir, &in_ffd, &out_ffd);
    flb_output_set(ctx, out_ffd, "format", "plain", NULL);

    out2_ffd = flb_output(ctx, (char *) "file", NULL);
    flb_output_set(ctx, out2_ffd, "match", "*", "path", dir, "uuid_file", "on",
                   "format", "plain", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    push(ctx, in_ffd, "[1759823400, {\"msg\":\"a\"}]");
    ret = wait_for_files(dir, ".log", 2);
    TEST_CHECK(ret == 0);

    flb_stop(ctx);
    flb_destroy(ctx);

    /* each output publishes its own copy, nothing is overwritten */
    list_files(dir, ".log", &list);
    TEST_CHECK(list.count == 2);
    for (i = 0; i < list.count; i++) {
        content = read_file(dir, list.names[i]);
        if (content != NULL && strcmp(content, "{\"msg\":\"a\"}\n") == 0) {
            count_a++;
        }
        free(content);
    }
    TEST_CHECK(count_a == 2);
    TEST_CHECK(count_files(dir, ".tmp") == 0);

    free_files(&list);
    remove_dir(dir);
}

/*
 * One process, many inputs flushed by several output workers at the same
 * time: names generated by concurrent threads never collide.
 */
void flb_test_uuid_file_workers_concurrent(void)
{
    int i;
    int r;
    int ret;
    int w;
    int in;
    int total = 0;
    int bad = 0;
    int out_ffd;
    int in_ffd[MP_INPUTS * 2];
    char dir[PATH_MAX];
    char tag[32];
    char json[128];
    char *content;
    char *line;
    char *saveptr;
    static unsigned char seen[MP_INPUTS * 2][MP_ROUNDS];
    struct file_list list;
    flb_ctx_t *ctx;

    make_test_dir(dir, sizeof(dir), "workers");
    memset(seen, 0, sizeof(seen));

    ctx = flb_create();
    flb_service_set(ctx, "flush", TEST_FLUSH, "grace", "2", "log_level", "error", NULL);
    for (i = 0; i < MP_INPUTS * 2; i++) {
        snprintf(tag, sizeof(tag), "in.%d", i);
        in_ffd[i] = flb_input(ctx, (char *) "lib", NULL);
        flb_input_set(ctx, in_ffd[i], "tag", tag, NULL);
    }
    out_ffd = flb_output(ctx, (char *) "file", NULL);
    flb_output_set(ctx, out_ffd, "match", "*", "path", dir, "uuid_file", "on",
                   "format", "plain", "workers", "8", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);

    for (r = 0; r < MP_ROUNDS; r++) {
        for (i = 0; i < MP_INPUTS * 2; i++) {
            snprintf(json, sizeof(json), "[1759823400, {\"w\":0,\"in\":%d,\"r\":%d}]", i, r);
            flb_lib_push(ctx, in_ffd[i], json, strlen(json));
        }
        flb_time_msleep(300);
    }

    flb_stop(ctx);
    flb_destroy(ctx);

    TEST_CHECK(count_files(dir, ".tmp") == 0);
    list_files(dir, ".log", &list);
    for (i = 0; i < list.count; i++) {
        content = read_file(dir, list.names[i]);
        if (content == NULL) {
            bad++;
            continue;
        }
        for (line = strtok_r(content, "\n", &saveptr); line != NULL;
             line = strtok_r(NULL, "\n", &saveptr)) {
            if (sscanf(line, "{\"w\":%d,\"in\":%d,\"r\":%d}", &w, &in, &r) != 3 ||
                in < 0 || in >= MP_INPUTS * 2 || r < 0 || r >= MP_ROUNDS ||
                seen[in][r]) {
                bad++;
                continue;
            }
            seen[in][r] = 1;
            total++;
        }
        free(content);
    }

    TEST_CHECK(bad == 0);
    TEST_CHECK(total == MP_INPUTS * 2 * MP_ROUNDS);
    TEST_MSG("files=%d records=%d bad=%d", list.count, total, bad);

    free_files(&list);
    remove_dir(dir);
}

static void sleep_until(struct timeval *at)
{
    long wait_us;
    struct timeval now;

    gettimeofday(&now, NULL);
    wait_us = (at->tv_sec - now.tv_sec) * 1000000L + (at->tv_usec - now.tv_usec);
    if (wait_us > 0) {
        usleep(wait_us);
    }
}

/*
 * Many processes starting at the same moment with mkdir enabled race to
 * create the same nested directory: all of them must start.
 */
void flb_test_uuid_file_multi_process_mkdir(void)
{
    int i;
    int ret;
    int round;
    int status;
    int failed = 0;
    int out_ffd;
    int in_ffd;
    char base[PATH_MAX];
    char dir[PATH_MAX];
    char mid[PATH_MAX];
    pid_t pids[MP_PROCS * 2];
    flb_ctx_t *ctx;
    struct timeval start_at;

    for (round = 0; round < 5; round++) {
        make_test_dir(base, sizeof(base), "mpmkdir");
        snprintf(mid, sizeof(mid), "%s/a", base);
        snprintf(dir, sizeof(dir), "%s/a/b", base);

        gettimeofday(&start_at, NULL);
        start_at.tv_usec += 300000;
        if (start_at.tv_usec >= 1000000) {
            start_at.tv_sec++;
            start_at.tv_usec -= 1000000;
        }

        for (i = 0; i < MP_PROCS * 2; i++) {
            pids[i] = fork();
            if (pids[i] == 0) {
                ctx = flb_create();
                flb_service_set(ctx, "flush", TEST_FLUSH, "log_level", "error", NULL);
                in_ffd = flb_input(ctx, (char *) "lib", NULL);
                flb_input_set(ctx, in_ffd, "tag", "app.log", NULL);
                out_ffd = flb_output(ctx, (char *) "file", NULL);
                flb_output_set(ctx, out_ffd, "match", "*", "path", dir,
                               "mkdir", "on", "uuid_file", "on", NULL);

                sleep_until(&start_at);
                ret = flb_start(ctx);
                if (ret == 0) {
                    flb_stop(ctx);
                }
                flb_destroy(ctx);
                _exit(ret == 0 ? 0 : 3);
            }
        }

        for (i = 0; i < MP_PROCS * 2; i++) {
            waitpid(pids[i], &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                failed++;
            }
        }

        rmdir(dir);
        rmdir(mid);
        remove_dir(base);
    }

    TEST_CHECK(failed == 0);
    TEST_MSG("%d of %d processes failed to start", failed, 5 * MP_PROCS * 2);
}
