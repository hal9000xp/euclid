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

#include "main.h"
#include "linked_list.h"
#include "logger.h"
#include "network.h"
#include "http.h"
#include "http_internal.h"
#include <zlib.h>

struct timeval     *cfg_http_response_timeout = NULL;
struct timeval     *cfg_http_check_messages_queue_interval = NULL;

static bool         g_percent_encoding_map[256];

/******************* Misc util functions **************************************/

static void __free_queue_elt( ll_t                         *queue,
                              http_messages_queue_elt_t    *q_elt )
{
    LL_DEL_NODE( queue, q_elt->id );

    if( q_elt->hdr )
        free( q_elt->hdr );

    if( q_elt->body )
        free( q_elt->body );

    memset( q_elt, 0, sizeof(http_messages_queue_elt_t) );
    free( q_elt );
}

static void __free_http( http_conn_t *http )
{
    http_tmr_node_t                *tmr;
    http_tmr_node_t                *tmr_next;
    http_messages_queue_elt_t      *q_elt;
    http_messages_queue_elt_t      *q_elt_next;

    if( http->tmr_list.total )
    {
        LL_CHECK( &http->tmr_list,
                  http->tmr_list.head );

        tmr = PTRID_GET_PTR( http->tmr_list.head );

        while( tmr )
        {
            tmr_next = PTRID_GET_PTR( tmr->next );

            LL_DEL_NODE( &http->tmr_list, tmr->id );

            c_assert( tmr->http_id == http->http_id );

            memset( tmr, 0, sizeof(http_tmr_node_t) );
            free( tmr );

            tmr = tmr_next;
        }
    }

    if( http->messages_queue.total )
    {
        LL_CHECK( &http->messages_queue,
                  http->messages_queue.head );

        q_elt = PTRID_GET_PTR( http->messages_queue.head );

        while( q_elt )
        {
            q_elt_next = PTRID_GET_PTR( q_elt->next );

            c_assert( q_elt->http_id == http->http_id &&
                      !(!q_elt->hdr_len && q_elt->body_len) );

            __free_queue_elt( &http->messages_queue, q_elt );

            q_elt = q_elt_next;
        }
    }

    c_assert( !http->tmr_list.total &&
              !http->tmr_list.head  &&
              !http->tmr_list.tail  &&
              !http->messages_queue.total &&
              !http->messages_queue.head  &&
              !http->messages_queue.tail );

    if( http->read_state.http_msg.body )
        free( http->read_state.http_msg.body );

    memset( http, 0, sizeof(http_conn_t) );
    free( http );
}

static void http_state_init( http_read_state_t     *read_state,
                             bool                   client )
{
    if( read_state->http_msg.body )
        free( read_state->http_msg.body );

    memset( read_state, 0, sizeof(http_read_state_t) );

    if( client )
    {
        read_state->state = S_HTTP_STATUS_LINE;
        read_state->hdr_state.no_content_length = true;
    }
    else
        read_state->state = S_HTTP_REQUEST_LINE;

    read_state->hdr_state.last_ch_type = HTTP_LF;
}

static ptr_id_t __messages_queue_push( http_conn_t *http,
                                       bool         connection_close,
                                       bool         connect_method )
{
    http_messages_queue_elt_t      *q_elt;

    if( http->sent_close )
        return 0;

    q_elt = malloc( sizeof(http_messages_queue_elt_t) );
    memset( q_elt, 0, sizeof(http_messages_queue_elt_t) );

    q_elt->http_id = http->http_id;

    q_elt->sent = G_now;

    q_elt->connection_close = connection_close;

    q_elt->connect_method = connect_method;

    LL_ADD_NODE( &http->messages_queue, q_elt );

    LOG( "http_id:0x%llx messages_queue_total:%d",
         PTRID_FMT( http->http_id ),
         http->messages_queue.total );

    return http->messages_queue.tail;
}

static int __messages_queue_pop( http_conn_t *http )
{
    http_messages_queue_elt_t      *q_elt;

    if( !http->messages_queue.total )
        return -1;

    q_elt = PTRID_GET_PTR( http->messages_queue.head );

    c_assert( q_elt->http_id == http->http_id );

    __free_queue_elt( &http->messages_queue, q_elt );
    return 0;
}

static void __check_messages_queue_tmr_cb( conn_id_t    conn_id,
                                           ptr_id_t     conn_udata_id,
                                           tmr_id_t     tmr_id,
                                           ptr_id_t     tmr_udata_id )
{
    http_conn_t                    *http;
    http_messages_queue_elt_t      *q_elt;
    struct timeval                  diff;
    int                             r;

    c_assert( conn_id && conn_udata_id &&
              tmr_id && !tmr_udata_id );

    LOGD( "conn_id:0x%llx conn_udata_id:0x%llx tmr_id:0x%llx",
          PTRID_FMT( conn_id ), PTRID_FMT( conn_udata_id ),
          PTRID_FMT( tmr_id ) );

    http = PTRID_GET_PTR( conn_udata_id );

    c_assert( http->http_id == conn_udata_id &&
              http->conn_id == conn_id );

    if( http->messages_queue.total )
    {
        LL_CHECK( &http->messages_queue,
                  http->messages_queue.head );

        q_elt = PTRID_GET_PTR( http->messages_queue.head );

        c_assert( q_elt->http_id == http->http_id );

        timersub( &G_now, &q_elt->sent, &diff );

        if( timercmp( &diff, cfg_http_response_timeout, > ) )
        {
            LOGE( "conn_id:0x%llx conn_udata_id:0x%llx "
                  "tmr_id:0x%llx tmr_udata_id:0x%llx",
                  PTRID_FMT( conn_id ), PTRID_FMT( conn_udata_id ),
                  PTRID_FMT( tmr_id ), PTRID_FMT( tmr_udata_id ) );

            r = net_shutdown_conn( conn_id, false );
            c_assert( !r );
        }
    }
}

static void __free_raw_hdr_fields( http_msg_t *msg )
{
    if( !msg->raw_fields.total )
        return;

    http_raw_hdr_field_t   *raw_field;
    http_raw_hdr_field_t   *raw_field_next;
    http_raw_hdr_value_t   *raw_value;
    http_raw_hdr_value_t   *raw_value_next;

    LL_CHECK( &msg->raw_fields, msg->raw_fields.head );

    raw_field = PTRID_GET_PTR( msg->raw_fields.head );

    while( raw_field )
    {
        raw_field_next = PTRID_GET_PTR( raw_field->next );

        c_assert( !raw_field_next ||
                  raw_field_next->id == raw_field->next );

        /* raw_value */
        if( raw_field->raw_value.total )
        {
            LL_CHECK( &raw_field->raw_value,
                      raw_field->raw_value.head );

            raw_value = PTRID_GET_PTR( raw_field->raw_value.head );

            while( raw_value )
            {
                raw_value_next = PTRID_GET_PTR( raw_value->next );

                c_assert( !raw_value_next ||
                          raw_value_next->id == raw_value->next );

                LL_DEL_NODE( &raw_field->raw_value,
                             raw_value->id );

                if( raw_value->value )
                    free( raw_value->value );

                memset( raw_value, 0,
                        sizeof(http_raw_hdr_value_t) );

                free( raw_value );

                raw_value = raw_value_next;
            }

            c_assert( !raw_field->raw_value.total );
        }
        /* /raw_value */

        LL_DEL_NODE( &msg->raw_fields, raw_field->id );

        if( raw_field->key )
            free( raw_field->key );

        memset( raw_field, 0, sizeof(http_raw_hdr_field_t) );
        free( raw_field );

        raw_field = raw_field_next;
    }

    c_assert( !msg->raw_fields.total );
}

static void __dup_raw_hdr_fields( http_msg_t   *duped_msg,
                                  http_msg_t   *msg )
{
    if( !msg->raw_fields.total )
        return;

    http_raw_hdr_field_t   *raw_field;
    http_raw_hdr_field_t   *raw_field_next;
    http_raw_hdr_field_t   *duped_raw_field;
    http_raw_hdr_value_t   *raw_value;
    http_raw_hdr_value_t   *raw_value_next;
    http_raw_hdr_value_t   *duped_raw_value;

    LL_CHECK( &msg->raw_fields, msg->raw_fields.head );

    raw_field = PTRID_GET_PTR( msg->raw_fields.head );

    while( raw_field )
    {
        raw_field_next = PTRID_GET_PTR( raw_field->next );

        c_assert( !raw_field_next ||
                  raw_field_next->id == raw_field->next );

        if( !raw_field->key || !raw_field->key_len )
        {
            return;
        }

        duped_raw_field = malloc( sizeof(http_raw_hdr_field_t) );

        memset( duped_raw_field, 0,
                sizeof(http_raw_hdr_field_t) );

        duped_raw_field->key = malloc( raw_field->key_len );

        memcpy( duped_raw_field->key,
                raw_field->key, raw_field->key_len );

        duped_raw_field->key_len = raw_field->key_len;

        LL_ADD_NODE( &duped_msg->raw_fields,
                     duped_raw_field );

        if( !raw_field->raw_value.total )
        {
            raw_field = raw_field_next;
            continue;
        }

        /* raw_value */
        LL_CHECK( &raw_field->raw_value,
                  raw_field->raw_value.head );

        raw_value = PTRID_GET_PTR( raw_field->raw_value.head );

        while( raw_value )
        {
            raw_value_next = PTRID_GET_PTR( raw_value->next );

            c_assert( !raw_value_next ||
                      raw_value_next->id == raw_value->next );

            if( !raw_value->value || !raw_value->value_len )
            {
                return;
            }

            duped_raw_value = malloc( sizeof(http_raw_hdr_value_t) );

            memset( duped_raw_value, 0,
                    sizeof(http_raw_hdr_value_t) );

            duped_raw_value->value = malloc( raw_value->value_len );

            memcpy( duped_raw_value->value,
                    raw_value->value,
                    raw_value->value_len );

            duped_raw_value->value_len = raw_value->value_len;

            LL_ADD_NODE( &duped_raw_field->raw_value,
                         duped_raw_value );

            raw_value = raw_value_next;
        }
        /* /raw_value */

        raw_field = raw_field_next;
    }
}

static int __set_www_form_pair( hash_table_t   *hash_table,
                                char           *key,
                                int             key_len,
                                char           *val,
                                int             val_len )
{
    char    decoded_key[HTTP_WWW_FORM_KEY_LEN];
    char    decoded_val[HTTP_WWW_FORM_VAL_LEN];
    int     decoded_key_len;
    int     decoded_val_len;

    decoded_key_len = http_percent_decode( decoded_key,
                                           sizeof(decoded_key),
                                           key, key_len, true );

    if( decoded_key_len == -1 )
        return -1;

    if( val )
    {
        decoded_val_len = http_percent_decode( decoded_val,
                                               sizeof(decoded_val),
                                               val, val_len, true );

        if( decoded_val_len == -1 )
            return -1;
    }
    else
    {
        decoded_val[0] = '\0';
        decoded_val_len = 0;
    }

    char   *duped_decoded_val;

    duped_decoded_val = strndup( decoded_val, decoded_val_len );

    hash_table_set_pair( hash_table,
                         decoded_key,
                         decoded_key_len,
                         duped_decoded_val );

    return 0;
}

static int __make_www_form( http_msg_t    *msg )
{
    if( !msg->www_form_size )
        return 0;

    static char         body[1024*1024];
    int                 body_len = 0;

    http_www_form_t    *www_form;
    char                key[HTTP_WWW_FORM_KEY_LEN];
    char                val[HTTP_WWW_FORM_VAL_LEN];
    int                 key_len;
    int                 val_len;
    int                 i, l;

    for( i = 0; i < msg->www_form_size; i++ )
    {
        www_form = &msg->www_form[i];

        if( !www_form->key || !www_form->key[0] )
            return -1;

        l = strlen( www_form->key );

        key_len = http_percent_encode( key, sizeof(key),
                                       www_form->key, l, true );

        if( key_len < l )
            return -1;

        if( www_form->val )
        {
            l = strlen( www_form->val );

            val_len = http_percent_encode( val, sizeof(val),
                                           www_form->val, l, true );

            if( val_len < l )
                return -1;
        }
        else
        {
            val[0] = '\0';
            val_len = 0;
        }

        l = snprintf( body + body_len,
                      sizeof(body) - body_len,
                      "%s=%s%s", key, val,
                      i == msg->www_form_size - 1 ? "" : "&" );

        if( l >= sizeof(body) - body_len )
            return -1;

        body_len += l;
    }

    msg->raw_body = body;
    msg->raw_body_len = body_len;

    return 0;
}

static char *__make_raw_hdr_fields( unsigned       *len,
                                    http_msg_t     *msg )
{
    static char             raw_hdr[HTTP_HDR_MAX_LEN];

    *len = 0;

    if( !msg->raw_fields.total )
    {
        raw_hdr[0] = '\0';
        return raw_hdr;
    }

    http_raw_hdr_field_t   *raw_field;
    http_raw_hdr_field_t   *raw_field_next;
    http_raw_hdr_value_t   *raw_value;
    http_raw_hdr_value_t   *raw_value_next;

    char                   *p = raw_hdr;
    unsigned                rem_len = sizeof(raw_hdr);
    int                     r;

    LL_CHECK( &msg->raw_fields, msg->raw_fields.head );

    raw_field = PTRID_GET_PTR( msg->raw_fields.head );

    while( raw_field )
    {
        raw_field_next = PTRID_GET_PTR( raw_field->next );

        c_assert( !raw_field_next ||
                  raw_field_next->id == raw_field->next );

        if( !raw_field->key || !raw_field->key_len )
        {
            return NULL;
        }

        r = snprintf( p, rem_len, "%.*s: ",
                      raw_field->key_len,
                      raw_field->key );

        if( r >= rem_len )
            return NULL;

        rem_len -= r;
        p += r;

        if( !raw_field->raw_value.total )
        {
            r = snprintf( p, rem_len, "\r\n" );

            if( r > rem_len )
                return NULL;

            rem_len -= r;
            p += r;

            raw_field = raw_field_next;
            continue;
        }

        /* raw_value */
        LL_CHECK( &raw_field->raw_value, raw_field->raw_value.head );

        raw_value = PTRID_GET_PTR( raw_field->raw_value.head );

        while( raw_value )
        {
            raw_value_next = PTRID_GET_PTR( raw_value->next );

            c_assert( !raw_value_next ||
                      raw_value_next->id == raw_value->next );

            if( !raw_value->value || !raw_value->value_len )
            {
                return NULL;
            }

            r = snprintf( p, rem_len, "%.*s\r\n",
                          raw_value->value_len,
                          raw_value->value );

            if( r > rem_len )
                return NULL;

            rem_len -= r;
            p += r;

            raw_value = raw_value_next;
        }
        /* /raw_value */

        raw_field = raw_field_next;
    }

    *len = sizeof(raw_hdr) - rem_len;

    return raw_hdr;
}

static char *__make_hdr( unsigned long     *result_len,
                         http_conn_t       *http,
                         http_msg_t        *msg )
{
    char                start_line[MAX_URL_LEN];
    unsigned            start_line_len = 0;

    char                host[MAX_DOMAIN_LEN];
    unsigned            host_len = 0;

    /* NOTE: hardcoded because there is no standard len */
    char                content_length[256];
    unsigned            content_length_len = 0;

    /* NOTE: hardcoded because there is no standard len */
    char                connection[256];
    unsigned            connection_len = 0;
    char               *connection_value;

    char                user_agent[MAX_USER_AGENT_LEN];
    unsigned            user_agent_len = 0;

    char                location[MAX_URL_LEN];
    unsigned            location_len = 0;

    /* NOTE: hardcoded because there is no standard len */
    char                accept_encoding[256];
    unsigned            accept_encoding_len = 0;

    /* NOTE: hardcoded because there is no standard len */
    char                content_encoding[256];
    unsigned            content_encoding_len = 0;

    /* NOTE: hardcoded because there is no standard len */
    char                transfer_encoding[256];
    unsigned            transfer_encoding_len = 0;

    static char        *raw_hdr;
    unsigned            raw_hdr_len = 0;

    static char         hdr[HTTP_HDR_MAX_LEN];
    unsigned long       hdr_len = 0;

    *result_len = 0;

    if( http->client_r_cb )
    {
        char *method;

        if( msg->is_options_method )
            method = HTTP_OPTIONS_METHOD;
        else
        if( msg->raw_body_len )
            method = HTTP_POST_METHOD;
        else
            method = HTTP_GET_METHOD;

        start_line_len = snprintf( start_line, sizeof(start_line),
                                   "%s%.*s %s\r\n",
                                   method, msg->url_len, msg->url,
                                   HTTP_VER );
    }
    else
    {
        char *reason;

        c_assert( http->server_r_cb );

        /* TODO: add reason more phrases */
        /* NOTE: right now '200 OK' is needed for pidgin */
        if( msg->status_code == 200 )
            reason = " OK";
        else
            reason = "";

        start_line_len = snprintf( start_line, sizeof(start_line),
                                   "%s %d%s\r\n",
                                   HTTP_VER, msg->status_code,
                                   reason );
    }

    if( start_line_len > sizeof(start_line) )
    {
        LOGE( "http_id:0x%llx len:%u",
              PTRID_FMT( http->http_id ), start_line_len );

        G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
        return NULL;
    }

    if( msg->host_len )
    {
        host_len = snprintf( host, sizeof(host), "%s%.*s\r\n",
                             HTTP_HDR_HOST, msg->host_len, msg->host );

        if( host_len > sizeof(host) )
        {
            LOGE( "http_id:0x%llx len:%u",
                  PTRID_FMT( http->http_id ), host_len );

            G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
            return NULL;
        }
    }

    if( (http->server_r_cb || msg->raw_body_len) &&
        !msg->transfer_encoding_len )
    {
        content_length_len = snprintf( content_length,
                                       sizeof(content_length),
                                       "%s%u\r\n",
                                       HTTP_HDR_CONTENT_LENGTH,
                                       msg->raw_body_len );

        c_assert( content_length_len <= sizeof(content_length) );
    }

    if( msg->connection_close )
        connection_value = HTTP_HDR_CONNECTION_CLOSE;
    else
        connection_value = HTTP_HDR_CONNECTION_KEEP_ALIVE;

    connection_len = snprintf( connection,
                               sizeof(connection),
                               "%s%s\r\n",
                               HTTP_HDR_CONNECTION,
                               connection_value );

    c_assert( connection_len <= sizeof(connection) );

    if( msg->user_agent_len )
    {
        user_agent_len = snprintf( user_agent, sizeof(user_agent),
                                   "%s%.*s\r\n", HTTP_HDR_USER_AGENT,
                                   msg->user_agent_len, msg->user_agent );

        if( user_agent_len > sizeof(user_agent) )
        {
            LOGE( "http_id:0x%llx len:%u",
                  PTRID_FMT( http->http_id ), user_agent_len );

            G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
            return NULL;
        }
    }

    if( msg->location_len )
    {
        location_len = snprintf( location, sizeof(location),
                                 "%s%.*s\r\n", HTTP_HDR_LOCATION,
                                 msg->location_len, msg->location );

        if( location_len > sizeof(location) )
        {
            LOGE( "http_id:0x%llx len:%u",
                  PTRID_FMT( http->http_id ), location_len );

            G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
            return NULL;
        }
    }

    if( msg->accept_encoding_len )
    {
        accept_encoding_len = snprintf( accept_encoding,
                                        sizeof(accept_encoding),
                                        "%s%.*s\r\n",
                                        HTTP_HDR_ACCEPT_ENCODING,
                                        msg->accept_encoding_len,
                                        msg->accept_encoding );

        if( accept_encoding_len > sizeof(accept_encoding) )
        {
            LOGE( "http_id:0x%llx len:%u",
                  PTRID_FMT( http->http_id ), accept_encoding_len );

            G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
            return NULL;
        }
    }

    if( msg->content_encoding_len )
    {
        content_encoding_len = snprintf( content_encoding,
                                         sizeof(content_encoding),
                                         "%s%.*s\r\n",
                                         HTTP_HDR_CONTENT_ENCODING,
                                         msg->content_encoding_len,
                                         msg->content_encoding );

        if( content_encoding_len > sizeof(content_encoding) )
        {
            LOGE( "http_id:0x%llx len:%u",
                  PTRID_FMT( http->http_id ), content_encoding_len );

            G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
            return NULL;
        }
    }

    if( msg->transfer_encoding_len )
    {
        transfer_encoding_len = snprintf( transfer_encoding,
                                          sizeof(transfer_encoding),
                                          "%s%.*s\r\n",
                                          HTTP_HDR_TRANSFER_ENCODING,
                                          msg->transfer_encoding_len,
                                          msg->transfer_encoding );

        if( transfer_encoding_len > sizeof(transfer_encoding) )
        {
            LOGE( "http_id:0x%llx len:%u",
                  PTRID_FMT( http->http_id ), transfer_encoding_len );

            G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
            return NULL;
        }
    }

    raw_hdr = __make_raw_hdr_fields( &raw_hdr_len, msg );

    if( !raw_hdr )
    {
        LOGE( "http_id:0x%llx len:%u",
              PTRID_FMT( http->http_id ), raw_hdr_len );

        G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
        return NULL;
    }

    hdr_len = snprintf( hdr, sizeof(hdr),
                        "%.*s" /* start_line */
                        "%.*s" /* host */
                        "%.*s" /* content_length */
                        "%.*s" /* connection */
                        "%.*s" /* user_agent */
                        "%.*s" /* location */
                        "%.*s" /* accept_encoding */
                        "%.*s" /* content_encoding */
                        "%.*s" /* transfer_encoding */
                        "%.*s" /* raw header fields */
                        "\r\n",
                        start_line_len, start_line,
                        host_len, host,
                        content_length_len, content_length,
                        connection_len, connection,
                        user_agent_len, user_agent,
                        location_len, location,
                        accept_encoding_len, accept_encoding,
                        content_encoding_len, content_encoding,
                        transfer_encoding_len, transfer_encoding,
                        raw_hdr_len, raw_hdr );

    if( hdr_len > sizeof(hdr) )
    {
        LOGE( "http_id:0x%llx len:%lu",
              PTRID_FMT( http->http_id ), hdr_len );

        G_http_errno = HTTP_ERRNO_HDR_TOO_LARGE;
        return NULL;
    }

    *result_len = hdr_len;

    return hdr;
}

static int __post_data( http_conn_t    *http,
                        char           *hdr,
                        unsigned        hdr_len,
                        char           *body,
                        unsigned        body_len,
                        bool            flush_and_close )
{
    int         r;

    r = net_post_data( http->conn_id, hdr, hdr_len,
                       body_len ? false : flush_and_close );

    if( r )
    {
        LOGE( "http_id:0x%llx conn_id:0x%llx",
              PTRID_FMT( http->http_id ),
              PTRID_FMT( http->conn_id ) );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;
        return -1;
    }

    if( body_len )
    {
        r = net_post_data( http->conn_id, body, body_len,
                           flush_and_close );

        if( r )
        {
            LOGE( "http_id:0x%llx conn_id:0x%llx",
                  PTRID_FMT( http->http_id ),
                  PTRID_FMT( http->conn_id ) );

            G_http_errno = HTTP_ERRNO_GENERAL_ERR;
            return -1;
        }
    }

    return 0;
}

/* TODO: XXX: make struct with len + char everywhere */
static int __post_server_response( http_conn_t             *http,
                                   ptr_id_t                 msg_qid,
                                   char                    *hdr,
                                   unsigned                 hdr_len,
                                   char                    *body,
                                   unsigned                 body_len,
                                   bool                     connection_close,
                                   http_post_state_t       *state )
{
    http_messages_queue_elt_t      *q_elt;
    http_messages_queue_elt_t      *q_elt_next;

    *state = HTTP_POST_STATE_DEFAULT;

    q_elt = PTRID_GET_PTR( msg_qid );

    c_assert( q_elt->http_id == http->http_id &&
              http->messages_queue.total &&
              !q_elt->hdr && !q_elt->hdr_len &&
              !q_elt->body && !q_elt->body_len );

    if( !q_elt->connection_close )
        q_elt->connection_close = connection_close;

    if( msg_qid != http->messages_queue.head )
    {
        q_elt->hdr = malloc( hdr_len );
        memcpy( q_elt->hdr, hdr, hdr_len );

        q_elt->hdr_len = hdr_len;

        q_elt->body = malloc( body_len );
        memcpy( q_elt->body, body, body_len );

        q_elt->body_len = body_len;

        return 0;
    }

    LL_CHECK( &http->messages_queue,
              http->messages_queue.head );

    do
    {
        q_elt_next = PTRID_GET_PTR( q_elt->next );

        c_assert( q_elt->http_id == http->http_id &&
                  !(!q_elt->hdr_len && q_elt->body_len) );

        if( *state == HTTP_POST_STATE_DEFAULT )
        {
            if( q_elt->id == msg_qid )
            {
                if( __post_data( http, hdr, hdr_len,
                                 body, body_len,
                                 q_elt->connection_close ) == -1 )
                {
                    __free_queue_elt( &http->messages_queue, q_elt );
                    return -1;
                }
            }
            else
            if( __post_data( http, q_elt->hdr, q_elt->hdr_len,
                             q_elt->body, q_elt->body_len,
                             q_elt->connection_close ) == -1 )
            {
                __free_queue_elt( &http->messages_queue, q_elt );
                return -1;
            }

            if( q_elt->connection_close )
            {
                *state = HTTP_POST_STATE_SENT_CLOSE;
                http->sent_close = true;
            }
            else
            if( q_elt->connect_method )
            {
                *state = HTTP_POST_STATE_TUNNELING;
                http->tunneling_mode = true;
            }
        }
        else
        {
            LOGE( "http_id:0x%llx msg_id:0x%llx",
                  PTRID_FMT( http->http_id ),
                  PTRID_FMT( q_elt->id ) );
        }

        __free_queue_elt( &http->messages_queue, q_elt );

        q_elt = q_elt_next;
    }
    while( q_elt && q_elt->hdr_len );

    return 0;
}

static int __post_client_request( http_conn_t    *http,
                                  char           *hdr,
                                  unsigned        hdr_len,
                                  char           *body,
                                  unsigned        body_len )
{
    if( __post_data( http, hdr, hdr_len,
                     body, body_len, false ) == -1 )
    {
        return -1;
    }

    __messages_queue_push( http, false, false );

    return 0;
}

/******************* HTTP message parsing functions ***************************/

static int __parse_status_line( http_read_state_t     *read_state,
                                char                  *line,
                                int                    len )
{
    char    code_str[4];
    int     code;

    if( read_state->hdr_state.line_num != 1         ||
        len < HTTP_STATUS_MIN_LEN                   ||
        ( len > HTTP_STATUS_MIN_LEN &&
          line[HTTP_STATUS_2SP_POS] != ' ' )        ||
        line[HTTP_STATUS_1SP_POS] != ' '            ||
        !IS_DIGIT( line[HTTP_STATUS_CODE_1POS] )    ||
        !IS_DIGIT( line[HTTP_STATUS_CODE_2POS] )    ||
        !IS_DIGIT( line[HTTP_STATUS_CODE_3POS] ) )
    {
        return -1;
    }

    if( !strncmp( line, HTTP_VER10, HTTP_VER10_LEN ) )
    {
        read_state->hdr_state.is_http_ver10 = true;
    }
    else
    if( strncmp( line, HTTP_VER, HTTP_VER_LEN ) )
    {
        return -1;
    }

    snprintf( code_str, sizeof(code_str), "%.3s",
              &line[HTTP_STATUS_CODE_1POS] );

    code = atoi( code_str );

    if( code < HTTP_STATUS_CODE_MIN ||
        code > HTTP_STATUS_CODE_MAX )
    {
        return -1;
    }

    read_state->http_msg.status_code = code;

    read_state->state = S_HTTP_HEADER_FIELDS;

    return 0;
}

static int __parse_request_line( http_read_state_t    *read_state,
                                 char                 *line,
                                 int                   len )
{
    char       *p_begin;
    char       *p_end;

    if( read_state->hdr_state.line_num != 1 )
        return -1;

    if( len > HTTP_GET_METHOD_LEN &&
        !strncmp( line, HTTP_GET_METHOD,
                  HTTP_GET_METHOD_LEN ) )
    {
        p_begin = &line[HTTP_GET_METHOD_LEN];

        read_state->http_msg.url_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_GET_METHOD_LEN;
    }
    else
    if( len > HTTP_POST_METHOD_LEN &&
        !strncmp( line, HTTP_POST_METHOD,
                  HTTP_POST_METHOD_LEN ) )
    {
        p_begin = &line[HTTP_POST_METHOD_LEN];

        read_state->http_msg.url_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_POST_METHOD_LEN;
    }
    else
    if( len > HTTP_OPTIONS_METHOD_LEN &&
        !strncmp( line, HTTP_OPTIONS_METHOD,
                  HTTP_OPTIONS_METHOD_LEN ) )
    {
        p_begin = &line[HTTP_OPTIONS_METHOD_LEN];

        read_state->http_msg.url_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_OPTIONS_METHOD_LEN;

        read_state->http_msg.is_options_method = true;
    }
    else
    if( len > HTTP_CONNECT_METHOD_LEN &&
        !strncmp( line, HTTP_CONNECT_METHOD,
                  HTTP_CONNECT_METHOD_LEN ) )
    {
        p_begin = &line[HTTP_CONNECT_METHOD_LEN];

        read_state->http_msg.url_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_CONNECT_METHOD_LEN;

        read_state->http_msg.is_connect_method = true;
    }
    else
    {
        return -1;
    }

    if( !IS_PRINTABLE_ASCII( *p_begin ) )
        return -1;

    p_end = memchr( p_begin, ' ', len );

    if( !p_end )
        return -1;

    read_state->http_msg.url_len = p_end - p_begin;

    if( !read_state->http_msg.url_len )
        return -1;

    p_end++;

    if( len - (p_end - line) == HTTP_VER10_LEN &&
        !strncmp( p_end, HTTP_VER10, HTTP_VER10_LEN ) )
    {
        read_state->hdr_state.is_http_ver10 = true;
    }
    else
    if( len - (p_end - line) != HTTP_VER_LEN ||
        strncmp( p_end, HTTP_VER, HTTP_VER_LEN ) )
    {
        return -1;
    }

    read_state->state = S_HTTP_HEADER_FIELDS;

    return 0;
}

static int __parse_header_fields( http_read_state_t       *read_state,
                                  char                    *line,
                                  int                      len )
{
    if( !read_state->hdr_state.first_colon_begin &&
        len > 0 && line[0] != ' ' && line[0] != '\t' )
    {
        return -1;
    }

    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_CONTENT_LENGTH_LEN &&
        !strncasecmp( line, HTTP_HDR_CONTENT_LENGTH,
                      HTTP_HDR_CONTENT_LENGTH_LEN ) )
    {
        char    cont_len_str[32];
        int     cont_len;
        int     i;

        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_CONTENT_LENGTH_LEN )
            return -1;

        for( i = HTTP_HDR_CONTENT_LENGTH_LEN; i < len; i++ )
        {
            if( !IS_DIGIT( line[i] ) && line[i] != ' ' )
                return -1;
        }

        snprintf( cont_len_str, sizeof(cont_len_str), "%.*s",
                  (int) (len - HTTP_HDR_CONTENT_LENGTH_LEN),
                  &line[HTTP_HDR_CONTENT_LENGTH_LEN] );

        cont_len = atoi( cont_len_str );

        read_state->http_msg.raw_body_len = cont_len;
        read_state->hdr_state.no_content_length = false;
    }
    else
    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_CONNECTION_LEN &&
        !strncasecmp( line, HTTP_HDR_CONNECTION,
                      HTTP_HDR_CONNECTION_LEN ) )
    {
        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_CONNECTION_LEN )
            return -1;

        if( len == HTTP_HDR_CONNECTION_LEN +
                   HTTP_HDR_CONNECTION_CLOSE_LEN &&
            !strncasecmp( &line[HTTP_HDR_CONNECTION_LEN],
                          HTTP_HDR_CONNECTION_CLOSE,
                          HTTP_HDR_CONNECTION_CLOSE_LEN ) )
        {
            read_state->http_msg.connection_close = true;
        }
    }
    else
    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_HOST_LEN &&
        !strncasecmp( line, HTTP_HDR_HOST,
                      HTTP_HDR_HOST_LEN ) )
    {
        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_HOST_LEN ||
            read_state->http_msg.host_ndx )
        {
            return -1;
        }

        read_state->http_msg.host_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_HDR_HOST_LEN;

        read_state->http_msg.host_len = len - HTTP_HDR_HOST_LEN;
    }
    else
    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_USER_AGENT_LEN &&
        !strncasecmp( line, HTTP_HDR_USER_AGENT,
                      HTTP_HDR_USER_AGENT_LEN ) )
    {
        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_USER_AGENT_LEN ||
            read_state->http_msg.user_agent_ndx )
        {
            return -1;
        }

        read_state->http_msg.user_agent_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_HDR_USER_AGENT_LEN;

        read_state->http_msg.user_agent_len = len - HTTP_HDR_USER_AGENT_LEN;
    }
    else
    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_LOCATION_LEN &&
        !strncasecmp( line, HTTP_HDR_LOCATION,
                      HTTP_HDR_LOCATION_LEN ) )
    {
        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_LOCATION_LEN ||
            read_state->http_msg.location_ndx )
        {
            return -1;
        }

        read_state->http_msg.location_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_HDR_LOCATION_LEN;

        read_state->http_msg.location_len = len - HTTP_HDR_LOCATION_LEN;
    }
    else
    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_ACCEPT_ENCODING_LEN &&
        !strncasecmp( line, HTTP_HDR_ACCEPT_ENCODING,
                      HTTP_HDR_ACCEPT_ENCODING_LEN ) )
    {
        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_ACCEPT_ENCODING_LEN ||
            read_state->http_msg.accept_encoding_ndx )
        {
            return -1;
        }

        read_state->http_msg.accept_encoding_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_HDR_ACCEPT_ENCODING_LEN;

        read_state->http_msg.accept_encoding_len =
                        len - HTTP_HDR_ACCEPT_ENCODING_LEN;
    }
    else
    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_CONTENT_ENCODING_LEN &&
        !strncasecmp( line, HTTP_HDR_CONTENT_ENCODING,
                      HTTP_HDR_CONTENT_ENCODING_LEN ) )
    {
        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_CONTENT_ENCODING_LEN ||
            read_state->http_msg.content_encoding_ndx )
        {
            return -1;
        }

        read_state->http_msg.content_encoding_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_HDR_CONTENT_ENCODING_LEN;

        read_state->http_msg.content_encoding_len =
                        len - HTTP_HDR_CONTENT_ENCODING_LEN;
    }
    else
    if( read_state->hdr_state.first_colon_begin &&
        len >= HTTP_HDR_TRANSFER_ENCODING_LEN &&
        !strncasecmp( line, HTTP_HDR_TRANSFER_ENCODING,
                      HTTP_HDR_TRANSFER_ENCODING_LEN ) )
    {
        char    encoding_vals[256];

        read_state->hdr_state.is_raw_field = false;

        if( len <= HTTP_HDR_TRANSFER_ENCODING_LEN ||
            read_state->http_msg.transfer_encoding_ndx )
        {
            return -1;
        }

        read_state->http_msg.transfer_encoding_ndx =
                        read_state->hdr_state.last_line_begin +
                        HTTP_HDR_TRANSFER_ENCODING_LEN;

        read_state->http_msg.transfer_encoding_len =
                        len - HTTP_HDR_TRANSFER_ENCODING_LEN;

        snprintf( encoding_vals, sizeof(encoding_vals), "%.*s",
                  (int) (len - HTTP_HDR_TRANSFER_ENCODING_LEN),
                  &line[HTTP_HDR_TRANSFER_ENCODING_LEN] );

        if( strstr( encoding_vals, HTTP_HDR_CHUNKED_ENCODING ) )
            read_state->http_msg.transfer_encoding_chunked = true;
    }
    else
    if( read_state->hdr_state.first_colon_begin && len )
    {
        http_raw_hdr_field_t   *raw_field;
        http_raw_hdr_value_t   *raw_value;

        read_state->hdr_state.is_raw_field = true;

        raw_field = malloc( sizeof(http_raw_hdr_field_t) );
        memset( raw_field, 0, sizeof(http_raw_hdr_field_t) );

        LL_ADD_NODE( &read_state->http_msg.raw_fields, raw_field );

        raw_field->key_ndx = read_state->hdr_state.last_line_begin;

        raw_field->key_len = read_state->hdr_state.first_colon_begin -
                             read_state->hdr_state.last_line_begin;

        if( !raw_field->key_len )
            return -1;

        /* NOTE: check whether first symbol in line *
         * after ':' + SP exists or not             */
        if( read_state->hdr_state.first_colon_begin + 2 >=
            read_state->hdr_state.last_line_begin + len )
        {
            return 0;
        }

        raw_value = malloc( sizeof(http_raw_hdr_value_t) );
        memset( raw_value, 0, sizeof(http_raw_hdr_value_t) );

        LL_ADD_NODE( &raw_field->raw_value, raw_value );

        /* NOTE: first symbol after ':' + SP */
        raw_value->value_ndx = read_state->hdr_state.first_colon_begin + 2;

        raw_value->value_len = read_state->hdr_state.last_line_begin + len -
                               raw_value->value_ndx;
    }
    else
    if( !read_state->hdr_state.first_colon_begin && len )
    {
        http_raw_hdr_field_t   *raw_field;
        http_raw_hdr_value_t   *raw_value;

        if( !read_state->hdr_state.is_raw_field ||
            !read_state->http_msg.raw_fields.total )
        {
            return -1;
        }

        raw_field = PTRID_GET_PTR( read_state->http_msg.raw_fields.tail );

        if( !raw_field->raw_value.total )
            return -1;

        raw_value = malloc( sizeof(http_raw_hdr_value_t) );
        memset( raw_value, 0, sizeof(http_raw_hdr_value_t) );

        raw_value->value_ndx = read_state->hdr_state.last_line_begin;
        raw_value->value_len = len;

        LL_ADD_NODE( &raw_field->raw_value, raw_value );
    }
    else
    if( !len )
    {
        c_assert( !(read_state->http_msg.raw_body_len &&
                    read_state->http_msg.transfer_encoding_chunked) );

        read_state->state = S_HTTP_BODY;
    }

    return 0;
}

static int __parse_line( http_read_state_t        *read_state,
                         char                     *line,
                         int                       len )
{
    int     r;

    c_assert( len >= 0 );

    read_state->hdr_state.line_num++;
    
    if( len > HTTP_HDR_MAX_LINE_LEN ||
        read_state->hdr_state.line_num > HTTP_HDR_MAX_LINES )
    {
        return -1;
    }

    switch( read_state->state )
    {
        case S_HTTP_STATUS_LINE:

            r = __parse_status_line( read_state, line, len );
            break;

        case S_HTTP_REQUEST_LINE:

            r = __parse_request_line( read_state, line, len );
            break;

        case S_HTTP_HEADER_FIELDS:

            r = __parse_header_fields( read_state, line, len );
            break;

        default:
            return -1;
    }

    return r == -1 ? -1 : 0;
}

static int __parse_hdr( http_read_state_t          *read_state,
                        char                       *buf,
                        int                         buf_len )
{
    int         line_len;
    int         r;

    while( ( read_state->state == S_HTTP_STATUS_LINE  ||
             read_state->state == S_HTTP_REQUEST_LINE ||
             read_state->state == S_HTTP_HEADER_FIELDS ) &&
           read_state->n < buf_len )
    {
        if( read_state->n > HTTP_HDR_MAX_LEN )
            return -1;

        if( read_state->hdr_state.last_ch_type == HTTP_LF )
        {
            read_state->hdr_state.last_line_begin = read_state->n;

            read_state->hdr_state.first_colon_begin = 0;
        }

        if( IS_PRINTABLE_ASCII( buf[read_state->n] ) )
        {
            if( read_state->hdr_state.last_ch_type == HTTP_CR )
                return -1;

            if( buf[read_state->n] == ':' )
            {
                if( !read_state->n )
                    return -1;

                if( !read_state->hdr_state.first_colon_begin )
                    read_state->hdr_state.first_colon_begin = read_state->n;
            }

            read_state->hdr_state.last_ch_type = HTTP_CH;
        }
        else
        if( buf[read_state->n] == '\r' )
        {
            if( read_state->hdr_state.last_ch_type == HTTP_CR )
                return -1;

            read_state->hdr_state.last_ch_type = HTTP_CR;
        }
        else
        if( buf[read_state->n] == '\n' )
        {
            line_len = read_state->n -
                       read_state->hdr_state.last_line_begin;

            if( read_state->hdr_state.last_ch_type == HTTP_CR )
                line_len--;

            c_assert( line_len >= 0 );

            r = __parse_line( read_state,
                              &buf[read_state->hdr_state.last_line_begin],
                              line_len );

            if( r == -1 )
                return -1;

            read_state->hdr_state.last_ch_type = HTTP_LF;
        }
        else
        {
            return -1;
        }

        read_state->n++;
    }

    return 0;
}

static int __parse_body( http_read_state_t         *read_state,
                         char                      *buf,
                         int                        buf_len )
{
    if( read_state->n < buf_len )
    {
        if( !read_state->http_msg.raw_body_ndx )
        {
            if( !read_state->n )
                return -1;

            read_state->http_msg.raw_body_ndx = read_state->n;
        }

        if( read_state->n +
            read_state->http_msg.raw_body_len <= buf_len )
        {
            read_state->n = read_state->n +
                            read_state->http_msg.raw_body_len;

            read_state->state = S_HTTP_EOM;
        }
    }

    return 0;
}

static int __parse_chunk_size( http_read_state_t       *read_state,
                               char                    *buf,
                               int                      buf_len )
{
    char    c = buf[read_state->n];

    if( !read_state->chunked_body_state.size_begin )
    {
        if( !IS_HEX( c ) )
            return -1;

        read_state->chunked_body_state.size_begin = read_state->n;

        read_state->chunked_body_state.last_ch_type = HTTP_CH;

        read_state->n++;
        return 0;
    }

    if( !read_state->chunked_body_state.size_len &&
        IS_HEX( c ) )
    {
        read_state->n++;
        return 0;
    }

    if( !read_state->chunked_body_state.size_len &&
        ( c == '\r' || c == ';' || c == ' ' || c == '\t' ) )
    {
        read_state->chunked_body_state.size_len =
                read_state->n - read_state->chunked_body_state.size_begin;

        c_assert( read_state->chunked_body_state.size_len );

        if( c == '\r' )
            read_state->chunked_body_state.last_ch_type = HTTP_CR;

        char    size_str[32];
        int     n;

        n = snprintf( size_str, sizeof(size_str), "%.*s",
                      read_state->chunked_body_state.size_len,
                      buf + read_state->chunked_body_state.size_begin );

        if( n >= sizeof(size_str) )
            return -1;

        read_state->chunked_body_state.size_val =
                                    strtol( size_str, NULL, 16 );

        if( read_state->chunked_body_state.size_val < 0 )
            return -1;

        read_state->n++;
        return 0;
    }

    if( c == '\n' )
    {
        if( read_state->chunked_body_state.last_ch_type != HTTP_CR ||
            !read_state->chunked_body_state.size_len )
        {
            return -1;
        }

        read_state->chunked_body_state.last_ch_type = HTTP_LF;

        if( read_state->chunked_body_state.size_val )
            read_state->chunked_body_state.st = S_CHUNK_DATA;
        else
            read_state->chunked_body_state.st = S_CHUNK_LAST;

        read_state->n++;
        return 0;
    }

    if( c == '\r' )
    {
        if( read_state->chunked_body_state.last_ch_type == HTTP_CR ||
            !read_state->chunked_body_state.size_len )
        {
            return -1;
        }

        read_state->chunked_body_state.last_ch_type = HTTP_CR;

        read_state->n++;
        return 0;
    }

    if( IS_PRINTABLE_ASCII( c ) || c == '\t' )
    {
        if( read_state->chunked_body_state.last_ch_type == HTTP_CR ||
            !read_state->chunked_body_state.size_len )
        {
            return -1;
        }

        read_state->n++;
        return 0;
    }

    return -1;
}

static int __parse_chunk_data( http_read_state_t       *read_state,
                               char                    *buf,
                               int                      buf_len )
{
    if( !read_state->chunked_body_state.data_begin )
        read_state->chunked_body_state.data_begin = read_state->n;

    if( !read_state->chunked_body_state.has_data &&
        read_state->chunked_body_state.data_begin +
        read_state->chunked_body_state.size_val <= buf_len )
    {
        if( !read_state->http_msg.body )
        {
            c_assert( !read_state->http_msg.body_len );

            read_state->http_msg.body =
                    malloc( read_state->chunked_body_state.size_val );

            memcpy( read_state->http_msg.body,
                    buf + read_state->chunked_body_state.data_begin,
                    read_state->chunked_body_state.size_val );

            read_state->http_msg.body_len =
                    read_state->chunked_body_state.size_val;
        }
        else
        {
            c_assert( read_state->http_msg.body_len );

            read_state->http_msg.body =
                    realloc( read_state->http_msg.body,
                             read_state->http_msg.body_len +
                             read_state->chunked_body_state.size_val );

            memcpy( read_state->http_msg.body +
                    read_state->http_msg.body_len,
                    buf + read_state->chunked_body_state.data_begin,
                    read_state->chunked_body_state.size_val );

            read_state->http_msg.body_len +=
                    read_state->chunked_body_state.size_val;
        }

        read_state->n = read_state->chunked_body_state.data_begin +
                        read_state->chunked_body_state.size_val;

        read_state->chunked_body_state.has_data = true;
        return 0;
    }
    else
    if( !read_state->chunked_body_state.has_data &&
        read_state->chunked_body_state.data_begin +
        read_state->chunked_body_state.size_val > buf_len )
    {
        return 1;
    }
    else
    if( read_state->chunked_body_state.has_data &&
        read_state->n + 2 <= buf_len )
    {
        if( buf[read_state->n] == '\r' &&
            buf[read_state->n + 1] == '\n' )
        {
            read_state->n += 2;

            read_state->chunked_body_state.st = S_CHUNK_SIZE;

            read_state->chunked_body_state.last_ch_type = 0;

            read_state->chunked_body_state.size_begin = 0;
            read_state->chunked_body_state.size_len = 0;
            read_state->chunked_body_state.size_val = 0;

            read_state->chunked_body_state.data_begin = 0;
            read_state->chunked_body_state.has_data = false;

            return 0;
        }
    }
    else
    if( read_state->chunked_body_state.has_data &&
        read_state->n + 2 > buf_len )
    {
        return 1;
    }

    return -1;
}

static int __parse_chunk_last( http_read_state_t       *read_state,
                               char                    *buf,
                               int                      buf_len )
{
    if( !read_state->chunked_body_state.trailer_last_line_begin )
    {
        read_state->chunked_body_state.trailer_last_line_begin =
                                            read_state->n;
    }

    if( IS_PRINTABLE_ASCII( buf[read_state->n] ) )
    {
        if( read_state->chunked_body_state.last_ch_type == HTTP_CR )
            return -1;

        read_state->chunked_body_state.last_ch_type = HTTP_CH;

        read_state->n++;
        return 0;
    }

    if( buf[read_state->n] == '\r' )
    {
        if( read_state->chunked_body_state.last_ch_type == HTTP_CR )
            return -1;

        read_state->chunked_body_state.last_ch_type = HTTP_CR;

        read_state->chunked_body_state.trailer_last_line_len =
                read_state->n -
                read_state->chunked_body_state.trailer_last_line_begin;

        read_state->n++;
        return 0;
    }

    if( buf[read_state->n] == '\n' )
    {
        if( read_state->chunked_body_state.last_ch_type != HTTP_CR )
            return -1;

        if( !read_state->chunked_body_state.trailer_last_line_len )
        {
            read_state->n++;

            read_state->http_msg.raw_body_len =
                    read_state->n - read_state->http_msg.raw_body_ndx;

            read_state->state = S_HTTP_EOM;
            return 0;
        }

        read_state->chunked_body_state.last_ch_type = HTTP_LF;

        read_state->chunked_body_state.trailer_last_line_begin = 0;
        read_state->chunked_body_state.trailer_last_line_len = 0;

        read_state->n++;
        return 0;
    }

    return -1;
}

static int __parse_chunked_body( http_read_state_t     *read_state,
                                 char                  *buf,
                                 int                    buf_len )
{
    int     r;

    if( !read_state->http_msg.raw_body_ndx &&
        read_state->n < buf_len )
    {
        if( !read_state->n )
            return -1;

        read_state->http_msg.raw_body_ndx = read_state->n;
    }

    while( read_state->state == S_HTTP_BODY &&
           read_state->n < buf_len )
    {
        switch( read_state->chunked_body_state.st )
        {
            case S_CHUNK_SIZE:

                r = __parse_chunk_size( read_state,
                                        buf, buf_len );

                if( r == -1 )
                    return -1;

                break;

            case S_CHUNK_DATA:

                r = __parse_chunk_data( read_state,
                                        buf, buf_len );

                if( r == -1 )
                    return -1;
                else
                if( r == 1 )
                    return 0;

                break;

            case S_CHUNK_LAST:

                r = __parse_chunk_last( read_state,
                                        buf, buf_len );

                if( r == -1 )
                    return -1;

                break;

            default:
                c_assert( false );
        }
    }

    return 0;
}

static int __parse_body_until_closed( http_read_state_t    *read_state,
                                      char                 *buf,
                                      int                   buf_len,
                                      bool                  is_closed )
{
    if( read_state->n < buf_len )
    {
        if( !read_state->http_msg.raw_body_ndx )
        {
            if( !read_state->n )
                return -1;

            read_state->http_msg.raw_body_ndx = read_state->n;
        }

        read_state->http_msg.raw_body_len =
                            buf_len - read_state->http_msg.raw_body_ndx;

        if( is_closed )
        {
            read_state->n = read_state->n +
                            read_state->http_msg.raw_body_len;

            read_state->state = S_HTTP_EOM;
        }
    }

    return 0;
}

/* NOTE: network buffer may be reallocated,     *
 * so first use _ndx after convert them to ptrs */

static void __msg_fill_ptrs( http_msg_t    *msg,
                             char          *buf,
                             int            buf_len )
{
    if( msg->url_ndx )
    {
        c_assert( msg->url_ndx +
                  msg->url_len <= buf_len );

        msg->url = buf + msg->url_ndx;
    }

    if( msg->raw_body_ndx )
    {
        c_assert( msg->raw_body_ndx +
                  msg->raw_body_len <= buf_len );

        msg->raw_body = buf + msg->raw_body_ndx;
    }

    if( msg->host_ndx )
    {
        c_assert( msg->host_ndx +
                  msg->host_len <= buf_len );

        msg->host = buf + msg->host_ndx;
    }

    if( msg->user_agent_ndx )
    {
        c_assert( msg->user_agent_ndx +
                  msg->user_agent_len <= buf_len );

        msg->user_agent = buf + msg->user_agent_ndx;
    }

    if( msg->location_ndx )
    {
        c_assert( msg->location_ndx +
                  msg->location_len <= buf_len );

        msg->location = buf + msg->location_ndx;
    }

    if( msg->accept_encoding_ndx )
    {
        c_assert( msg->accept_encoding_ndx +
                  msg->accept_encoding_len <= buf_len );

        msg->accept_encoding = buf + msg->accept_encoding_ndx;
    }

    if( msg->content_encoding_ndx )
    {
        c_assert( msg->content_encoding_ndx +
                  msg->content_encoding_len <= buf_len );

        msg->content_encoding = buf + msg->content_encoding_ndx;
    }

    if( msg->transfer_encoding_ndx )
    {
        c_assert( msg->transfer_encoding_ndx +
                  msg->transfer_encoding_len <= buf_len );

        msg->transfer_encoding = buf + msg->transfer_encoding_ndx;
    }

    if( msg->raw_fields.total )
    {
        http_raw_hdr_field_t   *raw_field;
        http_raw_hdr_field_t   *raw_field_next;
        http_raw_hdr_value_t   *raw_value;
        http_raw_hdr_value_t   *raw_value_next;

        LL_CHECK( &msg->raw_fields, msg->raw_fields.head );

        raw_field = PTRID_GET_PTR( msg->raw_fields.head );

        while( raw_field )
        {
            raw_field_next = PTRID_GET_PTR( raw_field->next );

            c_assert( !raw_field_next ||
                      raw_field_next->id == raw_field->next );

            c_assert( !raw_field->key && raw_field->key_ndx &&
                      raw_field->key_ndx + raw_field->key_len <= buf_len );

            raw_field->key = buf + raw_field->key_ndx;

            if( !raw_field->raw_value.total )
            {
                raw_field = raw_field_next;
                continue;
            }

            /* raw_value */
            LL_CHECK( &raw_field->raw_value,
                      raw_field->raw_value.head );

            raw_value = PTRID_GET_PTR( raw_field->raw_value.head );

            while( raw_value )
            {
                raw_value_next = PTRID_GET_PTR( raw_value->next );

                c_assert( !raw_value_next ||
                          raw_value_next->id == raw_value->next );

                c_assert( !raw_value->value && raw_value->value_ndx &&
                          raw_value->value_ndx +
                          raw_value->value_len <= buf_len );

                raw_value->value = buf + raw_value->value_ndx;

                raw_value = raw_value_next;
            }
            /* /raw_value */

            raw_field = raw_field_next;
        }
    }
}

static int __decompress_raw_body( http_msg_t  *msg )
{
    c_assert( (msg->body && msg->body_len) ||
              (!msg->body && !msg->body_len) );

    c_assert( (msg->raw_body && msg->raw_body_len) ||
              (!msg->raw_body && !msg->raw_body_len) );

    if( (!msg->raw_body_len && !msg->body_len) ||
        msg->content_encoding_len != HTTP_HDR_GZIP_ENCODING_LEN ||
        strncasecmp( msg->content_encoding,
                     HTTP_HDR_GZIP_ENCODING,
                     HTTP_HDR_GZIP_ENCODING_LEN ) )
    {
        return 0;
    }

    char       *body;
    int         body_len;

    z_stream    strm = {0};
    char       *out;
    int         chunk_size;
    int         alloc_size;
    int         offset;
    int         r;

    body = msg->body ? msg->body :
                       msg->raw_body;

    body_len = msg->body_len ? msg->body_len :
                               msg->raw_body_len;

    chunk_size = body_len * HTTP_ZLIB_COEFFICIENT;
    alloc_size = chunk_size;

    out = malloc( alloc_size );

    offset = 0;

    r = inflateInit2( &strm,
                      HTTP_ZLIB_WINDOW_BITS |
                      HTTP_ZLIB_GZIP_ENCODING );

    if( r != Z_OK )
        return -1;

    strm.avail_in = body_len;
    strm.next_in = (unsigned char *) body;

    strm.avail_out = chunk_size;
    strm.next_out = (unsigned char *) out;

    while( true )
    {
        r = inflate( &strm, Z_NO_FLUSH );

        if( r != Z_OK )
            break;

        if( !strm.avail_out )
        {
            alloc_size += chunk_size;
            out = realloc( out, alloc_size );

            offset += chunk_size;

            strm.avail_out = chunk_size;
            strm.next_out = (unsigned char *) (out + offset);
        }
    }

    if( r == Z_BUF_ERROR && !strm.avail_in )
    {
        LOG( "" );
    }
    else
    if( r != Z_STREAM_END )
    {
        LOGE( "%d", r );

        free( out );

        inflateEnd( &strm );
        return -1;
    }

    if( msg->body )
        free( msg->body );

    msg->body = out;
    msg->body_len = strm.total_out;

    inflateEnd( &strm );
    return 0;
}

static int __parse_message( http_conn_t    *http,
                            char           *buf,
                            int             buf_len,
                            bool            is_closed )
{
    int         n;
    int         r;

    c_assert( buf_len > 0 );

    if( http->read_state.state == S_HTTP_STATUS_LINE  ||
        http->read_state.state == S_HTTP_REQUEST_LINE ||
        http->read_state.state == S_HTTP_HEADER_FIELDS )
    {
        r = __parse_hdr( &http->read_state, buf, buf_len );

        if( r == -1 )
            return HTTP_PARSE_ERROR;
    }

    c_assert( http->read_state.n <= buf_len );

    if( http->read_state.state == S_HTTP_BODY &&
        !http->read_state.hdr_state.body_until_closed )
    {
        if( http->server_r_cb && is_closed )
        {
            LOGE( "conn_id:0x%llx http_id:0x%llx",
                  PTRID_FMT( http->conn_id ),
                  PTRID_FMT( http->http_id ) );

            return HTTP_PARSE_ERROR;
        }
        else
        if( http->server_r_cb &&
            !http->read_state.http_msg.raw_body_len &&
            !http->read_state.http_msg.transfer_encoding_chunked )
        {
            http->read_state.state = S_HTTP_EOM;
        }
        else
        if( http->client_r_cb &&
            !http->read_state.http_msg.raw_body_len &&
            !http->read_state.http_msg.transfer_encoding_chunked )
        {
            /* NOTE: rfc 2616: 4.4 Message Length */
            if( http->read_state.hdr_state.no_content_length )
            {
                if( http->read_state.http_msg.status_code >= 100 &&
                    http->read_state.http_msg.status_code <= 199 )
                {
                    http->read_state.state = S_HTTP_EOM;
                }
                else
                if( http->read_state.http_msg.status_code == 204 ||
                    http->read_state.http_msg.status_code == 304 )
                {
                    http->read_state.state = S_HTTP_EOM;
                }
                else
                {
                    if( !http->read_state.http_msg.connection_close )
                        return HTTP_PARSE_ERROR;

                    http->read_state.hdr_state.body_until_closed = true;
                }
            }
            else
                http->read_state.state = S_HTTP_EOM;
        }
    }

    if( http->read_state.state == S_HTTP_BODY )
    {
        if( http->read_state.hdr_state.body_until_closed )
        {
            r = __parse_body_until_closed( &http->read_state,
                                           buf, buf_len, is_closed );
        }
        else
        if( http->read_state.http_msg.transfer_encoding_chunked )
        {
            r = __parse_chunked_body( &http->read_state,
                                      buf, buf_len );
        }
        else
        {
            r = __parse_body( &http->read_state,
                              buf, buf_len );
        }

        if( r == -1 )
            return HTTP_PARSE_ERROR;
    }

    if( http->read_state.state == S_HTTP_EOM )
    {
        if( http->read_state.hdr_state.is_http_ver10 )
            http->read_state.http_msg.connection_close = true;

        __msg_fill_ptrs( &http->read_state.http_msg,
                         buf, buf_len );

        r = __decompress_raw_body( &http->read_state.http_msg );

        if( r == -1 )
        {
            LOGE( "conn_id:0x%llx http_id:0x%llx",
                  PTRID_FMT( http->conn_id ),
                  PTRID_FMT( http->http_id ) );

            return HTTP_PARSE_ERROR;
        }

        if( http->client_r_cb )
        {
            if( __messages_queue_pop( http ) == -1 )
            {
                LOGE( "conn_id:0x%llx http_id:0x%llx",
                      PTRID_FMT( http->conn_id ),
                      PTRID_FMT( http->http_id ) );

                return HTTP_PARSE_ERROR;
            }

            http->client_r_cb( http->http_id,
                               http->udata_id,
                               &http->read_state.http_msg );

            if( http->read_state.http_msg.connection_close )
                return HTTP_PARSE_CONN_CLOSE;
        }
        else
        {
            ptr_id_t    msg_qid;

            msg_qid = __messages_queue_push(
                            http,
                            http->read_state.http_msg.connection_close,
                            http->read_state.http_msg.is_connect_method );

            c_assert( !http->got_connect_method );

            http->got_connect_method =
                            http->read_state.http_msg.is_connect_method;

            /* NOTE: if msg_qid == 0, that means don't need reply because *
             * it's connection close was sent already                     */
            http->server_r_cb( http->http_id,
                               http->udata_id,
                               msg_qid,
                               &http->read_state.http_msg );
        }

        http->messages_handled++;

        n = http->read_state.n;

        http_state_init( &http->read_state,
                         !!http->client_r_cb );

        return n;
    }

    return 0;
}

static int __parse_multiply_messages( http_conn_t  *http,
                                      char         *buf,
                                      int           len,
                                      bool          is_closed )
{
    net_state_t net_r;
    int         nread = 0;
    int         r = 0;

    do
    {
        if( http->got_connect_method )
        {
            LOGE( "conn_id:0x%llx http_id:0x%llx",
                  PTRID_FMT( http->conn_id ),
                  PTRID_FMT( http->http_id ) );

            r = net_shutdown_conn( http->conn_id, false );
            c_assert( !r );

            break;
        }

        r = __parse_message( http, &buf[nread],
                             len - nread, is_closed );

        if( r == HTTP_PARSE_ERROR )
        {
            LOGE( "conn_id:0x%llx http_id:0x%llx read_state:%u "
                  "host:%s url:%.*s status_code:%d",
                  PTRID_FMT( http->conn_id ),
                  PTRID_FMT( http->http_id ),
                  http->read_state.state, http->host,
                  http->read_state.http_msg.url_len,
                  http->read_state.http_msg.url,
                  http->read_state.http_msg.status_code );

            r = net_shutdown_conn( http->conn_id, false );
            c_assert( !r );

            break;
        }
        else
        if( r == HTTP_PARSE_CONN_CLOSE )
        {
            /* NOTE: conn may be already in flush_and_close state, *
             * so don't need to check return value                 */

            net_shutdown_conn( http->conn_id, false );
            break;
        }

        c_assert( r >= 0 );

        nread += r;

        net_r = net_is_est_conn( http->conn_id );

        /* NOTE: clonn was closed in user callback */
        if( net_r != NET_STATE_EST &&
            net_r != NET_STATE_FLUSH_AND_CLOSE )
        {
            LOGD( "http_id:0x%llx",
                  PTRID_FMT( http->http_id ) );

            break;
        }
    }
    while( r && nread < len );

    c_assert( nread <= len );

    return nread;
}

/******************* HTTP callbacks *******************************************/

static int __r_cb( conn_id_t        conn_id,
                   ptr_id_t         udata_id,
                   char            *buf,
                   int              len,
                   bool             is_closed )
{
    http_conn_t    *http;
    int             r;

    c_assert( conn_id && udata_id && buf );

    if( !len )
        return 0;

    http = PTRID_GET_PTR( udata_id );

    c_assert( http->http_id == udata_id &&
              http->conn_id == conn_id );

    LOG( "conn_id:0x%llx http_id:0x%llx messages_handled:%d "
         "sent_close:%d is_closed:%d",
         PTRID_FMT( conn_id ), PTRID_FMT( udata_id ),
         http->messages_handled,
         http->sent_close, is_closed );

    if( http->tunneling_mode )
    {
        http_msg_t  msg = {0};

        c_assert( http->server_r_cb );

        msg.raw_body = buf;
        msg.raw_body_len = len;
        msg.connection_close = is_closed;

        http->server_r_cb( http->http_id,
                           http->udata_id,
                           0, &msg );

        r = len;
    }
    else
    {
        r = __parse_multiply_messages( http, buf, len, is_closed );
    }

    return r;
}

static void __est_cb( conn_id_t     conn_id,
                      ptr_id_t      udata_id )
{
    http_conn_t        *http;
    tmr_id_t            tmr_id;

    c_assert( conn_id && udata_id );

    http = PTRID_GET_PTR( udata_id );

    c_assert( http->http_id == udata_id &&
              http->conn_id == conn_id );

    tmr_id = net_make_conn_tmr( http->conn_id, 0,
                                __check_messages_queue_tmr_cb,
                                cfg_http_check_messages_queue_interval );

    /* NOTE: error is impossible for just established conn */
    c_assert( tmr_id );

    LOG( "conn_id:0x%llx http_id:0x%llx tmr_id:0x%llx",
         PTRID_FMT( conn_id ), PTRID_FMT( udata_id ),
         PTRID_FMT( tmr_id ) );

    http->est_cb( http->http_id, http->udata_id );
}

static void __clo_cb( conn_id_t     conn_id,
                      ptr_id_t      udata_id,
                      int           code )
{
    http_conn_t    *http;

    c_assert( conn_id && udata_id );

    http = PTRID_GET_PTR( udata_id );

    c_assert( http->http_id == udata_id &&
              http->conn_id == conn_id );

    LOG( "conn_id:0x%llx http_id:0x%llx",
         PTRID_FMT( conn_id ), PTRID_FMT( udata_id ) );

    http->clo_cb( udata_id, http->udata_id, code );

    /* NOTE: alive http_conn_t could be freed *
     * only in this function                  */
    __free_http( http );
}

static void __tmr_cb( conn_id_t     conn_id,
                      ptr_id_t      conn_udata_id,
                      tmr_id_t      tmr_id,
                      ptr_id_t      tmr_udata_id )
{
    http_conn_t        *http;
    http_tmr_node_t    *tmr;

    c_assert( conn_id && conn_udata_id &&
              tmr_id && tmr_udata_id );

    http = PTRID_GET_PTR( conn_udata_id );

    c_assert( http->http_id == conn_udata_id &&
              http->conn_id == conn_id );

    tmr = PTRID_GET_PTR( tmr_udata_id );

    c_assert( tmr->id == tmr_udata_id &&
              tmr->net_tmr_id == tmr_id &&
              tmr->http_id == conn_udata_id );

    LOG( "conn_id:0x%llx http_id:0x%llx "
         "tmr_id:0x%llx http_tmr_id:0x%llx",
         PTRID_FMT( conn_id ), PTRID_FMT( conn_udata_id ),
         PTRID_FMT( tmr_id ), PTRID_FMT( tmr_udata_id ) );

    tmr->cb( http->http_id, http->udata_id,
             tmr->id, tmr->udata_id );
}

static ptr_id_t __dup_udata_cb( conn_id_t   conn_id,
                                ptr_id_t    udata_id )
{
    http_conn_t        *l_http;
    http_conn_t        *http;
    http_id_t           l_prev_id;
    http_id_t           prev_id;

    c_assert( conn_id && udata_id );

    l_http = PTRID_GET_PTR( udata_id );

    c_assert( l_http->http_id == udata_id &&
              l_http->conn_id != conn_id );

    http = malloc( sizeof(http_conn_t) );
    memset( http, 0, sizeof(http_conn_t) );

    http->http_id = PTRID( http );

    http->conn_id = conn_id;

    http->server_r_cb = l_http->child_r_cb;
    http->est_cb = l_http->child_est_cb;
    http->clo_cb = l_http->child_clo_cb;

    l_prev_id = l_http->http_id;
    prev_id = http->http_id;

    http->is_in_dup_udata = true;
    l_http->is_in_dup_udata = true;

    http->udata_id = l_http->dup_udata_cb( http->http_id,
                                           l_http->udata_id );

    c_assert( l_http->http_id == l_prev_id &&
              http->http_id == prev_id &&
              http->udata_id != l_http->udata_id );

    http->is_in_dup_udata = false;
    l_http->is_in_dup_udata = false;

    http_state_init( &http->read_state, false );

    LOG( "conn_id:0x%llx l_http_id:0x%llx "
         "http_id:0x%llx udata_id:0x%llx",
         PTRID_FMT( http->conn_id ),
         PTRID_FMT( l_http->http_id ),
         PTRID_FMT( http->http_id ),
         PTRID_FMT( http->udata_id ) );

    return http->http_id;
}

/******************* HTTP api *************************************************/

http_id_t http_make_conn( net_host_t           *host,
                          http_client_r_uh_t    r_uh_cb,
                          http_est_uh_t         est_uh_cb,
                          http_clo_uh_t         clo_uh_cb,
                          ptr_id_t              udata_id )
{
    http_conn_t        *http;
    http_id_t           http_id;

    G_http_errno = HTTP_ERRNO_OK;

    if( !host || !host->hostname || !host->port )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return 0;
    }

    if( !r_uh_cb || !est_uh_cb || !clo_uh_cb || !udata_id )
    {
        LOGE( "host:%s:%s", host->hostname, host->port );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return 0;
    }

    http = malloc( sizeof(http_conn_t) );
    memset( http, 0, sizeof(http_conn_t) );

    http_id = PTRID( http );

    strncpy( http->host, host->hostname, sizeof(http->host) );
    strncpy( http->port, host->port, sizeof(http->port) );
    http->host[sizeof(http->host) - 1] = '\0';
    http->port[sizeof(http->port) - 1] = '\0';

    http->http_id = http_id;
    http->udata_id = udata_id;

    http->client_r_cb = r_uh_cb;
    http->est_cb = est_uh_cb;
    http->clo_cb = clo_uh_cb;

    http_state_init( &http->read_state, true );

    http->conn_id = net_make_conn( host, __r_cb, __est_cb,
                                   __clo_cb, http_id );

    if( !http->conn_id )
    {
        LOGE( "host:%s:%s", host->hostname, host->port );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;

        __free_http( http );
        return 0;
    }

    LOG( "conn_id:0x%llx http_id:0x%llx "
         "host:%s:%s use_ssl:%d",
         PTRID_FMT( http->conn_id ), PTRID_FMT( http_id ),
         host->hostname, host->port, host->use_ssl );

    return http_id;
}

http_id_t http_make_listen( http_server_r_uh_t  child_r_cb,
                            http_est_uh_t       child_est_cb,
                            http_clo_uh_t       child_clo_cb,
                            http_dup_udata_t    dup_udata_cb,
                            http_clo_uh_t       clo_cb,
                            ptr_id_t            udata_id,
                            int                 port,
                            bool                use_ssl )
{
    http_conn_t    *http;
    http_id_t       http_id;

    G_http_errno = HTTP_ERRNO_OK;

    if( !child_r_cb || !child_est_cb ||
        !child_clo_cb || !dup_udata_cb ||
        !clo_cb || !udata_id || port <= 0 )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return 0;
    }

    http = malloc( sizeof(http_conn_t) );
    memset( http, 0, sizeof(http_conn_t) );

    http_id = PTRID( http );

    http->listen_port = port;

    http->http_id = http_id;
    http->udata_id = udata_id;

    http->child_r_cb = child_r_cb;
    http->child_est_cb = child_est_cb;
    http->child_clo_cb = child_clo_cb;
    http->dup_udata_cb = dup_udata_cb;

    http->clo_cb = clo_cb;

    http->conn_id = net_make_listen( __r_cb, __est_cb,
                                     __clo_cb, __dup_udata_cb,
                                     __clo_cb, http_id,
                                     port, use_ssl );

    if( !http->conn_id )
    {
        LOGE( "post:%d", port );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;

        __free_http( http );
        return 0;
    }

    LOG( "conn_id:0x%llx http_id:0x%llx host:%d use_ssl:%d",
         PTRID_FMT( http->conn_id ), PTRID_FMT( http_id ),
         port, use_ssl );

    return http_id;
}

int http_post_data( http_id_t           http_id,
                    http_msg_t         *msg,
                    ptr_id_t            msg_qid,
                    http_post_state_t  *state )
{
    http_conn_t        *http;
    char               *hdr;
    unsigned long       hdr_len;

    G_http_errno = HTTP_ERRNO_OK;

    if( !http_id || !msg )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    http = PTRID_GET_PTR( http_id );
    c_assert( http->http_id == http_id );

    c_assert( (http->client_r_cb && !http->server_r_cb) ||
              (!http->client_r_cb && http->server_r_cb) );

    if( http->is_in_dup_udata ||
        http->sent_close ||
        http->tunneling_mode )
    {
        c_assert( !http->client_r_cb );

        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_STATE;
        return -1;
    }

    if( ( http->client_r_cb && (msg_qid || state) ) ||
        ( http->server_r_cb && (!msg_qid || !state) ) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( http->client_r_cb && (msg->status_code  ||
                              !msg->url_len     ||
                              !msg->url) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( http->server_r_cb && (msg->url_len      ||
                              msg->url          ||
                              msg->status_code < HTTP_STATUS_CODE_MIN ||
                              msg->status_code > HTTP_STATUS_CODE_MAX) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( http->client_r_cb &&
        (!msg->host_len || !msg->host) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( (msg->raw_body_len && !msg->raw_body) ||
        (!msg->raw_body_len && msg->raw_body) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( (msg->www_form && !msg->www_form_size) ||
        (!msg->www_form && msg->www_form_size) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( msg->www_form_size && msg->raw_body_len )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( http->server_r_cb && (msg->www_form ||
                              msg->www_form_size) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( http->server_r_cb && (msg->accept_encoding ||
                              msg->accept_encoding_len) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( http->client_r_cb && (msg->content_encoding ||
                              msg->content_encoding_len ||
                              msg->transfer_encoding ||
                              msg->transfer_encoding_len) )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( msg->is_connect_method )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    if( __make_www_form( msg ) )
        return -1;

    hdr = __make_hdr( &hdr_len, http, msg );
    if( !hdr )
        return -1;

    if( http->client_r_cb )
    {
        if( __post_client_request( http, hdr, hdr_len, msg->raw_body,
                                   msg->raw_body_len ) == -1 )
        {
            return -1;
        }
    }
    else
    {
        c_assert( http->server_r_cb );

        if( __post_server_response( http, msg_qid, hdr, hdr_len,
                                    msg->raw_body, msg->raw_body_len,
                                    msg->connection_close, state ) == -1 )
        {
            return -1;
        }
    }

    LOG( "http_id:0x%llx conn_id:0x%llx state:%d",
         PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ),
         state ? *state : 0 );

    return 0;
}

int http_post_raw_data( http_id_t       http_id,
                        http_msg_t     *msg )
{
    http_conn_t        *http;
    int                 r;

    G_http_errno = HTTP_ERRNO_OK;

    if( !http_id || !msg )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    http = PTRID_GET_PTR( http_id );
    c_assert( http->http_id == http_id );

    if( http->client_r_cb )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_CONN;
        return -1;
    }

    c_assert( http->server_r_cb );

    if( http->is_in_dup_udata ||
        http->sent_close      ||
        !http->tunneling_mode )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_STATE;
        return -1;
    }

    c_assert( !http->messages_queue.total );

    if( !msg->raw_body_len || !msg->raw_body )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    r = net_post_data( http->conn_id,
                       msg->raw_body,
                       msg->raw_body_len, false );

    if( r )
    {
        LOGE( "http_id:0x%llx conn_id:0x%llx",
              PTRID_FMT( http->http_id ),
              PTRID_FMT( http->conn_id ) );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;
        return -1;
    }

    LOG( "http_id:0x%llx conn_id:0x%llx",
         PTRID_FMT( http_id ),
         PTRID_FMT( http->conn_id ) );

    return 0;
}

http_tmr_id_t http_make_tmr( http_id_t        http_id,
                             ptr_id_t         udata_id,
                             http_tmr_cb_t    cb,
                             struct timeval  *timeout )
{
    http_conn_t        *http;
    http_tmr_node_t    *tmr;

    G_http_errno = HTTP_ERRNO_OK;

    if( !http_id )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return 0;
    }

    http = PTRID_GET_PTR( http_id );
    c_assert( http->http_id == http_id );

    if( http->is_in_dup_udata || http->sent_close )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_STATE;
        return 0;
    }

    tmr = malloc( sizeof(http_tmr_node_t) );
    memset( tmr, 0, sizeof(http_tmr_node_t) );

    tmr->http_id = http_id;

    tmr->udata_id = udata_id;
    tmr->cb = cb;

    LL_ADD_NODE( &http->tmr_list, tmr );

    tmr->net_tmr_id = net_make_conn_tmr( http->conn_id,
                                         http->tmr_list.tail,
                                         __tmr_cb, timeout );

    if( !tmr->net_tmr_id )
    {
        LOGE( "http_id:0x%llx conn_id:0x%llx udata_id:0x%llx",
              PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ),
              PTRID_FMT( udata_id ) );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;

        LL_DEL_NODE( &http->tmr_list, tmr->id );

        memset( tmr, 0, sizeof(http_tmr_node_t) );
        free( tmr );

        return 0;
    }

    LOG( "http_id:0x%llx conn_id:0x%llx udata_id:0x%llx "
         "tmr_id:0x%llx net_tmr_id:0x%llx",
         PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ),
         PTRID_FMT( udata_id ), PTRID_FMT( tmr->id ),
         PTRID_FMT( tmr->net_tmr_id ) );

    return http->tmr_list.tail;
}

int http_del_tmr( http_id_t     http_id,
                  http_tmr_id_t tmr_id )
{
    http_conn_t        *http;
    http_tmr_node_t    *tmr;
    tmr_id_t            net_tmr_id;
    int                 r;

    G_http_errno = HTTP_ERRNO_OK;

    if( !http_id )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    http = PTRID_GET_PTR( http_id );
    c_assert( http->http_id == http_id );

    if( http->is_in_dup_udata || http->sent_close )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_STATE;
        return -1;
    }

    tmr = PTRID_GET_PTR( tmr_id );
    c_assert( tmr->id == tmr_id );

    LL_DEL_NODE( &http->tmr_list, tmr_id );

    net_tmr_id = tmr->net_tmr_id;

    memset( tmr, 0, sizeof(http_tmr_node_t) );
    free( tmr );

    r = net_del_conn_tmr( http->conn_id, net_tmr_id );
    if( r )
    {
        LOGE( "http_id:0x%llx conn_id:0x%llx "
              "tmr_id:0x%llx net_tmr_id:0x%llx",
              PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ),
              PTRID_FMT( tmr_id ), PTRID_FMT( net_tmr_id ) );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;
        return -1;
    }

    LOG( "http_id:0x%llx conn_id:0x%llx "
         "tmr_id:0x%llx net_tmr_id:0x%llx",
         PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ),
         PTRID_FMT( tmr_id ), PTRID_FMT( net_tmr_id ) );

    return 0;
}

int http_shutdown( http_id_t    http_id,
                   bool         flush_and_close )
{
    http_conn_t    *http;
    int             r;

    G_http_errno = HTTP_ERRNO_OK;

    if( !http_id )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return -1;
    }

    http = PTRID_GET_PTR( http_id );
    c_assert( http->http_id == http_id );

    if( http->is_in_dup_udata || http->sent_close )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_STATE;
        return -1;
    }

    r = net_shutdown_conn( http->conn_id, flush_and_close );

    if( r )
    {
        LOGE( "http_id:0x%llx conn_id:0x%llx",
              PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ) );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;
        return -1;
    }

    LOG( "http_id:0x%llx conn_id:0x%llx",
         PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ) );

    return 0;
}

http_state_t http_is_est( http_id_t http_id )
{
    http_conn_t    *http;
    net_state_t     net_r;
    http_state_t    http_r = HTTP_STATE_ERROR;

    G_http_errno = HTTP_ERRNO_OK;

    if( !http_id )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return HTTP_STATE_ERROR;
    }

    http = PTRID_GET_PTR( http_id );
    c_assert( http->http_id == http_id );

    if( http->is_in_dup_udata )
    {
        LOGE( "http_id:0x%llx", PTRID_FMT( http_id ) );

        G_http_errno = HTTP_ERRNO_WRONG_STATE;
        return HTTP_STATE_ERROR;
    }

    net_r = net_is_est_conn( http->conn_id );
    if( net_r == NET_STATE_ERROR )
    {
        LOGE( "http_id:0x%llx conn_id:0x%llx",
              PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ) );

        G_http_errno = HTTP_ERRNO_GENERAL_ERR;
        return HTTP_STATE_ERROR;
    }

    LOGD( "http_id:0x%llx conn_id:0x%llx is_est:%d",
          PTRID_FMT( http_id ), PTRID_FMT( http->conn_id ), net_r );

    switch( net_r )
    {
        case NET_STATE_NOT_EST:

            http_r = HTTP_STATE_NOT_EST;
            break;

        case NET_STATE_EST:

            if( http->tunneling_mode )
                http_r = HTTP_STATE_TUNNELING;
            else
                http_r = HTTP_STATE_EST;

            break;

        case NET_STATE_FLUSH_AND_CLOSE:

            http_r = HTTP_STATE_SENT_CLOSE;
            break;

        default:
            c_assert( false );
    }

    return http_r;
}

http_msg_t *http_dup_msg( http_msg_t   *msg,
                          bool          no_proxy_url )
{
    http_msg_t     *duped_msg;

    duped_msg = malloc( sizeof(http_msg_t) );
    memset( duped_msg, 0, sizeof(http_msg_t) );

    if( msg->url_len )
    {
        char   *url;
        int     url_len;
        char   *p = NULL;

        if( no_proxy_url &&
            msg->url_len > HTTP_URL_PROTOCOL_NO_SSL_LEN &&
            !strncasecmp( msg->url, HTTP_URL_PROTOCOL_NO_SSL,
                          HTTP_URL_PROTOCOL_NO_SSL_LEN ) )
        {
            p = memchr( msg->url + HTTP_URL_PROTOCOL_NO_SSL_LEN, '/',
                        msg->url_len - HTTP_URL_PROTOCOL_NO_SSL_LEN );
        }
        else
        if( no_proxy_url &&
            msg->url_len > HTTP_URL_PROTOCOL_SSL_LEN &&
            !strncasecmp( msg->url, HTTP_URL_PROTOCOL_SSL,
                          HTTP_URL_PROTOCOL_SSL_LEN ) )
        {
            p = memchr( msg->url + HTTP_URL_PROTOCOL_SSL_LEN, '/',
                        msg->url_len - HTTP_URL_PROTOCOL_SSL_LEN );
        }

        if( p )
        {
            url = p;
            url_len = msg->url_len - (p - msg->url);
        }
        else
        {
            url = msg->url;
            url_len = msg->url_len;
        }

        duped_msg->url = malloc( url_len );
        memcpy( duped_msg->url, url, url_len );

        duped_msg->url_len = url_len;
    }

    if( msg->raw_body_len )
    {
        duped_msg->raw_body = malloc( msg->raw_body_len );
        memcpy( duped_msg->raw_body, msg->raw_body,
                msg->raw_body_len );

        duped_msg->raw_body_len = msg->raw_body_len;
    }

    if( msg->body_len )
    {
        duped_msg->body = malloc( msg->body_len );
        memcpy( duped_msg->body, msg->body,
                msg->body_len );

        duped_msg->body_len = msg->body_len;
    }

    duped_msg->status_code = msg->status_code;

    duped_msg->is_options_method = msg->is_options_method;

    duped_msg->connection_close = msg->connection_close;

    if( msg->host_len )
    {
        duped_msg->host = malloc( msg->host_len );
        memcpy( duped_msg->host, msg->host, msg->host_len );

        duped_msg->host_len = msg->host_len;
    }

    if( msg->user_agent_len )
    {
        duped_msg->user_agent = malloc( msg->user_agent_len );
        memcpy( duped_msg->user_agent, msg->user_agent,
                msg->user_agent_len );

        duped_msg->user_agent_len = msg->user_agent_len;
    }

    if( msg->location_len )
    {
        duped_msg->location = malloc( msg->location_len );
        memcpy( duped_msg->location, msg->location,
                msg->location_len );

        duped_msg->location_len = msg->location_len;
    }

    if( msg->accept_encoding_len )
    {
        duped_msg->accept_encoding = malloc( msg->accept_encoding_len );
        memcpy( duped_msg->accept_encoding, msg->accept_encoding,
                msg->accept_encoding_len );

        duped_msg->accept_encoding_len = msg->accept_encoding_len;
    }

    if( msg->content_encoding_len )
    {
        duped_msg->content_encoding = malloc( msg->content_encoding_len );
        memcpy( duped_msg->content_encoding, msg->content_encoding,
                msg->content_encoding_len );

        duped_msg->content_encoding_len = msg->content_encoding_len;
    }

    if( msg->transfer_encoding_len )
    {
        duped_msg->transfer_encoding = malloc( msg->transfer_encoding_len );
        memcpy( duped_msg->transfer_encoding, msg->transfer_encoding,
                msg->transfer_encoding_len );

        duped_msg->transfer_encoding_len = msg->transfer_encoding_len;
    }

    duped_msg->transfer_encoding_chunked = msg->transfer_encoding_chunked;

    __dup_raw_hdr_fields( duped_msg, msg );

    duped_msg->is_duped = true;

    return duped_msg;
}

void http_free_msg( http_msg_t *msg )
{
    c_assert( msg->is_duped );

    if( msg->url )
        free( msg->url );

    if( msg->raw_body )
        free( msg->raw_body );

    if( msg->body )
        free( msg->body );

    if( msg->host )
        free( msg->host );

    if( msg->user_agent )
        free( msg->user_agent );

    if( msg->location )
        free( msg->location );

    if( msg->accept_encoding )
        free( msg->accept_encoding );

    if( msg->content_encoding )
        free( msg->content_encoding );

    if( msg->transfer_encoding )
        free( msg->transfer_encoding );

    __free_raw_hdr_fields( msg );

    memset( msg, 0, sizeof(http_msg_t) );
    free( msg );
}

net_host_t *http_msg_get_host( http_msg_t  *msg,
                               bool         is_ssl )
{
    G_http_errno = HTTP_ERRNO_OK;

    if( !msg->url || !msg->url_len )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return NULL;
    }

    if( !msg->is_connect_method &&
        (!msg->host || !msg->host_len) )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return NULL;
    }

    if( msg->is_connect_method && is_ssl )
    {
        LOGE( "" );

        G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
        return NULL;
    }

    net_host_t     *host;
    char           *msg_host;
    int             msg_host_len;
    int             port;
    char           *port_ptr;
    char            port_buf[6]; /* NOTE: 65535 + '\0' */
    int             n;

    if( msg->is_connect_method )
    {
        port = HTTP_DEFAULT_PORT;
    }
    else
    {
        port = is_ssl ? HTTP_DEFAULT_SSL_PORT :
                        HTTP_DEFAULT_PORT;
    }

    if( !msg->is_connect_method )
    {
        if( msg->url_len >= HTTP_URL_PROTOCOL_NO_SSL_LEN &&
            !strncasecmp( msg->url, HTTP_URL_PROTOCOL_NO_SSL,
                          HTTP_URL_PROTOCOL_NO_SSL_LEN ) )
        {
            port = HTTP_DEFAULT_PORT;
            is_ssl = false;
        }
        else
        if( msg->url_len >= HTTP_URL_PROTOCOL_SSL_LEN &&
            !strncasecmp( msg->url, HTTP_URL_PROTOCOL_SSL,
                          HTTP_URL_PROTOCOL_SSL_LEN ) )
        {
            port = HTTP_DEFAULT_SSL_PORT;
            is_ssl = true;
        }
    }

    if( msg->is_connect_method )
    {
        msg_host = msg->url;
        msg_host_len = msg->url_len;
    }
    else
    {
        msg_host = msg->host;
        msg_host_len = msg->host_len;
    }

    host = malloc( sizeof(net_host_t) );
    memset( host, 0, sizeof(net_host_t) );

    /* NOTE: +1 is '\0' */
    host->hostname = malloc( msg_host_len + 1 );
    memcpy( host->hostname, msg_host, msg_host_len );
    host->hostname[msg_host_len] = '\0';

    if( (port_ptr = strchr( host->hostname, ':' )) )
    {
        port = atoi( port_ptr + 1 );

        if( port <= 0 && port > 65535 )
        {
            LOGE( "" );

            free( host->hostname );
            free( host );

            G_http_errno = HTTP_ERRNO_WRONG_PARAMS;
            return NULL;
        }

        *port_ptr = '\0';
    }

    n = snprintf( port_buf, sizeof(port_buf), "%d", port );
    c_assert( n <= 5 );

    /* NOTE: +1 is '\0' */
    host->port = malloc( n + 1 );
    memcpy( host->port, port_buf, n + 1 );

    host->use_ssl = is_ssl;

    net_update_host( host );

    return host;
}

int http_percent_encode( char      *out,
                         int        out_len,
                         char      *in,
                         int        in_len,
                         bool       is_www_form )
{
    int     i;
    int     l;
    int     n = 0;

    G_http_errno = HTTP_ERRNO_OK;

    if( out_len < in_len * 3 + 1 )
        return -1;

    for( i = 0; i < in_len; i++ )
    {
        if( is_www_form && in[i] == ' ' )
        {
            out[n] = '+';
            n++;
        }
        else
        if( g_percent_encoding_map[ (unsigned char) in[i] ] )
        {
            l = snprintf( out + n, out_len - n, "%%%02X",
                          (unsigned char) in[i] );

            n += l;
        }
        else
        {
            out[n] = in[i];
            n++;
        }
    }

    c_assert( n < out_len );

    out[n] = '\0';

    return n;
}

int http_percent_decode( char      *out,
                         int        out_len,
                         char      *in,
                         int        in_len,
                         bool       is_www_form )
{
    G_http_errno = HTTP_ERRNO_OK;

    if( out_len < in_len + 1 )
        return -1;

    enum {
        STATE_DEFAULT = 0,
        STATE_FIRST_HEX,
        STATE_SECOND_HEX
    };

    int     state = STATE_DEFAULT;
    int     n = 0;
    int     i;

    for( i = 0; i < in_len; i++ )
    {
        if( !IS_PRINTABLE_ASCII( in[i] ) )
            return -1;

        switch( state )
        {
            case STATE_DEFAULT:

                if( in[i] == '%' )
                {
                    state = STATE_FIRST_HEX;
                }
                else
                if( is_www_form && in[i] == '+' )
                {
                    out[n] = ' ';
                    n++;
                }
                else
                {
                    out[n] = in[i];
                    n++;
                }

                break;

            case STATE_FIRST_HEX:

                if( IS_HEX( in[i] ) )
                    state = STATE_SECOND_HEX;
                else
                    return -1;

                break;

            case STATE_SECOND_HEX:

                if( IS_HEX( in[i] ) )
                {
                    char        hex[3];
                    long int    r;

                    hex[0] = in[i - 1];
                    hex[1] = in[i];
                    hex[2] = '\0';

                    r = strtol( hex, (char **) NULL, 16 );

                    if( !( r >= 0 && r <= 255 ) )
                        return -1;

                    out[n] = r;
                    n++;

                    state = STATE_DEFAULT;
                }
                else
                    return -1;

                break;

            default:
                c_assert( false );
        }
    }

    out[n] = '\0';

    return n;
}

hash_table_t *http_parse_www_form( char    *form,
                                   int      form_len,
                                   bool     is_url,
                                   int      n_hash_elts )
{
    hash_table_t       *hash_table;

    G_http_errno = HTTP_ERRNO_OK;

    c_assert( form && form_len > 0 );

    if( form_len < HTTP_WWW_FORM_MIN_LEN )
        return NULL;

    if( n_hash_elts )
    {
        c_assert( n_hash_elts > 0 );

        hash_table = hash_table_create( n_hash_elts );
    }
    else
        hash_table = hash_table_create( HTTP_HASH_TABLE_SIZE );

    if( is_url )
    {
        char   *q_mark;

        q_mark = memchr( form, '?', form_len );

        if( !q_mark ||
            q_mark >= &form[form_len - HTTP_WWW_FORM_MIN_LEN] )
        {
            hash_table_destroy( hash_table, free );
            return NULL;
        }

        form_len = form_len - (q_mark - form) - 1;
        form = q_mark + 1;
    }

    enum {
        STATE_KEY = 0,
        STATE_VAL,
    };

    int     state = STATE_KEY;
    bool    is_last = false;

    char   *key = NULL;
    int     key_len = 0;
    char   *val = NULL;
    int     val_len = 0;

    int     i;

    for( i = 0; i < form_len; i++ )
    {
        if( i == form_len - 1 )
            is_last = true;

        if( !IS_PRINTABLE_ASCII( form[i] ) )
        {
            hash_table_destroy( hash_table, free );
            return NULL;
        }

        switch( state )
        {
            case STATE_KEY:

                if( form[i] == '&' )
                {
                    hash_table_destroy( hash_table, free );
                    return NULL;
                }

                if( !key )
                {
                    if( form[i] == '=' || is_last )
                    {
                        hash_table_destroy( hash_table, free );
                        return NULL;
                    }

                    key = &form[i];
                }
                else
                if( form[i] == '=' )
                {
                    key_len = &form[i] - key;

                    c_assert( key_len );

                    state = STATE_VAL;
                }

                break;

            case STATE_VAL:

                if( form[i] == '=' )
                {
                    hash_table_destroy( hash_table, free );
                    return NULL;
                }

                if( !val )
                {
                    if( form[i] != '&' )
                        val = &form[i];
                }

                if( form[i] == '&' )
                {
                    if( val )
                    {
                        val_len = &form[i] - val;

                        c_assert( val_len );
                    }

                    if( !is_last )
                    {
                        if( __set_www_form_pair( hash_table,
                                                 key, key_len,
                                                 val, val_len ) )
                        {
                            hash_table_destroy( hash_table, free );
                            return NULL;
                        }

                        key = NULL;
                        key_len = 0;
                        val = NULL;
                        val_len = 0;

                        state = STATE_KEY;
                    }
                }

                break;

            default:

                c_assert( false );
                break;
        }

        if( is_last )
        {
            if( !key_len )
            {
                hash_table_destroy( hash_table, free );
                return NULL;
            }

            if( val && !val_len )
            {
                val_len = (int) (&form[i] - val) + 1;
            }

            if( __set_www_form_pair( hash_table,
                                     key, key_len,
                                     val, val_len ) )
            {
                hash_table_destroy( hash_table, free );
                return NULL;
            }
        }
    }

    return hash_table;
}

/******************* Init *****************************************************/

static void __init_percent_encoding_map()
{
    int     i;

    for( i = 0; i < 256; i++ )
    {
        if( i >= 32 && i <= 126 )
        {
            switch( i )
            {
                case '!':
                case '*':
                case '\'':
                case '(':
                case ')':
                case ';':
                case ':':
                case '@':
                case '&':
                case '=':
                case '+':
                case '$':
                case ',':
                case '/':
                case '?':
                case '#':
                case '[':
                case ']':
                case '"':
                case '%':
                case '-':
                case '.':
                case '<':
                case '>':
                case '\\':
                case '^':
                case '_':
                case '`':
                case '{':
                case '|':
                case '}':
                case '~':
                case ' ':

                    g_percent_encoding_map[i] = true;
                    break;

                default:

                    g_percent_encoding_map[i] = false;
                    break;
            }
        }
        else
            g_percent_encoding_map[i] = true;
    }
}

static void __default_config_init()
{
    if( !cfg_http_response_timeout )
    {
        cfg_http_response_timeout = malloc( sizeof(struct timeval) );

        cfg_http_response_timeout->tv_sec = 10;
        cfg_http_response_timeout->tv_usec = 0;
    }

    if( !cfg_http_check_messages_queue_interval )
    {
        cfg_http_check_messages_queue_interval = malloc(sizeof(struct timeval));

        cfg_http_check_messages_queue_interval->tv_sec = 5;
        cfg_http_check_messages_queue_interval->tv_usec = 0;
    }
}

void http_init()
{
    __default_config_init();

    __init_percent_encoding_map();
}

