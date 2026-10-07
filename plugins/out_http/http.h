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

#ifndef FLB_OUT_HTTP_H
#define FLB_OUT_HTTP_H

#include <fluent-bit/flb_oauth2.h>
#include <fluent-bit/flb_sds.h>
#include <monkey/mk_core/mk_list.h>
#include <time.h>

#define FLB_HTTP_OUT_MSGPACK        FLB_PACK_JSON_FORMAT_NONE
#define FLB_HTTP_OUT_GELF           20

#define FLB_HTTP_CONTENT_TYPE   "Content-Type"
#define FLB_HTTP_MIME_MSGPACK   "application/msgpack"
#define FLB_HTTP_MIME_JSON      "application/json"
#define FLB_HTTP_MIME_NDJSON    "application/x-ndjson"

#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
#define FLB_HTTP_AWS_CREDENTIAL_PREFIX "aws_"
#endif
#endif

struct flb_out_http {
    /* HTTP Auth */
    char *http_user;
    char *http_passwd;

    /* AWS Auth */
#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
    int has_aws_auth;
    struct flb_aws_provider *aws_provider;
    const char *aws_region;
    const char *aws_service;
#endif
#endif

    /* Proxy */
    const char *proxy;
    char *proxy_host;
    int proxy_port;

    /* Output format */
    int out_format;
    flb_sds_t format;

    int json_date_format;
    flb_sds_t json_date_key;
    flb_sds_t date_key;        /* internal use */

    /* JSON envelope: {"<count_key>": N, "<events_key>": [...]} */
    flb_sds_t json_events_key;
    flb_sds_t json_count_key;
    flb_sds_t count_key;       /* internal use */

    /*
     * Batch mode: records are buffered in memory and sent in a single
     * request once per 'batch_interval'. It relies on a single worker so
     * the flush callback and the batch timer run in the same thread.
     */
    int batch_interval;        /* seconds, 0 = disabled */
    size_t batch_max_size;     /* max buffered bytes, 0 = unlimited */
    flb_sds_t batch_buf;       /* buffered msgpack records */
    flb_sds_t batch_tag;       /* tag of the first buffered chunk */
    int batch_timer_created;
    int batch_sending;         /* a batch request is in flight */
    uint64_t batch_next;       /* next scheduled send, monotonic ms */

    /*
     * Hold mode: instead of copying records, every flush coroutine is
     * suspended and its chunk stays in the engine (and in filesystem
     * storage) until the batch containing it is delivered.
     */
    int batch_hold_chunks;
    int batch_hold_max_chunks;  /* cap of held chunks, 0 = unlimited */
    int batch_retry_4xx;        /* keep batches rejected with a 4xx */
    int batch_full_warned;      /* 'batch is full' reported this round */
    int batch_held_count;      /* number of held chunks */
    size_t batch_held_size;    /* bytes of held chunks */
    struct mk_list batch_held; /* list of held chunks */

    /* Batch ids used in the logs */
    uint64_t batch_seq;                      /* batch round sequence */
    char batch_token[9];                     /* per process part of batch ids */

    /* HTTP URI */
    char *uri;
    char *host;
    int port;

    /* HTTP method */
    int http_method;

    /* GELF fields */
    struct flb_gelf_fields gelf_fields;

    /* which record key to use as body */
    flb_sds_t body_key;

    struct flb_record_accessor *body_ra;

    /* override headers with contents of the map in the key specified here */
    flb_sds_t headers_key;

    struct flb_record_accessor *headers_ra;

    /* Include tag in header */
    flb_sds_t header_tag;

    /* Compression mode (gzip) */
    int compress_gzip;
    int compress_snappy;
    int compress_zstd;

    /* Allow duplicated headers */
    int allow_dup_headers;

    /* Log the response paylod */
    int log_response_payload;

    /* Response timeout */
    int response_timeout;

    /* Read idle timeout */
    int read_idle_timeout;

    /* Upstream connection to the backend server */
    struct flb_upstream *u;

    /* Arbitrary HTTP headers */
    struct mk_list *headers;

    /* Plugin instance */
    struct flb_output_instance *ins;

    /* OAuth2 */
    struct flb_oauth2_config oauth2_config;
    struct flb_oauth2 *oauth2_ctx;
    flb_sds_t oauth2_auth_method;
};

#endif
