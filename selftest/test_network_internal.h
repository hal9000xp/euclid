/********************************************************************
 * Copyright (c) 2014, Eldar Gaynetdinov <hal9000ed2k@gmail.com>    *
 *                                                                  *
 * Permission to use, copy, modify, and/or distribute this software *
 * for any purpose with or without fee is hereby granted, provided  *
 * that the above copyright notice and this permission notice       *
 * appear in all copies.                                            *
 *                                                                  *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL    *
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED    *
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL     *
 * THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT,          *
 * OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING     *
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF       *
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF    *
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.   *
 ********************************************************************/

#define TOTAL_INCOMING_MSGS 5

typedef enum {
    CONN_LISTEN = 1,
    CONN_ACCEPTED,
    CONN_CONNECT
} conn_type_t;

typedef struct {
    unsigned int            crc32;
    int                     len;
} crc_check_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t                id;
    ptr_id_t                prev;
    ptr_id_t                next;

    http_id_t               http_id;

    /* NOTE: pointer to static, don't free */
    crc_check_t            *check;
} requests_queue_elt_t;

typedef struct {
    ptr_id_t                msg_id;
    unsigned                n;
    bool                    connection_close;
} incoming_msg_t;

typedef struct {
    http_id_t               http_id;
    ptr_id_t                udata_id;

    conn_type_t             type;
    bool                    keep_alive;
    bool                    is_ssl;

    ll_t                    requests_queue;
    int                     checked_responses;

    incoming_msg_t          incoming_msgs[TOTAL_INCOMING_MSGS];
    unsigned                incoming_req_num;

    bool                    to_itself;
    unsigned                outcoming_expected_num;
} conn_elt_t;

typedef struct {
    http_id_t               http_id;
    http_tmr_id_t           tmr_id;
} tmr_elt_t;

typedef enum {
    NET_MAKE_CONN = 0,
    NET_MAKE_LISTEN,
    NET_POST_DATA,
    NET_MAKE_CONN_TMR,
    NET_MAKE_GLOBAL_TMR,
    NET_DEL_CONN_TMR,
    NET_DEL_GLOBAL_TMR,
    NET_CLOSE_CONN,
    NET_TOTAL_API
} net_api_index_t;

#define UDATA_LABEL                 0xDEADBEEF

