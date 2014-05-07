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

#define HTTP_DEFAULT_PORT               80
#define HTTP_DEFAULT_SSL_PORT           443

#define HTTP_HDR_MAX_LINE_LEN           16384

#define HTTP_HDR_MAX_LINES              128

#define HTTP_VER                        "HTTP/1.1"
#define HTTP_VER_LEN                    8

#define HTTP_VER10                      "HTTP/1.0"
#define HTTP_VER10_LEN                  8

#define HTTP_STATUS_MIN_LEN             12
#define HTTP_STATUS_1SP_POS             8
#define HTTP_STATUS_CODE_1POS           9
#define HTTP_STATUS_CODE_2POS           10
#define HTTP_STATUS_CODE_3POS           11
#define HTTP_STATUS_2SP_POS             12

#define HTTP_GET_METHOD                 "GET "
#define HTTP_GET_METHOD_LEN             (sizeof(HTTP_GET_METHOD) - 1)

#define HTTP_POST_METHOD                "POST "
#define HTTP_POST_METHOD_LEN            (sizeof(HTTP_POST_METHOD) - 1)

#define HTTP_OPTIONS_METHOD             "OPTIONS "
#define HTTP_OPTIONS_METHOD_LEN         (sizeof(HTTP_OPTIONS_METHOD) - 1)

#define HTTP_CONNECT_METHOD             "CONNECT "
#define HTTP_CONNECT_METHOD_LEN         (sizeof(HTTP_CONNECT_METHOD) - 1)

#define HTTP_URL_PROTOCOL_NO_SSL        "http://"
#define HTTP_URL_PROTOCOL_NO_SSL_LEN    (sizeof(HTTP_URL_PROTOCOL_NO_SSL) - 1)

#define HTTP_URL_PROTOCOL_SSL           "https://"
#define HTTP_URL_PROTOCOL_SSL_LEN       (sizeof(HTTP_URL_PROTOCOL_SSL) - 1)

#define HTTP_HDR_CONTENT_LENGTH         "Content-Length: "
#define HTTP_HDR_CONTENT_LENGTH_LEN     (sizeof(HTTP_HDR_CONTENT_LENGTH) - 1)

#define HTTP_HDR_CONNECTION             "Connection: "
#define HTTP_HDR_CONNECTION_LEN         (sizeof(HTTP_HDR_CONNECTION) - 1)

#define HTTP_HDR_CONNECTION_CLOSE       "close"
#define HTTP_HDR_CONNECTION_CLOSE_LEN   (sizeof(HTTP_HDR_CONNECTION_CLOSE) - 1)
#define HTTP_HDR_CONNECTION_KEEP_ALIVE  "keep-alive"

#define HTTP_HDR_HOST                   "Host: "
#define HTTP_HDR_HOST_LEN               (sizeof(HTTP_HDR_HOST) - 1)

#define HTTP_HDR_USER_AGENT             "User-Agent: "
#define HTTP_HDR_USER_AGENT_LEN         (sizeof(HTTP_HDR_USER_AGENT) - 1)

#define HTTP_HDR_LOCATION               "Location: "
#define HTTP_HDR_LOCATION_LEN           (sizeof(HTTP_HDR_LOCATION) - 1)

#define HTTP_HDR_ACCEPT_ENCODING        "Accept-Encoding: "
#define HTTP_HDR_ACCEPT_ENCODING_LEN    (sizeof(HTTP_HDR_ACCEPT_ENCODING) - 1)

#define HTTP_HDR_CONTENT_ENCODING       "Content-Encoding: "
#define HTTP_HDR_CONTENT_ENCODING_LEN   (sizeof(HTTP_HDR_CONTENT_ENCODING) - 1)

#define HTTP_HDR_TRANSFER_ENCODING      "Transfer-Encoding: "
#define HTTP_HDR_TRANSFER_ENCODING_LEN  (sizeof(HTTP_HDR_TRANSFER_ENCODING) - 1)

#define HTTP_HDR_CHUNKED_ENCODING       "chunked"

#define HTTP_HDR_GZIP_ENCODING          "gzip"
#define HTTP_HDR_GZIP_ENCODING_LEN      (sizeof(HTTP_HDR_GZIP_ENCODING) - 1)

#define HTTP_PARSE_ERROR                (-1)
#define HTTP_PARSE_CONN_CLOSE           (-2)

#define HTTP_ZLIB_WINDOW_BITS           15
#define HTTP_ZLIB_GZIP_ENCODING         16
#define HTTP_ZLIB_COEFFICIENT           10

#define HTTP_HASH_TABLE_SIZE            16384

#define HTTP_WWW_FORM_KEY_LEN           65536
#define HTTP_WWW_FORM_VAL_LEN           65536

/* NOTE: smallest form requires 2 bytes: k= */
#define HTTP_WWW_FORM_MIN_LEN           2

/* S_ stands for state */
enum {
    S_HTTP_STATUS_LINE = 1,
    S_HTTP_REQUEST_LINE,
    S_HTTP_HEADER_FIELDS,
    S_HTTP_BODY,
    S_HTTP_EOM
};

enum {
    S_CHUNK_SIZE = 0,
    S_CHUNK_DATA,
    S_CHUNK_LAST
};

enum {
    HTTP_CH = 1,
    HTTP_CR,
    HTTP_LF
};

typedef struct {
    int                     last_ch_type;
    int                     last_line_begin;
    int                     first_colon_begin;
    bool                    is_raw_field;
    int                     line_num;
    bool                    is_http_ver10;
    bool                    no_content_length;
    bool                    body_until_closed;
} http_hdr_state_t;

typedef struct {
    unsigned                st;
    int                     last_ch_type;
    int                     size_begin;
    int                     size_len;
    int                     size_val;
    int                     data_begin;
    bool                    has_data;
    int                     trailer_last_line_begin;
    int                     trailer_last_line_len;
} http_chunked_state_t;

typedef struct {
    unsigned                state;
    http_hdr_state_t        hdr_state;
    http_chunked_state_t    chunked_body_state;
    http_msg_t              http_msg;
    int                     n;
} http_read_state_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t                id;
    ptr_id_t                prev;
    ptr_id_t                next;

    http_id_t               http_id;

    struct timeval          sent;

    char                   *hdr;
    unsigned                hdr_len;
    char                   *body;
    unsigned                body_len;

    bool                    connection_close;
    bool                    connect_method;
} http_messages_queue_elt_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t                id;
    ptr_id_t                prev;
    ptr_id_t                next;

    tmr_id_t                net_tmr_id;

    http_id_t               http_id;

    ptr_id_t                udata_id;
    http_tmr_cb_t           cb;
} http_tmr_node_t;

typedef struct {
    conn_id_t               conn_id;

    char                    host[MAX_DOMAIN_LEN];
    char                    port[MAX_PORT_STR_LEN];

    int                     listen_port;

    http_id_t               http_id;
    ptr_id_t                udata_id;

    http_client_r_uh_t      client_r_cb;
    http_server_r_uh_t      server_r_cb;

    http_est_uh_t           est_cb;
    http_clo_uh_t           clo_cb;

    http_server_r_uh_t      child_r_cb;
    http_est_uh_t           child_est_cb;
    http_clo_uh_t           child_clo_cb;

    http_dup_udata_t        dup_udata_cb;

    ll_t                    tmr_list;

    ll_t                    messages_queue;

    int                     messages_handled;

    http_read_state_t       read_state;

    bool                    is_in_dup_udata;
    bool                    sent_close;
    bool                    got_connect_method;
    bool                    tunneling_mode;
} http_conn_t;

