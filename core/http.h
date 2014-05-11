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

#include "hash_table.h"

#define IS_PRINTABLE_ASCII( c )         ( c >= 32 && c <= 126 )
#define IS_DIGIT( c )                   ( c >= '0' && c <= '9' )

#define IS_HEX( c )                     ( IS_DIGIT( c )             || \
                                          ( c >= 'a' && c <= 'f' )  || \
                                          ( c >= 'A' && c <= 'F' ) )

#define MAX_URL_LEN                     16384

#define MAX_USER_AGENT_LEN              256

/* TODO: move to config */
#define HTTP_USER_AGENT         "Mozilla/5.0 (X11; Ubuntu; Linux i686; rv:20.0)"
#define HTTP_USER_AGENT_LEN     (sizeof(HTTP_USER_AGENT) - 1)

#define DEFAULT_HOST                    "localhost"
#define DEFAULT_HOST_LEN                (sizeof(DEFAULT_HOST) - 1)

#define HTTP_HDR_MAX_LEN                65536

#define HTTP_STATUS_CODE_MIN            100
#define HTTP_STATUS_CODE_MAX            599

#define HTTP_ACCEPT_ENCODING_GZIP       "gzip"
#define HTTP_ACCEPT_ENCODING_GZIP_LEN   (sizeof(HTTP_ACCEPT_ENCODING_GZIP) - 1)

#define HTTP_CONTENT_TYPE               "Content-Type"
#define HTTP_CONTENT_TYPE_LEN           (sizeof(HTTP_CONTENT_TYPE) - 1)

#define HTTP_X_WWW_FORM_URLENCODED      "application/x-www-form-urlencoded"
#define HTTP_X_WWW_FORM_URLENCODED_LEN  (sizeof(HTTP_X_WWW_FORM_URLENCODED) - 1)

typedef struct {
    /* ll_node_t */
    ptr_id_t            id;
    ptr_id_t            prev;
    ptr_id_t            next;

    char               *value;
    unsigned            value_ndx;
    int                 value_len;
} http_raw_hdr_value_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t            id;
    ptr_id_t            prev;
    ptr_id_t            next;

    char               *key;
    unsigned            key_ndx;
    int                 key_len;

    ll_t                raw_value;
} http_raw_hdr_field_t;

typedef struct {
    char               *key;
    char               *val;
} http_www_form_t;

typedef struct {
    char               *url;
    int                 url_ndx;
    int                 url_len;

    char               *raw_body;
    int                 raw_body_ndx;
    int                 raw_body_len;

    char               *body;
    int                 body_len;

    http_www_form_t    *www_form;
    int                 www_form_size;

    int                 status_code;

    bool                is_options_method;

    /* NOTE: it's only supported in the case *
     *       we receive it as a server       */
    bool                is_connect_method;

    bool                connection_close;

    char               *host;
    int                 host_ndx;
    int                 host_len;

    char               *user_agent;
    int                 user_agent_ndx;
    int                 user_agent_len;

    char               *location;
    int                 location_ndx;
    int                 location_len;

    char               *accept_encoding;
    int                 accept_encoding_ndx;
    int                 accept_encoding_len;

    char               *content_encoding;
    int                 content_encoding_ndx;
    int                 content_encoding_len;

    char               *transfer_encoding;
    int                 transfer_encoding_ndx;
    int                 transfer_encoding_len;

    bool                transfer_encoding_chunked;

    ll_t                raw_fields;

    bool                is_duped;
} http_msg_t;

typedef enum {
    HTTP_ERRNO_OK = 0,
    HTTP_ERRNO_WRONG_PARAMS,
    HTTP_ERRNO_WRONG_CONN,
    HTTP_ERRNO_WRONG_STATE,
    HTTP_ERRNO_HDR_TOO_LARGE,
    HTTP_ERRNO_GENERAL_ERR
} http_errno_t;

typedef enum {
    HTTP_STATE_ERROR = -1,
    HTTP_STATE_NOT_EST,
    HTTP_STATE_EST,
    HTTP_STATE_SENT_CLOSE,
    HTTP_STATE_TUNNELING
} http_state_t;

typedef enum {
    HTTP_POST_STATE_DEFAULT = 0,
    HTTP_POST_STATE_SENT_CLOSE,
    HTTP_POST_STATE_TUNNELING
} http_post_state_t;

typedef ptr_id_t    http_id_t;
typedef ptr_id_t    http_tmr_id_t;

/* uh == user handler */
typedef void ( *http_client_r_uh_t )( http_id_t         http_id,
                                      ptr_id_t          udata_id,
                                      http_msg_t       *http_msg );

typedef void ( *http_server_r_uh_t )( http_id_t         http_id,
                                      ptr_id_t          udata_id,
                                      ptr_id_t          msg_id,
                                      http_msg_t       *http_msg );

typedef void ( *http_est_uh_t )( http_id_t              http_id,
                                 ptr_id_t               udata_id );

typedef void ( *http_clo_uh_t )( http_id_t              http_id,
                                 ptr_id_t               udata_id,
                                 int                    code );

typedef void ( *http_tmr_cb_t )( http_id_t              http_id,
                                 ptr_id_t               http_udata_id,
                                 http_tmr_id_t          tmr_id,
                                 ptr_id_t               tmr_udata_id );

typedef ptr_id_t ( *http_dup_udata_t )( http_id_t       http_id,
                                        ptr_id_t        udata_id );

unsigned        G_http_errno;

void            http_init();

http_id_t       http_make_conn( net_host_t             *host,
                                http_client_r_uh_t      r_uh_cb,
                                http_est_uh_t           est_uh_cb,
                                http_clo_uh_t           clo_uh_cb,
                                ptr_id_t                udata_id );

http_id_t       http_make_listen( http_server_r_uh_t    child_r_cb,
                                  http_est_uh_t         child_est_cb,
                                  http_clo_uh_t         child_clo_cb,
                                  http_dup_udata_t      dup_udata_cb,
                                  http_clo_uh_t         clo_uh_cb,
                                  ptr_id_t              udata_id,
                                  int                   port,
                                  bool                  use_ssl );

int             http_post_data( http_id_t               http_id,
                                http_msg_t             *msg,
                                ptr_id_t                msg_qid,
                                http_post_state_t      *state );

int             http_post_raw_data( http_id_t           http_id,
                                    http_msg_t         *msg );

http_tmr_id_t   http_make_tmr( http_id_t                http_id,
                               ptr_id_t                 udata_id,
                               http_tmr_cb_t            cb,
                               struct timeval          *timeout );

int             http_del_tmr( http_id_t                 http_id,
                              http_tmr_id_t             tmr_id );

int             http_shutdown( http_id_t                http_id,
                               bool                     flush_and_close );

http_state_t    http_is_est( http_id_t                  http_id );

http_msg_t     *http_dup_msg( http_msg_t               *msg,
                              bool                      no_proxy_url );

void            http_free_msg( http_msg_t              *msg );

net_host_t     *http_msg_get_host( http_msg_t          *msg,
                                   bool                 is_ssl );

int             http_percent_encode( char              *out,
                                     int                out_len,
                                     char              *in,
                                     int                in_len,
                                     bool               is_www_form );

int             http_percent_decode( char              *out,
                                     int                out_len,
                                     char              *in,
                                     int                in_len,
                                     bool               is_www_form );

hash_table_t   *http_parse_www_form( char              *form,
                                     int                form_len,
                                     bool               is_url,
                                     int                n_hash_elts );

extern struct timeval  *cfg_http_response_timeout;
extern struct timeval  *cfg_http_check_messages_queue_interval;

