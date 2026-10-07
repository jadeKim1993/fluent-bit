/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2026 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * One file per flush, named after a UUIDv7.
 *
 *  1. the chunk is written into <path>/<prefix><uuid><ext>.tmp, created with
 *     O_EXCL so concurrent writers never share a temporary file
 *  2. the file is fsync'ed (optional) and closed
 *  3. it is published as <path>/<prefix><uuid><ext> with link(2) followed by
 *     unlink(2) of the temporary name. link(2) is atomic and fails with EEXIST
 *     instead of replacing an existing file, unlike rename(2)
 *  4. the directory is fsync'ed (optional)
 *
 * Consumers only pick '*<ext>' names, so they only ever see complete files.
 * UUIDv7 names start with the creation time in milliseconds, so sorting the
 * names sorts the files by creation time.
 */

#include <fluent-bit/flb_compat.h>
#include <fluent-bit/flb_lock.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_random.h>
#include <fluent-bit/flb_sds.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef FLB_SYSTEM_WINDOWS
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "file_uuid.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* textual UUID length, without the trailing NUL */
#define FILE_UUID_LEN           36
#define FILE_UUID_MAX_ATTEMPTS  3
#define FILE_UUID_FILE_MODE     0640

struct file_uuid_ctx {
    flb_sds_t path;
    flb_sds_t prefix;
    flb_sds_t ext;
    int fsync;
    int tmp_max_age;

    /*
     * Last UUIDv7 time value handed out: unix milliseconds in the high bits
     * and a 12-bit sub-millisecond sequence in the low bits. Kept strictly
     * increasing so names created by one instance sort in creation order,
     * even inside the same millisecond and across workers.
     */
    uint64_t last_time;
    flb_lock_t lock;

    struct flb_output_instance *ins;
};

#ifdef FLB_SYSTEM_WINDOWS

struct file_uuid_ctx *file_uuid_create(struct flb_output_instance *ins,
                                       const char *path, const char *prefix,
                                       const char *extension, int fsync,
                                       int tmp_max_age)
{
    flb_plg_error(ins, "uuid_file is not supported on Windows");
    return NULL;
}

void file_uuid_destroy(struct file_uuid_ctx *ctx)
{
}

FILE *file_uuid_open(struct file_uuid_ctx *ctx, char *tmp, size_t tmp_size)
{
    return NULL;
}

int file_uuid_commit(struct file_uuid_ctx *ctx, FILE *fp, const char *tmp)
{
    return FILE_UUID_FAILED;
}

void file_uuid_abort(struct file_uuid_ctx *ctx, FILE *fp, const char *tmp)
{
}

#else

/*
 * Generate a RFC 9562 UUIDv7 in its canonical lowercase form:
 *
 *   48 bits unix_ts_ms | 4 bits version (7) | 12 bits rand_a |
 *    2 bits variant    | 62 bits rand_b
 *
 * rand_a carries the sub-millisecond sequence (RFC 9562 section 6.2,
 * method 3) and rand_b is random, so separate processes writing into the
 * same directory never need to coordinate to get unique names.
 */
static int uuidv7_generate(struct file_uuid_ctx *ctx, char *out, size_t size)
{
    uint64_t ms;
    uint64_t seq;
    uint64_t now;
    unsigned char b[16];
    struct timespec ts;

    if (size < FILE_UUID_LEN + 1) {
        return -1;
    }

    if (flb_random_bytes(b, sizeof(b)) != 0) {
        return -1;
    }

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }

    ms = ((uint64_t) ts.tv_sec * 1000) + ((uint64_t) ts.tv_nsec / 1000000);
    seq = (((uint64_t) ts.tv_nsec % 1000000) >> 8) & 0xfff;
    now = (ms << 12) | seq;

    if (flb_lock_acquire(&ctx->lock, FLB_LOCK_DEFAULT_RETRY_LIMIT,
                         FLB_LOCK_DEFAULT_RETRY_DELAY) != 0) {
        return -1;
    }
    if (now <= ctx->last_time) {
        now = ctx->last_time + 1;
    }
    ctx->last_time = now;
    flb_lock_release(&ctx->lock, FLB_LOCK_DEFAULT_RETRY_LIMIT,
                     FLB_LOCK_DEFAULT_RETRY_DELAY);

    ms = now >> 12;
    seq = now & 0xfff;

    b[0] = (unsigned char) (ms >> 40);
    b[1] = (unsigned char) (ms >> 32);
    b[2] = (unsigned char) (ms >> 24);
    b[3] = (unsigned char) (ms >> 16);
    b[4] = (unsigned char) (ms >> 8);
    b[5] = (unsigned char) ms;
    b[6] = (unsigned char) (0x70 | (seq >> 8));
    b[7] = (unsigned char) (seq & 0xff);
    b[8] = (unsigned char) ((b[8] & 0x3f) | 0x80);

    snprintf(out, size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);

    return 0;
}

/* <path>/<prefix><uuid><ext>, plus the temporary suffix when requested */
static int compose_name(struct file_uuid_ctx *ctx, int tmp,
                        char *out, size_t size)
{
    int ret;
    char id[FILE_UUID_LEN + 1];

    if (uuidv7_generate(ctx, id, sizeof(id)) != 0) {
        flb_plg_error(ctx->ins, "could not generate a UUIDv7");
        return -1;
    }

    ret = snprintf(out, size, "%s/%s%s%s%s", ctx->path, ctx->prefix, id, ctx->ext,
                   tmp ? FILE_UUID_TMP_SUFFIX : "");
    if (ret < 0 || (size_t) ret >= size) {
        flb_plg_error(ctx->ins, "output file name too long");
        return -1;
    }

    return 0;
}

static int has_suffix(const char *str, size_t len, const char *suffix)
{
    size_t suffix_len;

    suffix_len = strlen(suffix);
    if (len < suffix_len) {
        return FLB_FALSE;
    }

    return strcmp(str + len - suffix_len, suffix) == 0;
}

/*
 * Remove temporary files left by a crashed writer. Other processes may be
 * writing into the same directory right now, so only temporary names of this
 * instance's naming scheme older than tmp_max_age are removed.
 */
static void cleanup_stale_tmp(struct file_uuid_ctx *ctx)
{
    int ret;
    size_t len;
    size_t prefix_len;
    size_t suffix_len;
    time_t now;
    char suffix[PATH_MAX];
    char file[PATH_MAX];
    DIR *dir;
    struct dirent *ent;
    struct stat st;

    ret = snprintf(suffix, sizeof(suffix), "%s" FILE_UUID_TMP_SUFFIX, ctx->ext);
    if (ret < 0 || (size_t) ret >= sizeof(suffix)) {
        return;
    }
    suffix_len = ret;
    prefix_len = flb_sds_len(ctx->prefix);

    dir = opendir(ctx->path);
    if (dir == NULL) {
        flb_plg_warn(ctx->ins, "could not scan %s for stale temporary files: %s",
                     ctx->path, strerror(errno));
        return;
    }

    now = time(NULL);
    while ((ent = readdir(dir)) != NULL) {
        len = strlen(ent->d_name);
        if (len < prefix_len + FILE_UUID_LEN + suffix_len ||
            strncmp(ent->d_name, ctx->prefix, prefix_len) != 0 ||
            !has_suffix(ent->d_name, len, suffix)) {
            continue;
        }

        ret = snprintf(file, sizeof(file), "%s/%s", ctx->path, ent->d_name);
        if (ret < 0 || (size_t) ret >= sizeof(file)) {
            continue;
        }

        if (lstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (now - st.st_mtime <= ctx->tmp_max_age) {
            continue;
        }

        if (unlink(file) == 0) {
            flb_plg_info(ctx->ins, "removed stale temporary file %s", file);
        }
    }

    closedir(dir);
}

static int check_name_part(struct flb_output_instance *ins,
                           const char *name, const char *value)
{
    if (strchr(value, '/') != NULL || strchr(value, '\\') != NULL) {
        flb_plg_error(ins, "'%s' must not contain path separators", name);
        return -1;
    }

    /* a published name ending in .tmp would look like a temporary file */
    if (has_suffix(value, strlen(value), FILE_UUID_TMP_SUFFIX)) {
        flb_plg_error(ins, "'%s' must not end with '%s'", name, FILE_UUID_TMP_SUFFIX);
        return -1;
    }

    return 0;
}

struct file_uuid_ctx *file_uuid_create(struct flb_output_instance *ins,
                                       const char *path, const char *prefix,
                                       const char *extension, int fsync,
                                       int tmp_max_age)
{
    size_t name_len;
    struct stat st;
    struct file_uuid_ctx *ctx;

    if (path == NULL || path[0] == '\0') {
        flb_plg_error(ins, "'path' is required when uuid_file is enabled");
        return NULL;
    }

    if (tmp_max_age < 0) {
        flb_plg_error(ins, "'uuid_file_tmp_max_age' must not be negative");
        return NULL;
    }

    ctx = flb_calloc(1, sizeof(struct file_uuid_ctx));
    if (ctx == NULL) {
        flb_errno();
        return NULL;
    }
    ctx->ins = ins;
    ctx->fsync = fsync;
    ctx->tmp_max_age = tmp_max_age;

    if (flb_lock_init(&ctx->lock) != 0) {
        flb_plg_error(ins, "could not initialize uuid_file lock");
        flb_free(ctx);
        return NULL;
    }

    ctx->path = flb_sds_create(path);
    ctx->prefix = flb_sds_create(prefix != NULL ? prefix : "");

    /* the extension always starts with a dot, unless it is empty */
    if (extension == NULL || extension[0] == '\0' || extension[0] == '.') {
        ctx->ext = flb_sds_create(extension != NULL ? extension : "");
    }
    else {
        ctx->ext = flb_sds_create(".");
        if (ctx->ext != NULL) {
            ctx->ext = flb_sds_cat(ctx->ext, extension, strlen(extension));
        }
    }

    if (ctx->path == NULL || ctx->prefix == NULL || ctx->ext == NULL) {
        flb_errno();
        file_uuid_destroy(ctx);
        return NULL;
    }

    /* keep the directory name free of a trailing separator */
    while (flb_sds_len(ctx->path) > 1 &&
           ctx->path[flb_sds_len(ctx->path) - 1] == '/') {
        flb_sds_len_set(ctx->path, flb_sds_len(ctx->path) - 1);
        ctx->path[flb_sds_len(ctx->path)] = '\0';
    }

    if (check_name_part(ins, "uuid_file_prefix", ctx->prefix) != 0 ||
        check_name_part(ins, "uuid_file_extension", ctx->ext) != 0) {
        file_uuid_destroy(ctx);
        return NULL;
    }

    name_len = flb_sds_len(ctx->path) + 1 + flb_sds_len(ctx->prefix) +
               FILE_UUID_LEN + flb_sds_len(ctx->ext) + sizeof(FILE_UUID_TMP_SUFFIX);
    if (name_len > PATH_MAX) {
        flb_plg_error(ins, "path, uuid_file_prefix and uuid_file_extension are too long");
        file_uuid_destroy(ctx);
        return NULL;
    }

    if (stat(ctx->path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        flb_plg_error(ins, "path %s is not a directory", ctx->path);
        file_uuid_destroy(ctx);
        return NULL;
    }

    cleanup_stale_tmp(ctx);

    flb_plg_info(ins, "uuid_file: path=%s prefix='%s' extension='%s' fsync=%s",
                 ctx->path, ctx->prefix, ctx->ext, ctx->fsync ? "on" : "off");

    return ctx;
}

void file_uuid_destroy(struct file_uuid_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    flb_sds_destroy(ctx->path);
    flb_sds_destroy(ctx->prefix);
    flb_sds_destroy(ctx->ext);
    flb_lock_destroy(&ctx->lock);
    flb_free(ctx);
}

FILE *file_uuid_open(struct file_uuid_ctx *ctx, char *tmp, size_t tmp_size)
{
    int fd;
    int attempt;
    FILE *fp;

    for (attempt = 0; attempt < FILE_UUID_MAX_ATTEMPTS; attempt++) {
        if (compose_name(ctx, FLB_TRUE, tmp, tmp_size) != 0) {
            return NULL;
        }

        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, FILE_UUID_FILE_MODE);
        if (fd < 0 && errno == EEXIST) {
            /* extremely unlikely collision, try again with a new id */
            continue;
        }
        if (fd < 0) {
            flb_plg_error(ctx->ins, "could not create %s: %s", tmp, strerror(errno));
            return NULL;
        }

        fp = fdopen(fd, "wb");
        if (fp == NULL) {
            flb_plg_error(ctx->ins, "could not open stream for %s: %s",
                          tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return NULL;
        }

        return fp;
    }

    flb_plg_error(ctx->ins, "could not allocate a unique temporary file name");
    return NULL;
}

void file_uuid_abort(struct file_uuid_ctx *ctx, FILE *fp, const char *tmp)
{
    (void) ctx;

    fclose(fp);
    unlink(tmp);
}

/*
 * Make 'tmp' visible as 'final' without ever replacing an existing 'final'.
 * Filesystems without hard link support fall back to an existence check
 * followed by rename(2).
 *
 * Returns 1 when published, 0 when 'final' already exists and -1 on error.
 */
static int publish_no_replace(struct file_uuid_ctx *ctx,
                              const char *tmp, const char *final)
{
    struct stat st;

    if (link(tmp, final) == 0) {
        /* 'final' refers to the same inode, only drop the temporary name */
        if (unlink(tmp) != 0) {
            flb_plg_warn(ctx->ins, "could not remove temporary name %s: %s",
                         tmp, strerror(errno));
        }
        return 1;
    }

    if (errno == EEXIST) {
        return 0;
    }

    flb_plg_debug(ctx->ins, "link() to %s failed (%s), using rename() fallback",
                  final, strerror(errno));

    if (lstat(final, &st) == 0) {
        return 0;
    }

    if (rename(tmp, final) != 0) {
        flb_plg_error(ctx->ins, "could not publish %s: %s", final, strerror(errno));
        return -1;
    }

    return 1;
}

/*
 * Persist the directory entry created by the publish step. The file is
 * already visible, so a failure is only reported: retrying the chunk would
 * write the same records into a second file.
 */
static void sync_dir(struct file_uuid_ctx *ctx)
{
    int fd;

    fd = open(ctx->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        flb_plg_warn(ctx->ins, "could not open %s for fsync: %s",
                     ctx->path, strerror(errno));
        return;
    }

    if (fsync(fd) != 0) {
        flb_plg_warn(ctx->ins, "fsync of directory %s failed: %s",
                     ctx->path, strerror(errno));
    }
    close(fd);
}

int file_uuid_commit(struct file_uuid_ctx *ctx, FILE *fp, const char *tmp)
{
    int ret;
    int attempt;
    long size;
    size_t len;
    char final[PATH_MAX];

    if (fflush(fp) != 0 || ferror(fp)) {
        flb_plg_error(ctx->ins, "could not write %s: %s", tmp, strerror(errno));
        file_uuid_abort(ctx, fp, tmp);
        return FILE_UUID_FAILED;
    }

    /* chunks without any output never create a file */
    size = ftell(fp);
    if (size == 0) {
        file_uuid_abort(ctx, fp, tmp);
        return FILE_UUID_EMPTY;
    }

    if (ctx->fsync == FLB_TRUE && fsync(fileno(fp)) != 0) {
        flb_plg_error(ctx->ins, "fsync of %s failed: %s", tmp, strerror(errno));
        file_uuid_abort(ctx, fp, tmp);
        return FILE_UUID_FAILED;
    }

    if (fclose(fp) != 0) {
        flb_plg_error(ctx->ins, "could not close %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return FILE_UUID_FAILED;
    }

    /* first try the name matching the temporary file */
    len = strlen(tmp) - (sizeof(FILE_UUID_TMP_SUFFIX) - 1);
    if (len >= sizeof(final)) {
        unlink(tmp);
        return FILE_UUID_FAILED;
    }
    memcpy(final, tmp, len);
    final[len] = '\0';

    for (attempt = 0; attempt < FILE_UUID_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0 && compose_name(ctx, FLB_FALSE, final, sizeof(final)) != 0) {
            break;
        }

        ret = publish_no_replace(ctx, tmp, final);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            /* a complete file with this name exists, never touch it */
            continue;
        }

        if (ctx->fsync == FLB_TRUE) {
            sync_dir(ctx);
        }

        flb_plg_debug(ctx->ins, "published %s (%ld bytes)", final, size);
        return FILE_UUID_PUBLISHED;
    }

    if (attempt == FILE_UUID_MAX_ATTEMPTS) {
        flb_plg_error(ctx->ins, "could not allocate a unique file name");
    }
    unlink(tmp);
    return FILE_UUID_FAILED;
}

#endif
