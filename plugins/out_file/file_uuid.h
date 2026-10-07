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

#ifndef FLB_OUT_FILE_UUID_H
#define FLB_OUT_FILE_UUID_H

#include <fluent-bit/flb_output_plugin.h>

#include <stdio.h>

#define FILE_UUID_TMP_SUFFIX  ".tmp"

/* file_uuid_commit() results */
#define FILE_UUID_PUBLISHED    0
#define FILE_UUID_EMPTY        1
#define FILE_UUID_FAILED      -1

struct file_uuid_ctx;

/*
 * Create the 'one file per flush' context: every flush is written into
 * <path>/<prefix><uuidv7><extension>.tmp and then published atomically as
 * <path>/<prefix><uuidv7><extension>, never replacing an existing file.
 *
 * Validates the settings, checks that 'path' is a directory and removes
 * temporary files older than 'tmp_max_age' seconds left by crashed writers.
 * Returns NULL on invalid configuration or when the platform is unsupported.
 */
struct file_uuid_ctx *file_uuid_create(struct flb_output_instance *ins,
                                       const char *path, const char *prefix,
                                       const char *extension, int fsync,
                                       int tmp_max_age);

void file_uuid_destroy(struct file_uuid_ctx *ctx);

/*
 * Create a new exclusive temporary file. Its name is stored in 'tmp' and the
 * stream must be passed to file_uuid_commit() or file_uuid_abort().
 */
FILE *file_uuid_open(struct file_uuid_ctx *ctx, char *tmp, size_t tmp_size);

/*
 * Flush, sync and close 'fp', then publish it under its final name. Empty
 * files are removed instead of published. On failure nothing is published
 * and the temporary file is removed.
 */
int file_uuid_commit(struct file_uuid_ctx *ctx, FILE *fp, const char *tmp);

/* Close 'fp' and remove the temporary file without publishing it */
void file_uuid_abort(struct file_uuid_ctx *ctx, FILE *fp, const char *tmp);

#endif
