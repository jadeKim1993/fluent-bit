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

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_output.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_str.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_mp.h>
#include <fluent-bit/flb_scheduler.h>
#include <fluent-bit/flb_stream.h>
#include <fluent-bit/flb_upstream_conn.h>
#include <fluent-bit/flb_input.h>
#include <fluent-bit/flb_input_chunk.h>
#include <fluent-bit/flb_log_event_encoder.h>
#include <fluent-bit/flb_random.h>
#include <fluent-bit/flb_sds.h>

#include <fluent-bit/flb_gzip.h>
#include <fluent-bit/flb_snappy.h>
#include <fluent-bit/flb_zstd.h>

#include <fluent-bit/flb_record_accessor.h>
#include <fluent-bit/flb_log_event_decoder.h>
#include <msgpack.h>

#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
#include <fluent-bit/flb_aws_credentials.h>
#include <fluent-bit/flb_signv4.h>
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "http.h"
#include "http_conf.h"

#include <fluent-bit/flb_callback.h>

static int cb_http_init(struct flb_output_instance *ins,
                        struct flb_config *config, void *data)
{
    struct flb_out_http *ctx = NULL;
    (void) data;

    ctx = flb_http_conf_create(ins, config);
    if (!ctx) {
        return -1;
    }

    /* Set the plugin context */
    flb_output_set_context(ins, ctx);

    /*
     * This plugin instance uses the HTTP client interface, let's register
     * it debugging callbacks.
     */
    flb_output_set_http_debug_callbacks(ins);

    return 0;
}

static void append_headers(struct flb_http_client *c,
                           char **headers)
{
    int i;
    char *header_key;
    char *header_value;

    i = 0;
    header_key = NULL;
    header_value = NULL;
    while (*headers) {
        if (i % 2 == 0) {
            header_key = *headers;
        }
        else {
            header_value = *headers;
        }
        if (header_key && header_value) {
            flb_http_add_header(c,
                                header_key,
                                strlen(header_key),
                                header_value,
                                strlen(header_value));
            flb_free(header_key);
            flb_free(header_value);
            header_key = NULL;
            header_value = NULL;
        }
        headers++;
        i++;
    }
}

/* Optional request details, used to report the result of a batch */
struct http_request_info {
    const char *batch_id;           /* sent as X-Batch-Id when set */
    int http_status;                /* response status, 0 if none */
    char error[128];                /* reason of a failure */
};

static int http_request(struct flb_out_http *ctx,
                        const void *body, size_t body_len,
                        const char *tag, int tag_len,
                        char **headers, struct http_request_info *info)
{
    int ret = 0;
    int out_ret = FLB_OK;
    int compressed = FLB_FALSE;
    size_t b_sent;
    void *payload_buf = NULL;
    size_t payload_size = 0;
    struct flb_upstream *u;
    struct flb_connection *u_conn;
    struct flb_http_client *c;
    struct mk_list *head;
    struct flb_config_map_val *mv;
    struct flb_slist_entry *key = NULL;
    struct flb_slist_entry *val = NULL;
    flb_sds_t signature = NULL;

    if (info) {
        info->http_status = 0;
        info->error[0] = '\0';
    }

    /* Get upstream context and connection */
    u = ctx->u;
    u_conn = flb_upstream_conn_get(u);
    if (!u_conn) {
        flb_plg_error(ctx->ins, "no upstream connections available to %s:%i",
                      u->tcp_host, u->tcp_port);
        if (info) {
            snprintf(info->error, sizeof(info->error),
                     "no connection available to %s:%i",
                     u->tcp_host, u->tcp_port);
        }
        return FLB_RETRY;
    }

    /* Map payload */
    payload_buf = (void *) body;
    payload_size = body_len;

    /* Should we compress the payload ? */
    ret = 0;
    if (ctx->compress_gzip == FLB_TRUE) {
        ret = flb_gzip_compress((void *) body, body_len,
                                &payload_buf, &payload_size);
        if (ret == 0) {
            compressed = FLB_TRUE;
        }
    }
    else if (ctx->compress_snappy == FLB_TRUE) {
        ret = flb_snappy_compress((void *) body, body_len,
                                  (char **) &payload_buf, &payload_size);
        if (ret == 0) {
            compressed = FLB_TRUE;
        }
    }
    else if (ctx->compress_zstd == FLB_TRUE) {
        ret = flb_zstd_compress((void *) body, body_len,
                                &payload_buf, &payload_size);
        if (ret == 0) {
            compressed = FLB_TRUE;
        }
    }

    if (ret == -1) {
        flb_plg_warn(ctx->ins, "could not compress payload, sending as it is");
        compressed = FLB_FALSE;
    }


    /* Create HTTP client context */
    c = flb_http_client(u_conn, ctx->http_method, ctx->uri,
                        payload_buf, payload_size,
                        ctx->host, ctx->port,
                        ctx->proxy, 0);

    if (c == NULL) {
        flb_plg_error(ctx->ins, "[http_client] failed to create HTTP client");
        if (info) {
            snprintf(info->error, sizeof(info->error),
                     "could not create the HTTP client");
        }
        if (payload_buf != body) {
            flb_free(payload_buf);
        }

        if (u_conn) {
            flb_upstream_conn_release(u_conn);
        }

        return FLB_RETRY;
    }

    if (c->proxy.host) {
        flb_plg_debug(ctx->ins, "[http_client] proxy host: %s port: %i",
                      c->proxy.host, c->proxy.port);
    }

    /* Allow duplicated headers ? */
    flb_http_allow_duplicated_headers(c, ctx->allow_dup_headers);

    /*
     * Direct assignment of the callback context to the HTTP client context.
     * This needs to be improved through a more clean API.
     */
    c->cb_ctx = ctx->ins->callback;

    flb_http_set_response_timeout(c, ctx->response_timeout);

    if (ctx->read_idle_timeout > 0) {
        flb_http_set_read_idle_timeout(c, ctx->read_idle_timeout);
    }
    else {
        flb_http_set_read_idle_timeout(c, ctx->ins->net_setup.io_timeout);
    }

    /* Append headers */
    if (headers) {
        append_headers(c, headers);
    }
    else if ((ctx->out_format == FLB_PACK_JSON_FORMAT_JSON) ||
        (ctx->out_format == FLB_PACK_JSON_FORMAT_STREAM) ||
        (ctx->out_format == FLB_HTTP_OUT_GELF)) {
        flb_http_add_header(c,
                            FLB_HTTP_CONTENT_TYPE,
                            sizeof(FLB_HTTP_CONTENT_TYPE) - 1,
                            FLB_HTTP_MIME_JSON,
                            sizeof(FLB_HTTP_MIME_JSON) - 1);
    }
    else if (ctx->out_format == FLB_PACK_JSON_FORMAT_LINES) {
        flb_http_add_header(c,
                            FLB_HTTP_CONTENT_TYPE,
                            sizeof(FLB_HTTP_CONTENT_TYPE) - 1,
                            FLB_HTTP_MIME_NDJSON,
                            sizeof(FLB_HTTP_MIME_NDJSON) - 1);
    }
    else if (ctx->out_format == FLB_HTTP_OUT_MSGPACK) {
        flb_http_add_header(c,
                            FLB_HTTP_CONTENT_TYPE,
                            sizeof(FLB_HTTP_CONTENT_TYPE) - 1,
                            FLB_HTTP_MIME_MSGPACK,
                            sizeof(FLB_HTTP_MIME_MSGPACK) - 1);
    }

    if (ctx->header_tag) {
        flb_http_add_header(c,
                            ctx->header_tag,
                            flb_sds_len(ctx->header_tag),
                            tag, tag_len);
    }

    /* Content Encoding: gzip */
    if (compressed == FLB_TRUE) {
        if (ctx->compress_gzip == FLB_TRUE) {
            flb_http_set_content_encoding_gzip(c);
        }
        else if (ctx->compress_snappy == FLB_TRUE) {
            flb_http_set_content_encoding_snappy(c);
        }
        else if (ctx->compress_zstd == FLB_TRUE) {
            flb_http_set_content_encoding_zstd(c);
        }
    }

    /* Basic Auth headers */
    if (ctx->http_user && ctx->http_passwd) {
        flb_http_basic_auth(c, ctx->http_user, ctx->http_passwd);
    }

    flb_http_add_header(c, "User-Agent", 10, "Fluent-Bit", 10);

    flb_config_map_foreach(head, mv, ctx->headers) {
        key = mk_list_entry_first(mv->val.list, struct flb_slist_entry, _head);
        val = mk_list_entry_last(mv->val.list, struct flb_slist_entry, _head);

        flb_http_add_header(c,
                            key->str, flb_sds_len(key->str),
                            val->str, flb_sds_len(val->str));
    }

    /* Lets the receiver match the request with its batch status event */
    if (info && info->batch_id) {
        flb_http_add_header(c, "X-Batch-Id", 10,
                            info->batch_id, strlen(info->batch_id));
    }

#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
    /* AWS SigV4 headers */
    if (ctx->has_aws_auth == FLB_TRUE) {
        flb_plg_debug(ctx->ins, "signing request with AWS Sigv4");
        signature = flb_signv4_do(c,
                                  FLB_TRUE,  /* normalize URI ? */
                                  FLB_TRUE,  /* add x-amz-date header ? */
                                  time(NULL),
                                  (char *) ctx->aws_region,
                                  (char *) ctx->aws_service,
                                  0, NULL,
                                  ctx->aws_provider);

        if (!signature) {
            flb_plg_error(ctx->ins, "could not sign request with sigv4");
            if (info) {
                snprintf(info->error, sizeof(info->error),
                         "could not sign the request with sigv4");
            }
            out_ret = FLB_RETRY;
            goto cleanup;
        }
        flb_sds_destroy(signature);
    }
#endif
#endif

    ret = flb_http_do_with_oauth2(c, &b_sent, ctx->oauth2_ctx);
    if (ret == 0) {
        if (info) {
            info->http_status = c->resp.status;
        }

        /*
         * Only allow the following HTTP status:
         *
         * - 200: OK
         * - 201: Created
         * - 202: Accepted
         * - 203: no authorative resp
         * - 204: No Content
         * - 205: Reset content
         *
         */
        if (c->resp.status < 200 || c->resp.status > 205) {
            if (info) {
                snprintf(info->error, sizeof(info->error),
                         "HTTP status %i", c->resp.status);
            }
            if (ctx->log_response_payload &&
                c->resp.payload && c->resp.payload_size > 0) {
                flb_plg_error(ctx->ins, "%s:%i, HTTP status=%i\n%s",
                              ctx->host, ctx->port,
                              c->resp.status, c->resp.payload);
            }
            else {
                flb_plg_error(ctx->ins, "%s:%i, HTTP status=%i",
                              ctx->host, ctx->port, c->resp.status);
            }
            if (c->resp.status >= 400 && c->resp.status < 500 &&
                c->resp.status != 429 && c->resp.status != 408) {
                flb_plg_warn(ctx->ins, "could not flush records to %s:%i (http_do=%i), "
                                "chunk will not be retried",
                                ctx->host, ctx->port, ret);
                out_ret = FLB_ERROR;
            }
            else {
                out_ret = FLB_RETRY;
            }
        }
        else {
            if (ctx->log_response_payload &&
                c->resp.payload && c->resp.payload_size > 0) {
                flb_plg_info(ctx->ins, "%s:%i, HTTP status=%i\n%s",
                             ctx->host, ctx->port,
                             c->resp.status, c->resp.payload);
            }
            else {
                flb_plg_info(ctx->ins, "%s:%i, HTTP status=%i",
                             ctx->host, ctx->port,
                             c->resp.status);
            }
        }
    }
    else {
        flb_plg_error(ctx->ins, "could not flush records to %s:%i (http_do=%i)",
                      ctx->host, ctx->port, ret);
        if (info) {
            snprintf(info->error, sizeof(info->error),
                     "request to %s:%i failed (connection error or timeout)",
                     ctx->host, ctx->port);
        }

        /*
         * Transport failure (e.g: the server closed an idle keep-alive
         * connection): do not hand this connection back to the pool, so a
         * retry uses a new one.
         */
        flb_upstream_conn_recycle(u_conn, FLB_FALSE);
        out_ret = FLB_RETRY;
    }

cleanup:
    /*
     * If the payload buffer is different than incoming records in body, means
     * we generated a different payload and must be freed.
     */
    if (payload_buf != body) {
        flb_free(payload_buf);
    }

    /* Destroy HTTP client context */
    flb_http_client_destroy(c);

    /* Release the TCP connection */
    flb_upstream_conn_release(u_conn);

    return out_ret;
}

static int compose_payload_gelf(struct flb_out_http *ctx,
                                const char *data, uint64_t bytes,
                                void **out_body, size_t *out_size)
{
    flb_sds_t s;
    flb_sds_t tmp = NULL;
    size_t size = 0;
    msgpack_object map;
    struct flb_log_event_decoder log_decoder;
    struct flb_log_event log_event;
    int ret;

    size = bytes * 1.5;

    /* Allocate buffer for our new payload */
    s = flb_sds_create_size(size);
    if (!s) {
        flb_plg_error(ctx->ins, "flb_sds_create_size failed");
        return FLB_RETRY;
    }

    ret = flb_log_event_decoder_init(&log_decoder, (char *) data, bytes);

    if (ret != FLB_EVENT_DECODER_SUCCESS) {
        flb_plg_error(ctx->ins,
                      "Log event decoder initialization error : %d", ret);

        flb_sds_destroy(s);

        return FLB_RETRY;
    }

    while ((ret = flb_log_event_decoder_next(
                    &log_decoder,
                    &log_event)) == FLB_EVENT_DECODER_SUCCESS) {
        map = *log_event.body;

        tmp = flb_msgpack_to_gelf(&s, &map,
                                  &log_event.timestamp,
                                  &(ctx->gelf_fields));
        if (!tmp) {
            flb_plg_error(ctx->ins, "error encoding to GELF");

            flb_sds_destroy(s);
            flb_log_event_decoder_destroy(&log_decoder);

            return FLB_ERROR;
        }

        /* Append new line */
        tmp = flb_sds_cat(s, "\n", 1);
        if (!tmp) {
            flb_plg_error(ctx->ins, "error concatenating records");

            flb_sds_destroy(s);
            flb_log_event_decoder_destroy(&log_decoder);

            return FLB_RETRY;
        }

        s = tmp;
    }

    *out_body = s;
    *out_size = flb_sds_len(s);

    flb_log_event_decoder_destroy(&log_decoder);

    return FLB_OK;
}

/*
 * Wrap a JSON array of records into an envelope object:
 *
 *   {"<count_key>": <records>, "<events_key>": [ ... ]}
 *
 * The count member is omitted when count_key is NULL. On success the
 * input 'json' buffer is released and the new buffer is returned.
 */
static flb_sds_t wrap_json_events(struct flb_out_http *ctx, flb_sds_t json,
                                  int records)
{
    size_t size;
    flb_sds_t out;
    flb_sds_t tmp;

    size = flb_sds_len(json) + flb_sds_len(ctx->json_events_key) + 32;
    if (ctx->count_key) {
        size += flb_sds_len(ctx->count_key) + 16;
    }

    out = flb_sds_create_size(size);
    if (!out) {
        flb_errno();
        return NULL;
    }

    tmp = flb_sds_cat(out, "{", 1);
    if (!tmp) {
        goto error;
    }
    out = tmp;

    if (ctx->count_key) {
        tmp = flb_sds_cat(out, "\"", 1);
        if (!tmp) {
            goto error;
        }
        out = tmp;

        tmp = flb_sds_cat_utf8(&out, ctx->count_key,
                               flb_sds_len(ctx->count_key));
        if (!tmp) {
            goto error;
        }

        tmp = flb_sds_printf(&out, "\":%d,", records);
        if (!tmp) {
            goto error;
        }
    }

    tmp = flb_sds_cat(out, "\"", 1);
    if (!tmp) {
        goto error;
    }
    out = tmp;

    tmp = flb_sds_cat_utf8(&out, ctx->json_events_key,
                           flb_sds_len(ctx->json_events_key));
    if (!tmp) {
        goto error;
    }

    tmp = flb_sds_cat(out, "\":", 2);
    if (!tmp) {
        goto error;
    }
    out = tmp;

    tmp = flb_sds_cat(out, json, flb_sds_len(json));
    if (!tmp) {
        goto error;
    }
    out = tmp;

    tmp = flb_sds_cat(out, "}", 1);
    if (!tmp) {
        goto error;
    }
    out = tmp;

    flb_sds_destroy(json);
    return out;

error:
    flb_errno();
    flb_sds_destroy(out);
    return NULL;
}

static int compose_payload(struct flb_out_http *ctx,
                           const void *in_body, size_t in_size,
                           void **out_body, size_t *out_size,
                           struct flb_config *config)
{
    int records;
    flb_sds_t encoded;
    flb_sds_t wrapped;

    *out_body = NULL;
    *out_size = 0;

    if ((ctx->out_format == FLB_PACK_JSON_FORMAT_JSON) ||
        (ctx->out_format == FLB_PACK_JSON_FORMAT_STREAM) ||
        (ctx->out_format == FLB_PACK_JSON_FORMAT_LINES)) {

        encoded = flb_pack_msgpack_to_json_format(in_body,
                                                  in_size,
                                                  ctx->out_format,
                                                  ctx->json_date_format,
                                                  ctx->date_key,
                                                  config->json_escape_unicode);
        if (encoded == NULL) {
            flb_plg_error(ctx->ins, "failed to convert json");
            return FLB_ERROR;
        }

        if (ctx->json_events_key &&
            ctx->out_format == FLB_PACK_JSON_FORMAT_JSON) {
            records = flb_mp_count_log_records(in_body, in_size);
            wrapped = wrap_json_events(ctx, encoded, records);
            if (!wrapped) {
                flb_plg_error(ctx->ins, "failed to wrap json events");
                flb_sds_destroy(encoded);
                return FLB_RETRY;
            }
            encoded = wrapped;
        }

        *out_body = (void*)encoded;
        *out_size = flb_sds_len(encoded);
    }
    else if (ctx->out_format == FLB_HTTP_OUT_GELF) {
        return compose_payload_gelf(ctx, in_body, in_size, out_body, out_size);
    }
    else {
        /* Nothing to do, if the format is msgpack */
        *out_body = (void *)in_body;
        *out_size = in_size;
    }

    return FLB_OK;
}

/*
 * Milliseconds from a monotonic clock: batches are scheduled with it so a
 * system clock change (NTP step, VM resume) does not delay or skip rounds.
 */
static uint64_t batch_clock_ms(void)
{
#ifdef FLB_SYSTEM_WINDOWS
    return (uint64_t) GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Append the chunk records to the batch buffer */
static int batch_append(struct flb_out_http *ctx,
                        struct flb_event_chunk *event_chunk)
{
    size_t grow;
    flb_sds_t tmp;

    /* a single chunk is always accepted on an empty buffer */
    if (ctx->batch_max_size > 0 && ctx->batch_buf &&
        flb_sds_len(ctx->batch_buf) + event_chunk->size > ctx->batch_max_size) {
        if (!ctx->batch_full_warned) {
            flb_plg_warn(ctx->ins, "batch is full (%zu bytes, batch_max_size "
                         "%zu): further chunks are retried by the engine and "
                         "go in a later batch", flb_sds_len(ctx->batch_buf),
                         ctx->batch_max_size);
            ctx->batch_full_warned = FLB_TRUE;
        }
        return FLB_RETRY;
    }

    if (!ctx->batch_buf) {
        ctx->batch_buf = flb_sds_create_size(event_chunk->size);
        if (!ctx->batch_buf) {
            flb_errno();
            return FLB_RETRY;
        }

        ctx->batch_tag = flb_sds_create_len(event_chunk->tag,
                                            flb_sds_len(event_chunk->tag));
        if (!ctx->batch_tag) {
            flb_errno();
            flb_sds_destroy(ctx->batch_buf);
            ctx->batch_buf = NULL;
            return FLB_RETRY;
        }
    }

    /*
     * Chunks are appended one by one for a whole interval: grow the buffer
     * geometrically instead of by the exact size to avoid a reallocation
     * (and possibly a copy of the whole batch) on every append.
     */
    if (flb_sds_avail(ctx->batch_buf) < event_chunk->size) {
        grow = flb_sds_alloc(ctx->batch_buf);
        if (ctx->batch_max_size > 0 &&
            flb_sds_alloc(ctx->batch_buf) + grow > ctx->batch_max_size) {
            grow = ctx->batch_max_size - flb_sds_alloc(ctx->batch_buf);
        }
        if (grow < event_chunk->size) {
            grow = event_chunk->size;
        }

        tmp = flb_sds_increase(ctx->batch_buf, grow);
        if (!tmp) {
            flb_errno();
            return FLB_RETRY;
        }
        ctx->batch_buf = tmp;
    }

    tmp = flb_sds_cat(ctx->batch_buf, event_chunk->data, event_chunk->size);
    if (!tmp) {
        flb_errno();
        return FLB_RETRY;
    }
    ctx->batch_buf = tmp;

    return FLB_OK;
}

/* Details of one batch round, reported as a batch status event */
struct batch_report {
    char batch_id[160];
    int records;
    int carried_over;
    size_t bytes;
    int attempts;
    int64_t duration_ms;
    uint64_t started_ms;            /* monotonic, for the duration */
    struct flb_time started;
    struct flb_time first;          /* time of the first record */
    struct flb_time last;           /* time of the last record */
    struct http_request_info req;
};

static void batch_report_init(struct flb_out_http *ctx,
                              struct batch_report *rep)
{
    memset(rep, 0, sizeof(struct batch_report));
    flb_time_get(&rep->started);
    rep->started_ms = batch_clock_ms();

    /* <output>-<start epoch>-<sequence>-<process token> */
    ctx->batch_seq++;
    snprintf(rep->batch_id, sizeof(rep->batch_id), "%s-%lld-%llu-%s",
             flb_output_name(ctx->ins),
             (long long) rep->started.tm.tv_sec,
             (unsigned long long) ctx->batch_seq,
             ctx->batch_token);

    rep->carried_over = ctx->batch_carried;
    if (ctx->status_ins) {
        rep->req.batch_id = rep->batch_id;
    }
}

/* Count the records of a batch and get the time of the first and last one */
static int batch_scan(const char *buf, size_t size,
                      struct flb_time *first, struct flb_time *last)
{
    int count = 0;
    struct flb_log_event log_event;
    struct flb_log_event_decoder decoder;

    flb_time_zero(first);
    flb_time_zero(last);

    if (flb_log_event_decoder_init(&decoder, (char *) buf, size) !=
        FLB_EVENT_DECODER_SUCCESS) {
        return 0;
    }

    while (flb_log_event_decoder_next(&decoder, &log_event) ==
           FLB_EVENT_DECODER_SUCCESS) {
        if (count == 0) {
            flb_time_copy(first, &log_event.timestamp);
        }
        flb_time_copy(last, &log_event.timestamp);
        count++;
    }

    flb_log_event_decoder_destroy(&decoder);

    return count;
}

/* Append 'key: <ISO 8601 UTC time>', or 'key: null' when the time is unset */
static int batch_append_time(struct flb_log_event_encoder *enc,
                             char *key, struct flb_time *t, int set)
{
    int ret;
    size_t len;
    time_t sec;
    struct tm tm;
    char buf[40];

    ret = flb_log_event_encoder_append_body_cstring(enc, key);
    if (!set) {
        return ret | flb_log_event_encoder_append_body_null(enc);
    }

    sec = t->tm.tv_sec;
    gmtime_r(&sec, &tm);
    len = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf + len, sizeof(buf) - len, ".%03ldZ",
             (long) (t->tm.tv_nsec / 1000000));

    return ret | flb_log_event_encoder_append_body_cstring(enc, buf);
}

/*
 * Emit the result of a batch round as a record tagged with batch_status_tag.
 * It is queued in the ring buffer of the status input, which is safe from
 * the worker thread; the engine turns it into a regular chunk.
 */
static void batch_emit_status(struct flb_out_http *ctx,
                              struct flb_config *config,
                              char *status, struct batch_report *rep)
{
    int ret;
    struct flb_log_event_encoder enc;

    if (!ctx->status_ins) {
        return;
    }

    /* the engine no longer collects records */
    if (config->is_running == FLB_FALSE) {
        return;
    }

    ret = flb_log_event_encoder_init(&enc, FLB_LOG_EVENT_FORMAT_DEFAULT);
    if (ret != FLB_EVENT_ENCODER_SUCCESS) {
        flb_plg_error(ctx->ins, "cannot initialize the batch status encoder");
        return;
    }

    ret = flb_log_event_encoder_begin_record(&enc);
    ret |= flb_log_event_encoder_set_current_timestamp(&enc);
    ret |= flb_log_event_encoder_append_body_values(&enc,
               FLB_LOG_EVENT_CSTRING_VALUE("batch_id"),
               FLB_LOG_EVENT_CSTRING_VALUE(rep->batch_id),
               FLB_LOG_EVENT_CSTRING_VALUE("output"),
               FLB_LOG_EVENT_CSTRING_VALUE((char *) flb_output_name(ctx->ins)),
               FLB_LOG_EVENT_CSTRING_VALUE("status"),
               FLB_LOG_EVENT_CSTRING_VALUE(status),
               FLB_LOG_EVENT_CSTRING_VALUE("http_status"));
    if (rep->req.http_status > 0) {
        ret |= flb_log_event_encoder_append_body_int64(&enc,
                                                       rep->req.http_status);
    }
    else {
        ret |= flb_log_event_encoder_append_body_null(&enc);
    }
    ret |= flb_log_event_encoder_append_body_values(&enc,
               FLB_LOG_EVENT_CSTRING_VALUE("records"),
               FLB_LOG_EVENT_INT64_VALUE(rep->records),
               FLB_LOG_EVENT_CSTRING_VALUE("carried_over_records"),
               FLB_LOG_EVENT_INT64_VALUE(rep->carried_over),
               FLB_LOG_EVENT_CSTRING_VALUE("bytes"),
               FLB_LOG_EVENT_INT64_VALUE((int64_t) rep->bytes),
               FLB_LOG_EVENT_CSTRING_VALUE("attempts"),
               FLB_LOG_EVENT_INT64_VALUE(rep->attempts));
    ret |= batch_append_time(&enc, "started_at", &rep->started, FLB_TRUE);
    ret |= flb_log_event_encoder_append_body_values(&enc,
               FLB_LOG_EVENT_CSTRING_VALUE("duration_ms"),
               FLB_LOG_EVENT_INT64_VALUE(rep->duration_ms));
    ret |= batch_append_time(&enc, "first_record_time", &rep->first,
                             rep->records > 0);
    ret |= batch_append_time(&enc, "last_record_time", &rep->last,
                             rep->records > 0);
    ret |= flb_log_event_encoder_append_body_cstring(&enc, "error");
    if (rep->req.error[0] != '\0') {
        ret |= flb_log_event_encoder_append_body_cstring(&enc, rep->req.error);
    }
    else {
        ret |= flb_log_event_encoder_append_body_null(&enc);
    }
    ret |= flb_log_event_encoder_commit_record(&enc);

    if (ret != FLB_EVENT_ENCODER_SUCCESS) {
        flb_plg_error(ctx->ins, "cannot encode the batch status event");
    }
    else {
        ret = flb_input_chunk_ring_buffer_enqueue(ctx->status_ins,
                                                  FLB_INPUT_LOGS, 1,
                                                  ctx->batch_status_tag,
                                                  flb_sds_len(ctx->batch_status_tag),
                                                  enc.output_buffer,
                                                  enc.output_length);
        if (ret != 0) {
            flb_plg_warn(ctx->ins, "could not queue batch status event '%s'",
                         rep->batch_id);
        }
    }

    flb_log_event_encoder_destroy(&enc);
}

/*
 * Compose the payload of a batch and send it. A failed request is retried
 * once right away on a new connection: the connection sat idle for a whole
 * interval and a kept-alive one may have been closed by the server. Not on
 * shutdown: a receiver that does not answer would hold the shutdown for two
 * response timeouts.
 */
static int batch_request(struct flb_out_http *ctx,
                         const char *data, size_t size,
                         const char *tag, int tag_len, int stopping,
                         struct batch_report *rep, struct flb_config *config)
{
    int ret;
    size_t out_size;
    void *out_body;

    ret = compose_payload(ctx, data, size, &out_body, &out_size, config);
    if (ret != FLB_OK) {
        snprintf(rep->req.error, sizeof(rep->req.error),
                 "could not compose the payload");
        return ret;
    }
    rep->bytes = out_size;

    rep->attempts = 1;
    ret = http_request(ctx, out_body, out_size, tag, tag_len, NULL, &rep->req);
    if (ret == FLB_RETRY && !stopping) {
        flb_plg_info(ctx->ins, "batch %s: %s, retrying now",
                     rep->batch_id, rep->req.error);
        rep->attempts++;
        ret = http_request(ctx, out_body, out_size, tag, tag_len, NULL,
                           &rep->req);
    }

    /*
     * A whole interval of records is at stake: a 413 means the request is
     * larger than the receiver accepts, which is fixed with a smaller
     * batch_max_size, so the batch is kept instead of dropped. Other 4xx
     * are kept on request (e.g: an expired credential).
     */
    if (ret == FLB_ERROR && rep->req.http_status == 413) {
        flb_plg_error(ctx->ins, "batch %s: request of %zu bytes rejected with "
                      "HTTP 413, set 'batch_max_size' below the receiver limit",
                      rep->batch_id, out_size);
        ret = FLB_RETRY;
    }
    else if (ret == FLB_ERROR && ctx->batch_retry_4xx &&
             rep->req.http_status >= 400 && rep->req.http_status < 500) {
        ret = FLB_RETRY;
    }

    if (out_body != data) {
        flb_sds_destroy(out_body);
    }

    return ret;
}

/* A chunk held by its suspended flush coroutine */
struct http_batch_entry {
    struct flb_coro *coro;
    const char *data;
    size_t size;
    const char *tag;
    int tag_len;
    int ret;
    struct mk_list _head;
};

/*
 * Hold the chunk: the flush coroutine is suspended, so the engine keeps the
 * chunk (and its file on filesystem storage) until the batch timer resumes
 * the coroutine with the delivery result.
 */
static int batch_hold(struct flb_out_http *ctx,
                      struct flb_event_chunk *event_chunk)
{
    struct http_batch_entry entry;

    /*
     * A single chunk is always accepted on an empty batch. Every held chunk
     * keeps a file descriptor, memory and an engine task (the task map is
     * shared by all the pipelines), so the number of held chunks is capped:
     * further chunks stay in storage and are retried by the engine.
     */
    if (ctx->batch_held_count > 0 &&
        ((ctx->batch_max_size > 0 &&
          ctx->batch_held_size + event_chunk->size > ctx->batch_max_size) ||
         (ctx->batch_hold_max_chunks > 0 &&
          ctx->batch_held_count >= ctx->batch_hold_max_chunks))) {
        if (!ctx->batch_full_warned) {
            flb_plg_warn(ctx->ins, "batch is full (%i chunks, %zu bytes; "
                         "batch_hold_max_chunks %i, batch_max_size %zu): "
                         "further chunks stay in storage and are retried by "
                         "the engine", ctx->batch_held_count,
                         ctx->batch_held_size, ctx->batch_hold_max_chunks,
                         ctx->batch_max_size);
            ctx->batch_full_warned = FLB_TRUE;
        }
        return FLB_RETRY;
    }

    entry.coro = flb_coro_get();
    entry.data = event_chunk->data;
    entry.size = event_chunk->size;
    entry.tag = event_chunk->tag;
    entry.tag_len = flb_sds_len(event_chunk->tag);
    entry.ret = FLB_RETRY;

    mk_list_add(&entry._head, &ctx->batch_held);
    ctx->batch_held_count++;
    ctx->batch_held_size += entry.size;

    flb_coro_yield(entry.coro, FLB_FALSE);

    return entry.ret;
}

/* Resume the first 'count' held chunks with the delivery result */
static void batch_release(struct flb_out_http *ctx, int count, int ret)
{
    struct flb_coro *self;
    struct http_batch_entry *entry;

    self = flb_coro_get();

    while (count-- > 0 && ctx->batch_held_count > 0) {
        entry = mk_list_entry_first(&ctx->batch_held,
                                    struct http_batch_entry, _head);
        mk_list_del(&entry->_head);
        ctx->batch_held_count--;
        ctx->batch_held_size -= entry->size;
        entry->ret = ret;

        /* 'entry' lives on the stack of the resumed coroutine */
        flb_coro_resume(entry->coro);

        /* flb_coro_resume() replaced the current coroutine, restore ours */
        flb_coro_set(self);
    }
}

/* Send every held chunk in a single request, then release them */
static int batch_send_held(struct flb_out_http *ctx, struct flb_config *config,
                           int stopping, int due)
{
    int ret;
    int count;
    flb_sds_t buf;
    flb_sds_t tmp;
    struct mk_list *head;
    struct http_batch_entry *entry;
    struct batch_report rep;

    count = ctx->batch_held_count;
    if (count == 0) {
        if (due && !stopping) {
            batch_report_init(ctx, &rep);
            flb_plg_debug(ctx->ins, "batch %s: nothing to send", rep.batch_id);
            batch_emit_status(ctx, config, "empty", &rep);
        }
        return FLB_OK;
    }

    batch_report_init(ctx, &rep);
    ret = FLB_RETRY;

    buf = flb_sds_create_size(ctx->batch_held_size);
    if (buf) {
        mk_list_foreach(head, &ctx->batch_held) {
            entry = mk_list_entry(head, struct http_batch_entry, _head);
            tmp = flb_sds_cat(buf, entry->data, entry->size);
            if (!tmp) {
                flb_sds_destroy(buf);
                buf = NULL;
                break;
            }
            buf = tmp;
        }
    }

    if (!buf) {
        flb_errno();
        snprintf(rep.req.error, sizeof(rep.req.error), "out of memory");
    }
    else {
        /* the first chunk stays held while the request is in flight */
        entry = mk_list_entry_first(&ctx->batch_held,
                                    struct http_batch_entry, _head);
        rep.records = batch_scan(buf, flb_sds_len(buf), &rep.first, &rep.last);
        ret = batch_request(ctx, buf, flb_sds_len(buf),
                            entry->tag, entry->tag_len, stopping, &rep, config);
        flb_sds_destroy(buf);
    }

    rep.duration_ms = (int64_t) (batch_clock_ms() - rep.started_ms);

    if (ret == FLB_RETRY && !stopping) {
        flb_plg_warn(ctx->ins, "batch %s failed: %s; %i records (%i chunks) "
                     "kept in storage for the next interval in %is",
                     rep.batch_id, rep.req.error, rep.records, count,
                     ctx->batch_interval);
        ctx->batch_carried = rep.records;
        batch_emit_status(ctx, config, "retry_next_interval", &rep);
        return ret;
    }

    ctx->batch_carried = 0;

    if (ret == FLB_OK) {
        flb_plg_info(ctx->ins, "batch %s sent: %i records (%i chunks), "
                     "%zu bytes, HTTP %i, %i attempt(s), %lld ms",
                     rep.batch_id, rep.records, count, rep.bytes,
                     rep.req.http_status, rep.attempts,
                     (long long) rep.duration_ms);
        batch_emit_status(ctx, config, "success", &rep);
    }
    else if (ret == FLB_RETRY) {
        flb_plg_warn(ctx->ins, "batch %s failed on shutdown: %s; %i records "
                     "(%i chunks) stay in storage and are sent again after "
                     "a restart", rep.batch_id, rep.req.error, rep.records,
                     count);
        batch_emit_status(ctx, config, "failed_on_shutdown", &rep);
    }
    else {
        flb_plg_error(ctx->ins, "batch %s dropped: %s; %i records (%i chunks) "
                      "are lost", rep.batch_id, rep.req.error, rep.records,
                      count);
        batch_emit_status(ctx, config, "dropped", &rep);
    }

    batch_release(ctx, count, ret);

    return ret;
}

/* Send all buffered records in a single request */
static int batch_send(struct flb_out_http *ctx, struct flb_config *config,
                      int stopping, int due)
{
    int ret;
    flb_sds_t buf;
    flb_sds_t tag;
    flb_sds_t tmp;
    struct batch_report rep;

    if (!ctx->batch_buf) {
        if (due && !stopping) {
            batch_report_init(ctx, &rep);
            flb_plg_debug(ctx->ins, "batch %s: nothing to send", rep.batch_id);
            batch_emit_status(ctx, config, "empty", &rep);
        }
        return FLB_OK;
    }

    /*
     * Detach the buffer: records flushed while the request is in flight
     * are appended to a new batch.
     */
    buf = ctx->batch_buf;
    tag = ctx->batch_tag;
    ctx->batch_buf = NULL;
    ctx->batch_tag = NULL;

    batch_report_init(ctx, &rep);
    rep.records = batch_scan(buf, flb_sds_len(buf), &rep.first, &rep.last);

    ret = batch_request(ctx, buf, flb_sds_len(buf),
                        tag, flb_sds_len(tag), stopping, &rep, config);
    rep.duration_ms = (int64_t) (batch_clock_ms() - rep.started_ms);

    if (ret == FLB_OK) {
        flb_plg_info(ctx->ins, "batch %s sent: %i records, %zu bytes, HTTP %i, "
                     "%i attempt(s), %lld ms", rep.batch_id, rep.records,
                     rep.bytes, rep.req.http_status, rep.attempts,
                     (long long) rep.duration_ms);
        ctx->batch_carried = 0;
        batch_emit_status(ctx, config, "success", &rep);
    }
    else if (ret == FLB_RETRY) {
        /* put the records back in front of the new batch */
        if (ctx->batch_buf) {
            tmp = flb_sds_cat(buf, ctx->batch_buf, flb_sds_len(ctx->batch_buf));
            if (!tmp) {
                flb_errno();
                flb_plg_error(ctx->ins, "batch %s dropped: out of memory "
                              "while keeping it; %i records are lost",
                              rep.batch_id, rep.records);
                ctx->batch_carried = 0;
                snprintf(rep.req.error, sizeof(rep.req.error),
                         "out of memory while keeping the batch");
                batch_emit_status(ctx, config, "dropped", &rep);
                flb_sds_destroy(buf);
                flb_sds_destroy(tag);
                return FLB_ERROR;
            }
            buf = tmp;
            flb_sds_destroy(ctx->batch_buf);
            flb_sds_destroy(ctx->batch_tag);
        }
        ctx->batch_buf = buf;
        ctx->batch_tag = tag;

        if (stopping) {
            flb_plg_warn(ctx->ins, "batch %s failed on shutdown: %s; %i "
                         "records kept in memory, sent again before exit",
                         rep.batch_id, rep.req.error, rep.records);
        }
        else {
            flb_plg_warn(ctx->ins, "batch %s failed: %s; %i records kept for "
                         "the next interval in %is", rep.batch_id,
                         rep.req.error, rep.records, ctx->batch_interval);
        }
        ctx->batch_carried = rep.records;
        batch_emit_status(ctx, config,
                          stopping ? "failed_on_shutdown" : "retry_next_interval",
                          &rep);
        return ret;
    }
    else {
        flb_plg_error(ctx->ins, "batch %s dropped: %s; %i records are lost",
                      rep.batch_id, rep.req.error, rep.records);
        ctx->batch_carried = 0;
        batch_emit_status(ctx, config, "dropped", &rep);
    }

    flb_sds_destroy(buf);
    flb_sds_destroy(tag);

    return ret;
}

static char **extract_headers(msgpack_object *obj) {
    size_t i;
    char **headers = NULL;
    size_t str_count;
    msgpack_object_map map;
    msgpack_object_str k;
    msgpack_object_str v;

    if (obj->type != MSGPACK_OBJECT_MAP) {
        goto err;
    }

    map = obj->via.map;
    str_count = map.size * 2 + 1;
    headers = flb_calloc(str_count, sizeof *headers);

    if (!headers) {
        goto err;
    }

    for (i = 0; i < map.size; i++) {
        if (map.ptr[i].key.type != MSGPACK_OBJECT_STR ||
            map.ptr[i].val.type != MSGPACK_OBJECT_STR) {
            continue;
        }

        k = map.ptr[i].key.via.str;
        v = map.ptr[i].val.via.str;

        headers[i * 2] = strndup(k.ptr, k.size);

        if (!headers[i]) {
            goto err;
        }

        headers[i * 2 + 1] = strndup(v.ptr, v.size);

        if (!headers[i]) {
            goto err;
        }
    }

    return headers;

err:
    if (headers) {
        for (i = 0; i < str_count; i++) {
            if (headers[i]) {
                flb_free(headers[i]);
            }
        }
        flb_free(headers);
    }
    return NULL;
}

static int send_all_requests(struct flb_out_http *ctx,
                             const char *data, size_t size,
                             flb_sds_t body_key,
                             flb_sds_t headers_key,
                             struct flb_event_chunk *event_chunk)
{
    msgpack_object map;
    msgpack_object *k;
    msgpack_object *v;
    msgpack_object *start_key;
    const char *body;
    size_t body_size;
    bool body_found;
    bool headers_found;
    char **headers;
    size_t record_count = 0;
    int ret = 0;
    struct flb_log_event_decoder log_decoder;
    struct flb_log_event log_event;

    ret = flb_log_event_decoder_init(&log_decoder, (char *) data, size);

    if (ret != FLB_EVENT_DECODER_SUCCESS) {
        flb_plg_error(ctx->ins,
                      "Log event decoder initialization error : %d", ret);

        return -1;
    }

    while ((flb_log_event_decoder_next(
                    &log_decoder,
                    &log_event)) == FLB_EVENT_DECODER_SUCCESS) {
        headers = NULL;
        body_found = false;
        headers_found = false;

        map = *log_event.body;

        if (map.type != MSGPACK_OBJECT_MAP) {
            ret = -1;
            break;
        }

        if (!flb_ra_get_kv_pair(ctx->body_ra, map, &start_key, &k, &v)) {
            if (v->type == MSGPACK_OBJECT_STR || v->type == MSGPACK_OBJECT_BIN) {
                body = v->via.str.ptr;
                body_size = v->via.str.size;
                body_found = true;
            }
            else {
                flb_plg_warn(ctx->ins,
                             "failed to extract body using pattern \"%s\" "
                             "(must be a msgpack string or bin)", ctx->body_key);
            }
        }

        if (!flb_ra_get_kv_pair(ctx->headers_ra, map, &start_key, &k, &v)) {
            headers = extract_headers(v);
            if (headers) {
                headers_found = true;
            }
            else {
                flb_plg_warn(ctx->ins,
                             "error extracting headers using pattern \"%s\"",
                             ctx->headers_key);
            }
        }

        if (body_found && headers_found) {
            flb_plg_trace(ctx->ins, "sending record %zu via %s",
                          record_count++,
                          ctx->http_method == FLB_HTTP_POST ? "POST" : "PUT");
            ret = http_request(ctx, body, body_size, event_chunk->tag,
                    flb_sds_len(event_chunk->tag), headers, NULL);
        }
        else {
            flb_plg_warn(ctx->ins,
                         "failed to extract body/headers using patterns "
                         "\"%s\" and \"%s\"", ctx->body_key, ctx->headers_key);
            ret = -1;
            continue;
        }

        flb_free(headers);
    }

    flb_log_event_decoder_destroy(&log_decoder);

    return ret;
}

static void cb_http_flush(struct flb_event_chunk *event_chunk,
                          struct flb_output_flush *out_flush,
                          struct flb_input_instance *i_ins,
                          void *out_context,
                          struct flb_config *config)
{
    int ret = FLB_ERROR;
    struct flb_out_http *ctx = out_context;
    void *out_body;
    size_t out_size;
    (void) i_ins;

    if (ctx->batch_interval > 0 && ctx->batch_hold_chunks) {
        /* suspended until the batch containing this chunk is resolved */
        ret = batch_hold(ctx, event_chunk);
        FLB_OUTPUT_RETURN(ret);
    }

    if (ctx->batch_interval > 0) {
        /* buffered records are sent by the batch timer */
        ret = batch_append(ctx, event_chunk);
        FLB_OUTPUT_RETURN(ret);
    }

    if (ctx->body_key) {
        ret = send_all_requests(ctx, event_chunk->data, event_chunk->size,
                                ctx->body_key, ctx->headers_key, event_chunk);
        if (ret < 0) {
            flb_plg_error(ctx->ins,
                          "failed to send requests using body key \"%s\"", ctx->body_key);
        }
    }
    else {
        ret = compose_payload(ctx, event_chunk->data, event_chunk->size,
                              &out_body, &out_size, config);
        if (ret != FLB_OK) {
            FLB_OUTPUT_RETURN(ret);
        }

        if ((ctx->out_format == FLB_PACK_JSON_FORMAT_JSON) ||
            (ctx->out_format == FLB_PACK_JSON_FORMAT_STREAM) ||
            (ctx->out_format == FLB_PACK_JSON_FORMAT_LINES) ||
            (ctx->out_format == FLB_HTTP_OUT_GELF)) {
            ret = http_request(ctx, out_body, out_size,
                               event_chunk->tag, flb_sds_len(event_chunk->tag), NULL, NULL);
            flb_sds_destroy(out_body);
        }
        else {
            /* msgpack */
            ret = http_request(ctx,
                               event_chunk->data, event_chunk->size,
                               event_chunk->tag, flb_sds_len(event_chunk->tag), NULL, NULL);
        }
    }

    FLB_OUTPUT_RETURN(ret);
}

/*
 * Runs every second: sends the batch once the interval is due, or right away
 * when the service is shutting down so held chunks are released before the
 * worker stops.
 */
static void cb_http_batch_timer(struct flb_config *config, void *data)
{
    int due;
    int stopping;
    uint64_t now;
    uint64_t interval;
    struct flb_out_http *ctx = data;

    now = batch_clock_ms();
    interval = (uint64_t) ctx->batch_interval * 1000;

    /*
     * is_shutting_down is set when the grace period starts, while a direct
     * flb_engine_shutdown() (e.g: engine thread cancelled) only clears
     * is_ingestion_active: handle both.
     */
    stopping = config->is_shutting_down ||
               config->is_ingestion_active == FLB_FALSE;

    if (ctx->batch_sending == FLB_FALSE &&
        (now >= ctx->batch_next || stopping)) {
        /* only a scheduled round reports an empty batch */
        due = (now >= ctx->batch_next);
        if (due) {
            ctx->batch_next += interval;
            if (ctx->batch_next <= now) {
                ctx->batch_next = now + interval;
            }
        }

        ctx->batch_sending = FLB_TRUE;
        if (ctx->batch_hold_chunks) {
            batch_send_held(ctx, config, stopping, due);
        }
        else {
            batch_send(ctx, config, stopping, due);
        }
        ctx->batch_sending = FLB_FALSE;
        ctx->batch_full_warned = FLB_FALSE;
    }

    flb_sched_timer_cb_coro_return();
}

static int cb_http_worker_init(void *data, struct flb_config *config)
{
    int ret;
    struct flb_sched *sched;
    struct flb_out_http *ctx = data;

    if (ctx->batch_interval <= 0 || ctx->batch_timer_created) {
        return 0;
    }

    ctx->batch_next = batch_clock_ms() + (uint64_t) ctx->batch_interval * 1000;

    sched = flb_sched_ctx_get();
    ret = flb_sched_timer_coro_cb_create(sched, FLB_SCHED_TIMER_CB_PERM, 1000,
                                         cb_http_batch_timer, ctx, NULL);
    if (ret == -1) {
        flb_plg_error(ctx->ins, "failed to create batch timer");
        return -1;
    }
    ctx->batch_timer_created = FLB_TRUE;

    if (ctx->batch_hold_chunks) {
        flb_plg_info(ctx->ins, "batch mode enabled, sending every %is, chunks "
                     "held until delivered (batch_max_size %zu, "
                     "batch_hold_max_chunks %i)", ctx->batch_interval,
                     ctx->batch_max_size, ctx->batch_hold_max_chunks);
    }
    else {
        flb_plg_info(ctx->ins, "batch mode enabled, sending every %is "
                     "(batch_max_size %zu)", ctx->batch_interval,
                     ctx->batch_max_size);
    }

    return 0;
}

static int cb_http_worker_exit(void *data, struct flb_config *config)
{
    struct flb_out_http *ctx = data;

    if (ctx->batch_interval <= 0 || !ctx->batch_buf) {
        return 0;
    }

    /* the event loop is gone, send the remaining records synchronously */
    flb_plg_info(ctx->ins, "sending remaining batch before exit");
    flb_stream_disable_async_mode(&ctx->u->base);
    batch_send(ctx, config, FLB_TRUE, FLB_FALSE);

    return 0;
}

static int cb_http_exit(void *data, struct flb_config *config)
{
    struct flb_out_http *ctx = data;

    flb_http_conf_destroy(ctx);
    return 0;
}

/* Configuration properties map */
static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "proxy", NULL,
     0, FLB_FALSE, 0,
     "Specify an HTTP Proxy. The expected format of this value is http://host:port. "
    },
    {
     FLB_CONFIG_MAP_BOOL, "allow_duplicated_headers", "true",
     0, FLB_TRUE, offsetof(struct flb_out_http, allow_dup_headers),
     "Specify if duplicated headers are allowed or not"
    },
    {
     FLB_CONFIG_MAP_BOOL, "log_response_payload", "true",
     0, FLB_TRUE, offsetof(struct flb_out_http, log_response_payload),
     "Specify if the response paylod should be logged or not"
    },
    {
     FLB_CONFIG_MAP_TIME, "http.response_timeout", "60s",
     0, FLB_TRUE, offsetof(struct flb_out_http, response_timeout),
     "Set maximum time to wait for a server response"
    },
    {
     FLB_CONFIG_MAP_TIME, "http.read_idle_timeout", "0s",
     0, FLB_TRUE, offsetof(struct flb_out_http, read_idle_timeout),
     "Set maximum allowed time between two consecutive reads"
    },
    {
     FLB_CONFIG_MAP_STR, "http_user", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, http_user),
     "Set HTTP auth user"
    },
    {
     FLB_CONFIG_MAP_STR, "http_passwd", "",
     0, FLB_TRUE, offsetof(struct flb_out_http, http_passwd),
     "Set HTTP auth password"
    },
    {
     FLB_CONFIG_MAP_BOOL, "oauth2.enable", "false",
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.enabled),
     "Enable OAuth2 client credentials for outgoing requests"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.token_url", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.token_url),
     "OAuth2 token endpoint URL"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.client_id", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.client_id),
     "OAuth2 client_id"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.client_secret", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.client_secret),
     "OAuth2 client_secret"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.user_agent", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.user_agent),
     "Optional User-Agent header for OAuth2 token requests"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.scope", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.scope),
     "Optional OAuth2 scope"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.audience", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.audience),
     "Optional OAuth2 audience parameter"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.resource", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.resource),
     "Optional OAuth2 resource parameter"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.auth_method", "basic",
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_auth_method),
     "OAuth2 client authentication method: basic, post or private_key_jwt"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.jwt_key_file", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http,
                           oauth2_config.jwt_key_file),
     "Path to PEM private key used by private_key_jwt"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.jwt_cert_file", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http,
                           oauth2_config.jwt_cert_file),
     "Path to certificate file used by private_key_jwt"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.jwt_aud", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http,
                           oauth2_config.jwt_aud),
     "Audience for private_key_jwt assertion (defaults to oauth2.token_url)"
    },
    {
     FLB_CONFIG_MAP_STR, "oauth2.jwt_header", "kid",
     0, FLB_TRUE, offsetof(struct flb_out_http,
                           oauth2_config.jwt_header),
     "JWT header claim name for private_key_jwt thumbprint (kid or x5t)"
    },
    {
     FLB_CONFIG_MAP_INT, "oauth2.jwt_ttl_seconds", "300",
     0, FLB_TRUE, offsetof(struct flb_out_http,
                           oauth2_config.jwt_ttl),
     "Lifetime in seconds for private_key_jwt client assertions"
    },
    {
     FLB_CONFIG_MAP_INT, "oauth2.refresh_skew_seconds", "60",
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.refresh_skew),
     "Seconds before expiry to refresh the access token"
    },
    {
     FLB_CONFIG_MAP_TIME, "oauth2.timeout", "0s",
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.timeout),
     "Timeout for OAuth2 token requests (defaults to response_timeout when unset)"
    },
    {
     FLB_CONFIG_MAP_TIME, "oauth2.connect_timeout", "0s",
     0, FLB_TRUE, offsetof(struct flb_out_http, oauth2_config.connect_timeout),
     "Connect timeout for OAuth2 token requests"
    },
#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
    {
     FLB_CONFIG_MAP_BOOL, "aws_auth", "false",
     0, FLB_TRUE, offsetof(struct flb_out_http, has_aws_auth),
     "Enable AWS SigV4 authentication"
    },
    {
     FLB_CONFIG_MAP_STR, "aws_service", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, aws_service),
     "AWS destination service code, used by SigV4 authentication"
    },
    FLB_AWS_CREDENTIAL_BASE_CONFIG_MAP(FLB_HTTP_AWS_CREDENTIAL_PREFIX),
#endif
#endif
    {
     FLB_CONFIG_MAP_STR, "header_tag", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, header_tag),
     "Set a HTTP header which value is the Tag"
    },
    {
     FLB_CONFIG_MAP_STR, "format", "json",
     0, FLB_TRUE, offsetof(struct flb_out_http, format),
     "Set desired payload format: json, json_stream, json_lines, gelf or msgpack"
    },
    {
     FLB_CONFIG_MAP_STR, "json_date_format", NULL,
     0, FLB_FALSE, 0,
     FBL_PACK_JSON_DATE_FORMAT_DESCRIPTION
    },
    {
     FLB_CONFIG_MAP_STR, "json_date_key", "date",
     0, FLB_TRUE, offsetof(struct flb_out_http, json_date_key),
     "Specify the name of the date field in output"
    },
    {
     FLB_CONFIG_MAP_STR, "json_events_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, json_events_key),
     "When format is 'json', wrap the records array into a JSON object under "
     "this key, e.g: {\"count\": 2, \"events\": [...]}"
    },
    {
     FLB_CONFIG_MAP_STR, "json_count_key", "count",
     0, FLB_TRUE, offsetof(struct flb_out_http, json_count_key),
     "Name of the record count field added when 'json_events_key' is set. "
     "Set to 'false' to omit the count field"
    },
    {
     FLB_CONFIG_MAP_TIME, "batch_interval", "0",
     0, FLB_TRUE, offsetof(struct flb_out_http, batch_interval),
     "Buffer records and send them in a single request once per interval, "
     "e.g: 5m. Requires a single worker. 0 disables batching"
    },
    {
     FLB_CONFIG_MAP_SIZE, "batch_max_size", "0",
     0, FLB_TRUE, offsetof(struct flb_out_http, batch_max_size),
     "Maximum size of the batch buffer, 0 means unlimited. When the buffer "
     "is full new chunks are retried later"
    },
    {
     FLB_CONFIG_MAP_BOOL, "batch_hold_chunks", "false",
     0, FLB_TRUE, offsetof(struct flb_out_http, batch_hold_chunks),
     "Keep chunks in the engine (and filesystem storage) until the batch "
     "containing them is delivered, instead of buffering a copy in memory"
    },
    {
     FLB_CONFIG_MAP_INT, "batch_hold_max_chunks", "512",
     0, FLB_TRUE, offsetof(struct flb_out_http, batch_hold_max_chunks),
     "Maximum number of chunks held by 'batch_hold_chunks'. Every held chunk "
     "keeps a file descriptor and an engine task: further chunks stay in "
     "storage and are retried by the engine. 0 means unlimited"
    },
    {
     FLB_CONFIG_MAP_BOOL, "batch_retry_4xx", "false",
     0, FLB_TRUE, offsetof(struct flb_out_http, batch_retry_4xx),
     "Keep a batch rejected with a 4xx status and retry it on the next "
     "interval instead of dropping it. A 413 is always kept"
    },
    {
     FLB_CONFIG_MAP_STR, "batch_status_tag", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, batch_status_tag),
     "Emit the result of every batch round as a record with this tag, so it "
     "can be routed to another output (e.g: pgsql)"
    },
    {
     FLB_CONFIG_MAP_STR, "batch_status_storage.type", "memory",
     0, FLB_TRUE, offsetof(struct flb_out_http, batch_status_storage_type),
     "Storage type of the batch status events: 'memory' or 'filesystem'"
    },
    {
     FLB_CONFIG_MAP_STR, "compress", NULL,
     0, FLB_FALSE, 0,
     "Set payload compression mechanism. Option available are 'gzip', 'snappy' and 'zstd'"
    },
    {
     FLB_CONFIG_MAP_SLIST_1, "header", NULL,
     FLB_CONFIG_MAP_MULT, FLB_TRUE, offsetof(struct flb_out_http, headers),
     "Add a HTTP header key/value pair. Multiple headers can be set"
    },
    {
     FLB_CONFIG_MAP_STR, "uri", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, uri),
     "Specify an optional HTTP URI for the target web server, e.g: /something"
    },
    {
     FLB_CONFIG_MAP_STR, "http_method", "POST",
     0, FLB_FALSE, 0,
     "Specify the HTTP method to use. Supported methods are POST and PUT"
    },

    /* Gelf Properties */
    {
     FLB_CONFIG_MAP_STR, "gelf_timestamp_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, gelf_fields.timestamp_key),
     "Specify the key to use for 'timestamp' in gelf format"
    },
    {
     FLB_CONFIG_MAP_STR, "gelf_host_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, gelf_fields.host_key),
     "Specify the key to use for the 'host' in gelf format"
    },
    {
     FLB_CONFIG_MAP_STR, "gelf_short_message_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, gelf_fields.short_message_key),
     "Specify the key to use as the 'short' message in gelf format"
    },
    {
     FLB_CONFIG_MAP_STR, "gelf_full_message_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, gelf_fields.full_message_key),
     "Specify the key to use for the 'full' message in gelf format"
    },
    {
     FLB_CONFIG_MAP_STR, "gelf_level_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, gelf_fields.level_key),
     "Specify the key to use for the 'level' in gelf format"
    },
    {
     FLB_CONFIG_MAP_STR, "body_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, body_key),
     "Specify the key which contains the body"
    },
    {
     FLB_CONFIG_MAP_STR, "headers_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_out_http, headers_key),
     "Specify the key which contains the headers"
    },

    /* EOF */
    {0}
};

static int cb_http_format_test(struct flb_config *config,
                               struct flb_input_instance *ins,
                               void *plugin_context,
                               void *flush_ctx,
                               int event_type,
                               const char *tag, int tag_len,
                               const void *data, size_t bytes,
                               void **out_data, size_t *out_size)
{
    struct flb_out_http *ctx = plugin_context;
    int ret;

    ret = compose_payload(ctx, data, bytes, out_data, out_size, config);
    if (ret != FLB_OK) {
        flb_error("ret=%d", ret);
        return -1;
    }
    return 0;
}

/* Plugin reference */
struct flb_output_plugin out_http_plugin = {
    .name        = "http",
    .description = "HTTP Output",
    .cb_init     = cb_http_init,
    .cb_pre_run  = NULL,
    .cb_flush    = cb_http_flush,
    .cb_exit     = cb_http_exit,
    .cb_worker_init = cb_http_worker_init,
    .cb_worker_exit = cb_http_worker_exit,
    .config_map  = config_map,

    /* for testing */
    .test_formatter.callback = cb_http_format_test,

    .flags       = FLB_OUTPUT_NET | FLB_IO_OPT_TLS,
    .workers     = 2
};
