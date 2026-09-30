/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019-2022 The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
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

#include <fluent-bit.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_time.h>
#include <float.h>
#include <math.h>
#include <msgpack.h>
#include "flb_tests_runtime.h"

struct test_ctx {
    flb_ctx_t *flb;    /* Fluent Bit library context */
    int i_ffd;         /* Input fd  */
    int f_ffd;         /* Filter fd (unused) */
    int o_ffd;         /* Output fd */
};


pthread_mutex_t result_mutex = PTHREAD_MUTEX_INITIALIZER;
int num_output = 0;
static const char *callback_error = NULL;

static int get_output_num()
{
    int ret;

    pthread_mutex_lock(&result_mutex);
    ret = num_output;
    pthread_mutex_unlock(&result_mutex);

    return ret;
}

static void increment_output_num()
{
    pthread_mutex_lock(&result_mutex);
    num_output++;
    pthread_mutex_unlock(&result_mutex);
}

static void add_output_num(int num)
{
    pthread_mutex_lock(&result_mutex);
    num_output += num;
    pthread_mutex_unlock(&result_mutex);
}

static void set_callback_error(const char *message)
{
    pthread_mutex_lock(&result_mutex);
    if (callback_error == NULL) {
        callback_error = message;
    }
    pthread_mutex_unlock(&result_mutex);
}

static void clear_output_num()
{
    pthread_mutex_lock(&result_mutex);
    num_output = 0;
    callback_error = NULL;
    pthread_mutex_unlock(&result_mutex);
}

static void check_callback_error()
{
    const char *message;

    pthread_mutex_lock(&result_mutex);
    message = callback_error;
    pthread_mutex_unlock(&result_mutex);

    if (!TEST_CHECK(message == NULL)) {
        TEST_MSG("%s", message);
    }
}

struct str_list {
    size_t size;
    char **lists;
};

/* Callback to check expected results */
static void cb_check_str_list(void *ctx, int ffd, int res_ret, 
                              void *res_data, size_t res_size, void *data)
{
    char *p;
    flb_sds_t out_line = res_data;
    size_t i;
    struct str_list *l = (struct str_list *)data;

    if (res_data == NULL) {
        set_callback_error("formatter returned no output");
        return;
    }

    if (l == NULL) {
        set_callback_error("formatter callback data is NULL");
        flb_sds_destroy(out_line);
        return;
    }

    if (res_ret != 0) {
        set_callback_error("formatter returned an error");
    }

    for (i = 0; i < l->size; i++) {
        p = strstr(out_line, l->lists[i]);
        if (p == NULL) {
            set_callback_error("formatter output did not contain an expected string");
        }
    }

    increment_output_num();
    flb_sds_destroy(out_line);
}

static int msgpack_strncmp(char* str, size_t str_len, msgpack_object obj)
{
    int ret = -1;

    if (str == NULL) {
        flb_error("str is NULL");
        return -1;
    }

    switch (obj.type)  {
    case MSGPACK_OBJECT_STR:
        if (obj.via.str.size != str_len) {
            return -1;
        }
        ret = strncmp(str, obj.via.str.ptr, str_len);
        break;
    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        {
            unsigned long val = strtoul(str, NULL, 10);
            if (val == (unsigned long)obj.via.u64) {
                ret = 0;
            }
        }
        break;
    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        {
            long long val = strtoll(str, NULL, 10);
            if (val == obj.via.i64) {
                ret = 0;
            }
        }
        break;
    case MSGPACK_OBJECT_FLOAT32:
    case MSGPACK_OBJECT_FLOAT64:
        {
            double val = strtod(str, NULL);
            if (fabs(val - obj.via.f64) < DBL_EPSILON) {
                ret = 0;
            }
        }
        break;
    case MSGPACK_OBJECT_BOOLEAN:
        if (obj.via.boolean) {
            if (str_len != 4 /*true*/) {
                return -1;
            }
            ret = strncasecmp(str, "true", 4);
        }
        else {
            if (str_len != 5 /*false*/) {
                return -1;
            }
            ret = strncasecmp(str, "false", 5);
        }
        break;
    default:
        flb_error("not supported");
    }

    return ret;
}

/* Callback to check expected results */
static void cb_check_msgpack_kv(void *ctx, int ffd, int res_ret,
                                void *res_data, size_t res_size, void *data)
{
    msgpack_unpacked result;
    msgpack_object obj;
    size_t off = 0;
    struct str_list *l = (struct str_list *)data;
    int i_map;
    int map_size;
    int i_list;
    int matches = 0;
    msgpack_unpack_return unpack_result = MSGPACK_UNPACK_CONTINUE;

    if (res_data == NULL) {
        set_callback_error("formatter returned no output");
        return;
    }

    if (data == NULL) {
        set_callback_error("formatter callback data is NULL");
        return;
    }

    if (res_ret != 0) {
        set_callback_error("formatter returned an error");
    }

    /* Iterate each item array and apply rules */
    msgpack_unpacked_init(&result);
    while (off < res_size) {
        unpack_result = msgpack_unpack_next(&result, res_data, res_size, &off);
        if (unpack_result != MSGPACK_UNPACK_SUCCESS) {
            break;
        }

        obj = result.data;
        /*
        msgpack_object_print(stdout, obj);
        */
        if (obj.type != MSGPACK_OBJECT_ARRAY || obj.via.array.size != 2) {
            set_callback_error("formatter output contained an invalid record");
            continue;
        }
        obj = obj.via.array.ptr[1];
        if (obj.type != MSGPACK_OBJECT_MAP) {
            set_callback_error("formatter output record was not a map");
            continue;
        }
        map_size = obj.via.map.size;
        for (i_map=0; i_map<map_size; i_map++) {
            if (obj.via.map.ptr[i_map].key.type != MSGPACK_OBJECT_STR) {
                set_callback_error("formatter output map key was not a string");
                continue;
            }
            for (i_list=0; i_list< l->size/2; i_list++)  {
                if (msgpack_strncmp(l->lists[i_list*2], strlen(l->lists[i_list*2]),
                                    obj.via.map.ptr[i_map].key) == 0 &&
                    msgpack_strncmp(l->lists[i_list*2+1], strlen(l->lists[i_list*2+1]),
                                    obj.via.map.ptr[i_map].val) == 0) {
                    matches++;
                }
            }
        }
    }

    if (unpack_result == MSGPACK_UNPACK_PARSE_ERROR) {
        set_callback_error("formatter output contained invalid MessagePack");
    }
    else if (unpack_result == MSGPACK_UNPACK_NOMEM_ERROR) {
        set_callback_error("could not unpack formatter output");
    }
    else if (unpack_result == MSGPACK_UNPACK_CONTINUE && res_size != 0) {
        set_callback_error("formatter output contained incomplete MessagePack");
    }
    else if (unpack_result != MSGPACK_UNPACK_SUCCESS &&
             unpack_result != MSGPACK_UNPACK_CONTINUE) {
        set_callback_error("formatter output returned an unexpected unpack result");
    }
    else {
        add_output_num(matches);
    }

    msgpack_unpacked_destroy(&result);
}

static struct test_ctx *test_ctx_create()
{
    int i_ffd;
    int o_ffd;
    struct test_ctx *ctx = NULL;

    ctx = flb_malloc(sizeof(struct test_ctx));
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("malloc failed");
        flb_errno();
        return NULL;
    }

    /* Service config */
    ctx->flb = flb_create();
    flb_service_set(ctx->flb,
                    "Flush", "0.200000000",
                    "Grace", "1",
                    "Log_Level", "error",
                    NULL);

    /* Input */
    i_ffd = flb_input(ctx->flb, (char *) "lib", NULL);
    TEST_CHECK(i_ffd >= 0);
    ctx->i_ffd = i_ffd;

    /* Output */
    o_ffd = flb_output(ctx->flb, (char *) "http", NULL);
    ctx->o_ffd = o_ffd;

    return ctx;
}

static void test_ctx_destroy(struct test_ctx *ctx)
{
    TEST_CHECK(ctx != NULL);

    sleep(1);
    flb_stop(ctx->flb);
    check_callback_error();
    flb_destroy(ctx->flb);
    flb_free(ctx);
}

void flb_test_format_msgpack()
{
    int attempts;
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\", \"val\":1000, \"nval\":-10000, \"bool\":true, \"float\":1.234}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"msg", "hello world", "val", "1000", "nval", "-10000", "bool", "true", "float", "1.234"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "msgpack",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_msgpack_kv,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    for (attempts = 0; attempts < 50; attempts++) {
        if (get_output_num() == expected.size / 2) {
            break;
        }
        flb_time_msleep(100);
    }

    num = get_output_num();
    if (!TEST_CHECK(num == expected.size / 2))  {
        TEST_MSG("got %d, expected %lu", num, expected.size/2);
    }

    test_ctx_destroy(ctx);
}

void flb_test_format_json()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);
    char *buf2 = "[2, {\"msg\":\"hello world\"}]";
    size_t size2 = strlen(buf2);

    char *expected_strs[] = {"[{\"date\":1.0,\"msg\":\"hello world\"},{\"date\":2.0,\"msg\":\"hello world\"}]"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf2, size2);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_format_json_stream()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);
    char *buf2 = "[2, {\"msg\":\"hello world\"}]";
    size_t size2 = strlen(buf2);

    char *expected_strs[] = {"{\"date\":1.0,\"msg\":\"hello world\"}{\"date\":2.0,\"msg\":\"hello world\"}"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json_stream",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf2, size2);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_format_json_lines()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);
    char *buf2 = "[2, {\"msg\":\"hello world\"}]";
    size_t size2 = strlen(buf2);

    char *expected_strs[] = {"{\"date\":1.0,\"msg\":\"hello world\"}\n{\"date\":2.0,\"msg\":\"hello world\"}"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json_lines",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf2, size2);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_format_gelf()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"\"short_message\":\"hello world\"", "\"timestamp\":1.000"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "gelf",
                         "gelf_short_message_key", "msg",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}


void flb_test_format_gelf_host_key()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\", \"h_key\":\"localhost\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"\"short_message\":\"hello world\"", "\"timestamp\":1.000", "\"host\":\"localhost\""};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "gelf",
                         "gelf_short_message_key", "msg",
                         "gelf_host_key", "h_key",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_format_gelf_timestamp_key()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\", \"t_key\":\"2018-05-30T09:39:52.000681Z\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"\"short_message\":\"hello world\"", "\"timestamp\":\"2018-05-30T09:39:52.000681Z\""};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "gelf",
                         "gelf_short_message_key", "msg",
                         "gelf_timestamp_key", "t_key",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_format_gelf_full_message_key()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\", \"f_msg\":\"full message\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"\"short_message\":\"hello world\"", "\"timestamp\":1.000","\"full_message\":\"full message\""};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "gelf",
                         "gelf_short_message_key", "msg",
                         "gelf_full_message_key", "f_msg",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_format_gelf_level_key()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\", \"l_msg\":6\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"\"short_message\":\"hello world\"", "\"timestamp\":1.000","\"level\":6"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "gelf",
                         "gelf_short_message_key", "msg",
                         "gelf_level_key", "l_msg",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_set_json_date_key()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"{\"timestamp\":1.0,\"msg\":\"hello world\"}"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json",
                         "json_date_key", "timestamp",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_disable_json_date_key()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"{\"msg\":\"hello world\"}"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json",
                         "json_date_key", "false",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_json_date_format_epoch()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"{\"date\":1,\"msg\":\"hello world\"}"};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json",
                         "json_date_format", "epoch",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_json_date_format_iso8601()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"\"msg\":\"hello world\"", "\"date\":\"1970-01-01T00:00:01.000000Z\""};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json",
                         "json_date_format", "iso8601",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

void flb_test_json_date_format_java_sql_timestamp()
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);

    char *expected_strs[] = {"\"msg\":\"hello world\"", "\"date\":\"1970-01-01 00:00:01.000000\""};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json",
                         "json_date_format", "java_sql_timestamp",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

static void run_json_events_key_test(char *count_key, char *expected_json)
{
    struct test_ctx *ctx;
    int ret;
    int num;

    char *buf1 = "[1, {\"msg\":\"hello world\"}]";
    size_t size1 = strlen(buf1);
    char *buf2 = "[2, {\"msg\":\"hello world\"}]";
    size_t size2 = strlen(buf2);

    char *expected_strs[] = {expected_json};
    struct str_list expected = {
                                .size = sizeof(expected_strs)/sizeof(char*),
                                .lists = &expected_strs[0],
    };

    clear_output_num();

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "format", "json",
                         "json_events_key", "events",
                         NULL);
    TEST_CHECK(ret == 0);

    if (count_key != NULL) {
        ret = flb_output_set(ctx->flb, ctx->o_ffd,
                             "json_count_key", count_key,
                             NULL);
        TEST_CHECK(ret == 0);
    }

    ret = flb_output_set_test(ctx->flb, ctx->o_ffd,
                         "formatter", cb_check_str_list,
                          &expected, NULL);
    TEST_CHECK(ret == 0);

    /* Start the engine */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf1, size1);
    TEST_CHECK(ret >= 0);
    ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf2, size2);
    TEST_CHECK(ret >= 0);

    /* waiting to flush */
    flb_time_msleep(500);

    num = get_output_num();
    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }
    check_callback_error();

    test_ctx_destroy(ctx);
}

void flb_test_json_events_key()
{
    run_json_events_key_test(NULL,
        "{\"count\":2,\"events\":[{\"date\":1.0,\"msg\":\"hello world\"},"
        "{\"date\":2.0,\"msg\":\"hello world\"}]}");
}

void flb_test_json_events_key_count_key()
{
    run_json_events_key_test("total",
        "{\"total\":2,\"events\":[{\"date\":1.0,\"msg\":\"hello world\"},"
        "{\"date\":2.0,\"msg\":\"hello world\"}]}");
}

void flb_test_json_events_key_disable_count()
{
    run_json_events_key_test("false",
        "{\"events\":[{\"date\":1.0,\"msg\":\"hello world\"},"
        "{\"date\":2.0,\"msg\":\"hello world\"}]}");
}

int callback_test(void* data, size_t size, void* cb_data)
{
    if (size > 0) {
        increment_output_num();
    }
    return 0;
}

static int batch_saw_count = 0;

/* in_http turns every request body object into a single record */
static int callback_batch(void* data, size_t size, void* cb_data)
{
    flb_sds_t s;

    if (size > 0) {
        increment_output_num();

        s = flb_sds_create_len(data, size);
        if (s && strstr(s, (char *) cb_data) != NULL) {
            pthread_mutex_lock(&result_mutex);
            batch_saw_count++;
            pthread_mutex_unlock(&result_mutex);
        }
        flb_sds_destroy(s);
    }
    return 0;
}

/* records flushed during an interval must be sent in exactly one request */
static void run_batch_test(char *hold_chunks, char *port, char *workers)
{
    struct test_ctx *ctx;
    int ret;
    int num;
    int i;
    int i_ffd;
    int o_ffd;
    struct flb_lib_out_cb cb;
    char *buf = "[1, {\"msg\":\"hello world\"}]";
    size_t size = strlen(buf);

    cb.cb   = callback_batch;
    cb.data = "\"count\":3";
    clear_output_num();
    batch_saw_count = 0;

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_input_set(ctx->flb, ctx->i_ffd, "tag", "lib", NULL);
    TEST_CHECK(ret == 0);

    /* receiver */
    i_ffd = flb_input(ctx->flb, (char *) "http", NULL);
    TEST_CHECK(i_ffd >= 0);
    ret = flb_input_set(ctx->flb, i_ffd,
                        "port", port,
                        "tag", "http",
                        "host", "127.0.0.1",
                        NULL);
    TEST_CHECK(ret == 0);

    o_ffd = flb_output(ctx->flb, (char *) "lib", &cb);
    TEST_CHECK(o_ffd >= 0);
    ret = flb_output_set(ctx->flb, o_ffd,
                         "match", "http",
                         "format", "json",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "lib",
                         "host", "127.0.0.1",
                         "port", port,
                         "format", "json",
                         "json_events_key", "events",
                         "batch_interval", "2",
                         "batch_hold_chunks", hold_chunks,
                         "workers", workers,
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* records arrive over several engine flushes (Flush 0.2) */
    for (i = 0; i < 3; i++) {
        ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf, size);
        TEST_CHECK(ret >= 0);
        flb_time_msleep(300);
    }

    /* first interval sends the batch, second one has nothing to send */
    flb_time_msleep(4500);

    num = get_output_num();
    if (!TEST_CHECK(num == 1)) {
        TEST_MSG("expected 1 request, got %d", num);
    }
    pthread_mutex_lock(&result_mutex);
    ret = batch_saw_count;
    pthread_mutex_unlock(&result_mutex);
    if (!TEST_CHECK(ret == 1)) {
        TEST_MSG("request did not contain the 3 buffered records");
    }

    test_ctx_destroy(ctx);
}

void flb_test_batch_interval()
{
    run_batch_test("off", "8889", "1");
}

/* chunks are held by the engine until the batch is delivered */
void flb_test_batch_hold_chunks()
{
    run_batch_test("on", "8890", "1");
}

/* without workers the flush callbacks and the timer run in the engine */
void flb_test_batch_interval_no_workers()
{
    run_batch_test("off", "8894", "0");
}

void flb_test_batch_hold_chunks_no_workers()
{
    run_batch_test("on", "8895", "0");
}

/* batch status events received through out_lib (JSON) */
struct status_counts {
    int total;
    int success;       /* 3 records sent in 1 attempt, HTTP 201, no error */
    int empty;         /* nothing to send */
    int retry;         /* 2 attempts, no response, an error reason */
    int kept_413;      /* rejected with 413 and kept for the next round */
    int dropped;
    int success_any;   /* any success event */
    int success_records;       /* records of all the success events */
    int first_success_records; /* records of the first success event */
};
static struct status_counts status_counts;

static int cb_status_event(void* data, size_t size, void* cb_data)
{
    flb_sds_t s;

    s = flb_sds_create_len(data, size);
    if (!s) {
        return 0;
    }

    pthread_mutex_lock(&result_mutex);
    status_counts.total++;
    if (strstr(s, "\"status\":\"success\"") && strstr(s, "\"records\":3") &&
        strstr(s, "\"attempts\":1") && strstr(s, "\"http_status\":201") &&
        strstr(s, "\"error\":null") && strstr(s, "\"batch_id\":\"")) {
        status_counts.success++;
    }
    if (strstr(s, "\"status\":\"empty\"") && strstr(s, "\"records\":0") &&
        strstr(s, "\"attempts\":0") && strstr(s, "\"http_status\":null")) {
        status_counts.empty++;
    }
    if (strstr(s, "\"status\":\"retry_next_interval\"") &&
        strstr(s, "\"attempts\":2") && strstr(s, "\"http_status\":null") &&
        !strstr(s, "\"error\":null")) {
        status_counts.retry++;
    }
    if (strstr(s, "\"status\":\"retry_next_interval\"") &&
        strstr(s, "\"http_status\":413") && strstr(s, "\"attempts\":1")) {
        status_counts.kept_413++;
    }
    if (strstr(s, "\"status\":\"dropped\"")) {
        status_counts.dropped++;
    }
    if (strstr(s, "\"status\":\"success\"")) {
        char *p = strstr(s, "\"records\":");
        int n = p ? atoi(p + 10) : 0;

        if (status_counts.success_any == 0) {
            status_counts.first_success_records = n;
        }
        status_counts.success_any++;
        status_counts.success_records += n;
    }
    pthread_mutex_unlock(&result_mutex);

    flb_sds_destroy(s);
    return 0;
}

static struct status_counts run_status_test(char *hold_chunks, char *port,
                                            int with_receiver, int records)
{
    int i;
    int ret;
    int i_ffd;
    int o_ffd;
    struct test_ctx *ctx;
    struct flb_lib_out_cb cb;
    struct status_counts result;
    char *buf = "[1, {\"msg\":\"hello world\"}]";
    size_t size = strlen(buf);

    cb.cb   = cb_status_event;
    cb.data = NULL;
    clear_output_num();
    memset(&status_counts, 0, sizeof(status_counts));

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_input_set(ctx->flb, ctx->i_ffd, "tag", "lib", NULL);
    TEST_CHECK(ret == 0);

    /* receiver: its records have no route, only the HTTP status matters */
    if (with_receiver) {
        i_ffd = flb_input(ctx->flb, (char *) "http", NULL);
        TEST_CHECK(i_ffd >= 0);
        ret = flb_input_set(ctx->flb, i_ffd,
                            "port", port,
                            "tag", "http",
                            "host", "127.0.0.1",
                            NULL);
        TEST_CHECK(ret == 0);
    }

    /* status events */
    o_ffd = flb_output(ctx->flb, (char *) "lib", &cb);
    TEST_CHECK(o_ffd >= 0);
    ret = flb_output_set(ctx->flb, o_ffd,
                         "match", "batch.status",
                         "format", "json",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "lib",
                         "host", "127.0.0.1",
                         "port", port,
                         "format", "json",
                         "json_events_key", "events",
                         "batch_interval", "2",
                         "batch_hold_chunks", hold_chunks,
                         "batch_status_tag", "batch.status",
                         "workers", "1",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    for (i = 0; i < records; i++) {
        ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf, size);
        TEST_CHECK(ret >= 0);
        flb_time_msleep(100);
    }

    /* first round sends the records, the next one finds nothing */
    flb_time_msleep(5000);

    pthread_mutex_lock(&result_mutex);
    result = status_counts;
    pthread_mutex_unlock(&result_mutex);

    test_ctx_destroy(ctx);

    return result;
}

static void check_status_success(char *hold_chunks, char *port)
{
    struct status_counts r;

    r = run_status_test(hold_chunks, port, FLB_TRUE, 3);
    if (!TEST_CHECK(r.success == 1)) {
        TEST_MSG("expected 1 success event, got %d (total %d)",
                 r.success, r.total);
    }
    if (!TEST_CHECK(r.empty >= 1)) {
        TEST_MSG("expected an empty event, got %d (total %d)",
                 r.empty, r.total);
    }
}

/* a batch round emits 'success', a round with nothing to send 'empty' */
void flb_test_batch_status_events()
{
    check_status_success("off", "8892");
}

void flb_test_batch_status_events_hold()
{
    check_status_success("on", "8893");
}

/* a failed round emits 'retry_next_interval' with the reason */
void flb_test_batch_status_retry()
{
    struct status_counts r;

    /* nothing listens on this port */
    r = run_status_test("off", "8899", FLB_FALSE, 2);
    if (!TEST_CHECK(r.retry >= 1)) {
        TEST_MSG("expected a retry_next_interval event, got %d (total %d)",
                 r.retry, r.total);
    }
    TEST_CHECK(r.success == 0);
}

/*
 * Runs a batch output with status events against an in_http receiver. The
 * receiver buffer limit can be lowered to make it answer 413.
 */
static struct status_counts run_limit_test(char *port, char *hold_chunks,
                                           char *max_chunks,
                                           char *receiver_max_size,
                                           int records, int wait_ms)
{
    int i;
    int ret;
    int i_ffd;
    int o_ffd;
    struct test_ctx *ctx;
    struct flb_lib_out_cb cb;
    struct status_counts result;
    char *buf = "[1, {\"msg\":\"hello world\"}]";
    size_t size = strlen(buf);

    cb.cb   = cb_status_event;
    cb.data = NULL;
    clear_output_num();
    memset(&status_counts, 0, sizeof(status_counts));

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_input_set(ctx->flb, ctx->i_ffd, "tag", "lib", NULL);
    TEST_CHECK(ret == 0);

    i_ffd = flb_input(ctx->flb, (char *) "http", NULL);
    TEST_CHECK(i_ffd >= 0);
    ret = flb_input_set(ctx->flb, i_ffd,
                        "port", port,
                        "tag", "http",
                        "host", "127.0.0.1",
                        NULL);
    TEST_CHECK(ret == 0);
    if (receiver_max_size) {
        ret = flb_input_set(ctx->flb, i_ffd,
                            "buffer_chunk_size", "64",
                            "buffer_max_size", receiver_max_size,
                            NULL);
        TEST_CHECK(ret == 0);
    }

    o_ffd = flb_output(ctx->flb, (char *) "lib", &cb);
    TEST_CHECK(o_ffd >= 0);
    ret = flb_output_set(ctx->flb, o_ffd,
                         "match", "batch.status",
                         "format", "json",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "lib",
                         "host", "127.0.0.1",
                         "port", port,
                         "format", "json",
                         "json_events_key", "events",
                         "batch_interval", "2",
                         "batch_hold_chunks", hold_chunks,
                         "batch_hold_max_chunks", max_chunks,
                         "batch_status_tag", "batch.status",
                         "retry_limit", "no_limits",
                         "http.response_timeout", "2s",
                         "workers", "1",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* one record per engine flush (Flush 0.2): one chunk per record */
    for (i = 0; i < records; i++) {
        ret = flb_lib_push(ctx->flb, ctx->i_ffd, (char *) buf, size);
        TEST_CHECK(ret >= 0);
        flb_time_msleep(300);
    }

    flb_time_msleep(wait_ms);

    pthread_mutex_lock(&result_mutex);
    result = status_counts;
    pthread_mutex_unlock(&result_mutex);

    test_ctx_destroy(ctx);

    return result;
}

/* a batch larger than the receiver accepts (413) is kept, not dropped */
void flb_test_batch_413_kept()
{
    struct status_counts r;

    r = run_limit_test("8896", "on", "512", "128", 10, 4000);
    if (!TEST_CHECK(r.kept_413 >= 1)) {
        TEST_MSG("expected a retry_next_interval event with HTTP 413, "
                 "got %d (total %d)", r.kept_413, r.total);
    }
    if (!TEST_CHECK(r.dropped == 0)) {
        TEST_MSG("the batch was dropped %d time(s)", r.dropped);
    }
}

/* chunks above batch_hold_max_chunks wait in storage and are sent later */
void flb_test_batch_hold_max_chunks()
{
    struct status_counts r;

    r = run_limit_test("8897", "on", "2", NULL, 5, 20000);
    if (!TEST_CHECK(r.first_success_records >= 1 &&
                    r.first_success_records <= 2)) {
        TEST_MSG("first batch had %d records, expected at most 2 chunks",
                 r.first_success_records);
    }
    if (!TEST_CHECK(r.success_records == 5)) {
        TEST_MSG("expected all 5 records delivered, got %d in %d batches",
                 r.success_records, r.success_any);
    }
    TEST_CHECK(r.dropped == 0);
}

/* the status tag must not be matched by the batch output itself */
void flb_test_batch_status_tag_loop()
{
    int ret;
    struct test_ctx *ctx;

    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_output_set(ctx->flb, ctx->o_ffd,
                         "match", "*",
                         "host", "127.0.0.1",
                         "port", "8899",
                         "batch_interval", "2",
                         "batch_status_tag", "batch.status",
                         NULL);
    TEST_CHECK(ret == 0);

    ret = flb_start(ctx->flb);
    if (!TEST_CHECK(ret == -1)) {
        TEST_MSG("the engine started although the status tag loops back");
        flb_stop(ctx->flb);
    }

    flb_destroy(ctx->flb);
    flb_free(ctx);
}

/* test to make sure out_http is always able to work with in_http by default. */
void flb_test_in_http()
{
    struct test_ctx *ctx;
    int ret;
    int num;
    int i_ffd;
    int o_ffd;
    int trys;
    struct flb_lib_out_cb cb;
    char *buf = "[1, {\"msg\":\"hello world\"}]";
    size_t size = strlen(buf);

    cb.cb   = callback_test;
    cb.data = NULL;
    clear_output_num();


    ctx = test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        TEST_MSG("test_ctx_create failed");
        exit(EXIT_FAILURE);
    }

    ret = flb_input_set(ctx->flb,
                        ctx->i_ffd,
                        "tag", "lib",
                        NULL);
    TEST_CHECK(ret == 0);

    /* Input */
    i_ffd = flb_input(ctx->flb, (char *) "http", NULL);
    TEST_CHECK(i_ffd >= 0);

    ret = flb_input_set(ctx->flb,
                        i_ffd,
                        "port", "8888",
                        "tag", "http",
                        "host", "127.0.0.1",
                        NULL);
    TEST_CHECK(ret == 0);

    /* Output */
    o_ffd = flb_output(ctx->flb, (char *) "lib", &cb);
    TEST_CHECK(o_ffd >= 0);
    ret = flb_output_set(ctx->flb,
                         o_ffd,
                         "match", "http",
                         NULL);
    TEST_CHECK(ret == 0);

    /* explicitly do not set anything beyond match, port and localhost
     * to be sure the default options work with in_http.
     */
    ret = flb_output_set(ctx->flb,
                         ctx->o_ffd,
                         "match", "lib",
                         "host", "127.0.0.1",
                         "port", "8888",
                         NULL);
    TEST_CHECK(ret == 0);

    /* Start the engines */
    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);

    /* Ingest data sample */
    ret = flb_lib_push(ctx->flb,
                       ctx->i_ffd,
                       (char *) buf,
                       size);
    TEST_CHECK(ret >= 0);

    /* try several times to detect the record being flushed. */
    for (trys = 0, num = 0; trys < 20 && num <= 0; trys++) {
        num = get_output_num();
        if (num <= 0) {
            flb_time_msleep(500);
        }
    }

    if (!TEST_CHECK(num > 0))  {
        TEST_MSG("no outputs");
    }

    test_ctx_destroy(ctx);
}

/* Test list */
TEST_LIST = {
    {"format_msgpack" , flb_test_format_msgpack},
    {"format_json" , flb_test_format_json},
    {"format_json_stream" , flb_test_format_json_stream},
    {"format_json_lines" , flb_test_format_json_lines},
    {"format_gelf" , flb_test_format_gelf},
    {"format_gelf_host_key" , flb_test_format_gelf_host_key},
    {"format_gelf_timestamp_key" , flb_test_format_gelf_timestamp_key},
    {"format_gelf_full_message_key" , flb_test_format_gelf_full_message_key},
    {"format_gelf_level_key" , flb_test_format_gelf_level_key},
    {"set_json_date_key" , flb_test_set_json_date_key},
    {"disable_json_date_key" , flb_test_disable_json_date_key},
    {"json_date_format_epoch" , flb_test_json_date_format_epoch},
    {"json_date_format_iso8601" , flb_test_json_date_format_iso8601},
    {"json_date_format_java_sql_timestamp" , flb_test_json_date_format_java_sql_timestamp},
    {"json_events_key" , flb_test_json_events_key},
    {"json_events_key_count_key" , flb_test_json_events_key_count_key},
    {"json_events_key_disable_count" , flb_test_json_events_key_disable_count},
    {"batch_interval", flb_test_batch_interval},
    {"batch_hold_chunks", flb_test_batch_hold_chunks},
    {"batch_interval_no_workers", flb_test_batch_interval_no_workers},
    {"batch_hold_chunks_no_workers", flb_test_batch_hold_chunks_no_workers},
    {"batch_status_events", flb_test_batch_status_events},
    {"batch_status_events_hold", flb_test_batch_status_events_hold},
    {"batch_status_retry", flb_test_batch_status_retry},
    {"batch_status_tag_loop", flb_test_batch_status_tag_loop},
    {"batch_413_kept", flb_test_batch_413_kept},
    {"batch_hold_max_chunks", flb_test_batch_hold_max_chunks},
    {"in_http", flb_test_in_http},
    {NULL, NULL}
};
