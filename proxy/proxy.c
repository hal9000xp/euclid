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

/* The forward http proxy */

/* NOTE: this proxy was written for testing purposes, *
 * so assert is used here instead of c_assert         */

#include "main.h"
#include "linked_list.h"
#include "module.h"
#include "config.h"
#include "logger.h"
#include "network.h"
#include "http.h"
#include "proxy.h"
#include "proxy_internal.h"

int            *cfg_proxy_listen_port = NULL;
int            *cfg_proxy_listen_port_ssl = NULL;

static void __client_r_cb( http_id_t        http_id,
                           ptr_id_t         udata_id,
                           http_msg_t      *http_msg );

static void __client_est_cb( http_id_t      http_id,
                             ptr_id_t       udata_id );

static void __client_clo_cb( http_id_t      http_id,
                             ptr_id_t       udata_id,
                             int            code );

static int __raw_client_r_cb( conn_id_t     conn_id,
                              ptr_id_t      udata_id,
                              char         *buf,
                              int           len,
                              bool          is_closed );

static void __raw_client_est_cb( conn_id_t  conn_id,
                                 ptr_id_t   udata_id );

static void __raw_client_clo_cb( conn_id_t  conn_id,
                                 ptr_id_t   udata_id,
                                 int        code );

/******************* Mics functions *******************************************/

void __shutdown_child_conns( conn_in_t *conn_in )
{
    ll_t               *conn_out_list = &conn_in->conn_out_list;
    conn_out_t         *conn_out;
    conn_raw_t         *conn_raw;

    if( conn_out_list->total )
    {
        LL_CHECK( conn_out_list, conn_out_list->head );

        conn_out = PTRID_GET_PTR( conn_out_list->head );

        while( conn_out )
        {
            assert( conn_out->conn_in_id && conn_out->msg_id );

            conn_out->conn_in_id = 0;
            conn_out->msg_id = 0;

            http_shutdown( conn_out->http_id, false );

            LL_DEL_NODE( conn_out_list, conn_out->id );

            conn_out = PTRID_GET_PTR( conn_out->next );
        }
    }

    if( conn_in->conn_raw_id )
    {
        conn_raw = PTRID_GET_PTR( conn_in->conn_raw_id );

        assert( conn_raw->udata_id == conn_in->conn_raw_id );

        conn_raw->conn_in_id = 0;

        net_shutdown_conn( conn_raw->conn_id, false );
    }
}

static void __make_conn_out( conn_in_t         *conn_in,
                             net_host_t        *host,
                             ptr_id_t           msg_id,
                             http_msg_t        *http_msg )
{
    conn_out_t     *conn_out;

    conn_out = malloc( sizeof(conn_out_t) );
    memset( conn_out, 0, sizeof(conn_out_t) );

    conn_out->udata_id = PTRID( conn_out );
    conn_out->conn_in_id = conn_in->udata_id;
    conn_out->msg_id = msg_id;

    conn_out->origin_connection_close = http_msg->connection_close;

    conn_out->http_msg = http_dup_msg( http_msg, true );
    conn_out->http_msg->connection_close = true;

    conn_out->host = host;

    conn_out->http_id = http_make_conn( conn_out->host,
                                        __client_r_cb,
                                        __client_est_cb,
                                        __client_clo_cb,
                                        conn_out->udata_id );

    assert( conn_out->http_id );

    LL_ADD_NODE( &conn_in->conn_out_list, conn_out );

    LOG( "server_http_id:0x%llx server_udata_id:0x%llx "
         "msg_id:0x%llx "
         "client_http_id:0x%llx client_udata_id:0x%llx "
         "host:%.*s url:%.*s",
         PTRID_FMT( conn_in->http_id ),
         PTRID_FMT( conn_in->udata_id ),
         PTRID_FMT( msg_id ),
         PTRID_FMT( conn_out->http_id ),
         PTRID_FMT( conn_out->udata_id ),
         http_msg->host_len, http_msg->host,
         http_msg->url_len, http_msg->url );
}

static void __make_conn_raw( conn_in_t         *conn_in,
                             net_host_t        *host,
                             ptr_id_t           msg_id )
{
    conn_raw_t     *conn_raw;

    assert( !conn_in->conn_raw_id );

    conn_raw = malloc( sizeof(conn_raw_t) );
    memset( conn_raw, 0, sizeof(conn_raw_t) );

    conn_raw->udata_id = PTRID( conn_raw );

    conn_raw->conn_in_id = conn_in->udata_id;

    conn_raw->host = host;
    conn_raw->msg_id = msg_id;

    conn_raw->conn_id = net_make_conn( host,
                                       __raw_client_r_cb,
                                       __raw_client_est_cb,
                                       __raw_client_clo_cb,
                                       conn_raw->udata_id );

    assert( conn_raw->conn_id );

    conn_in->conn_raw_id = conn_raw->udata_id;

    LOG( "server_http_id:0x%llx server_udata_id:0x%llx "
         "msg_id:0x%llx "
         "client_conn_id:0x%llx client_udata_id:0x%llx "
         "host:%s:%s",
         PTRID_FMT( conn_in->http_id ),
         PTRID_FMT( conn_in->udata_id ),
         PTRID_FMT( msg_id ),
         PTRID_FMT( conn_raw->conn_id ),
         PTRID_FMT( conn_raw->udata_id ),
         host->hostname, host->port );
}

static void __tunneling_flush_upstream_data( conn_in_t *conn_in )
{
    conn_raw_t     *conn_raw;

    assert( conn_in->conn_raw_id );

    conn_raw = PTRID_GET_PTR( conn_in->conn_raw_id );

    assert( conn_raw->udata_id == conn_in->conn_raw_id &&
            !conn_raw->pending_data_sent );

    if( !conn_raw->pending_data_len )
        return;

    assert( conn_raw->pending_data );

    http_msg_t      http_msg = {0};
    int             r;

    http_msg.raw_body = conn_raw->pending_data;
    http_msg.raw_body_len = conn_raw->pending_data_len;

    r = http_post_raw_data( conn_in->http_id, &http_msg );
    assert( !r );

    conn_raw->pending_data_sent = true;

    LOG( "server_http_id:0x%llx server_udata_id:0x%llx "
         "msg_id:0x%llx "
         "client_conn_id:0x%llx client_udata_id:0x%llx "
         "host:%s:%s",
         PTRID_FMT( conn_in->http_id ),
         PTRID_FMT( conn_in->udata_id ),
         PTRID_FMT( conn_raw->msg_id ),
         PTRID_FMT( conn_raw->conn_id ),
         PTRID_FMT( conn_raw->udata_id ),
         conn_raw->host->hostname,
         conn_raw->host->port );
}

static void __tunneling_write_to_upstream( conn_in_t   *conn_in,
                                           http_msg_t  *http_msg )
{
    conn_raw_t     *conn_raw;
    int             r;

    assert( http_msg->raw_body && http_msg->raw_body_len );

    /* NOTE: conn was shutdown with flush_and_close */
    if( !conn_in->conn_raw_id )
    {
        LOGE( "server_http_id:0x%llx",
              PTRID_FMT( conn_in->http_id ) );
 
        return;
    }

    conn_raw = PTRID_GET_PTR( conn_in->conn_raw_id );

    assert( conn_raw->udata_id == conn_in->conn_raw_id );

    r = net_post_data( conn_raw->conn_id,
                       http_msg->raw_body,
                       http_msg->raw_body_len,
                       http_msg->connection_close );

    if( r )
    {
        LOGE( "server_http_id:0x%llx client_conn_id:0x%llx",
              PTRID_FMT( conn_in->http_id ),
              PTRID_FMT( conn_raw->conn_id ) );

        r = http_shutdown( conn_in->http_id, false );
        assert( !r );

        return;
    }

    LOG( "server_http_id:0x%llx server_udata_id:0x%llx "
         "client_conn_id:0x%llx client_udata_id:0x%llx "
         "connection_close:%d host:%s:%s",
         PTRID_FMT( conn_in->http_id ),
         PTRID_FMT( conn_in->udata_id ),
         PTRID_FMT( conn_raw->conn_id ),
         PTRID_FMT( conn_raw->udata_id ),
         http_msg->connection_close,
         conn_raw->host->hostname,
         conn_raw->host->port );
}

/******************* Callback functions ***************************************/

static void __client_r_cb( http_id_t        http_id,
                           ptr_id_t         udata_id,
                           http_msg_t      *http_msg )
{
    conn_out_t         *conn_out;
    conn_in_t          *conn_in;
    http_post_state_t   state = HTTP_POST_STATE_DEFAULT;
    int                 r;

    assert( http_id && udata_id );

    conn_out = PTRID_GET_PTR( udata_id );

    assert( conn_out->http_id == http_id &&
            conn_out->udata_id == udata_id &&
            conn_out->host && conn_out->msg_id &&
            conn_out->conn_in_id );

    conn_in = PTRID_GET_PTR( conn_out->conn_in_id );

    assert( conn_in->udata_id == conn_out->conn_in_id );

    LOG( "client_http_id:0x%llx client_udata_id:0x%llx msg_id:0x%llx "
         "server_http_id:0x%llx server_udata_id:0x%llx "
         "host:%.*s url:%.*s status:%d",
         PTRID_FMT( http_id ), PTRID_FMT( udata_id ),
         PTRID_FMT( conn_out->msg_id ),
         PTRID_FMT( conn_in->http_id ),
         PTRID_FMT( conn_in->udata_id ),
         conn_out->http_msg->host_len,
         conn_out->http_msg->host,
         conn_out->http_msg->url_len,
         conn_out->http_msg->url,
         http_msg->status_code );

    if( conn_in->tunneling_mode )
    {
        LOGE( "client_http_id:0x%llx msg_id:0x%llx",
              PTRID_FMT( http_id ),
              PTRID_FMT( conn_out->msg_id ) );

        return;
    }

    http_msg->connection_close =
                        conn_out->origin_connection_close;

    r = http_post_data( conn_in->http_id, http_msg,
                        conn_out->msg_id, &state );

    if( r )
    {
        LOGE( "server_http_id:0x%llx http_errno:%u net_errno:%u",
              PTRID_FMT( conn_in->http_id ),
              G_http_errno, G_net_errno );

        http_shutdown( conn_in->http_id, false );
        return;
    }

    conn_out->sent_reply = true;

    r = http_shutdown( conn_out->http_id, false );
    assert( !r );

    if( state == HTTP_POST_STATE_TUNNELING )
    {
        conn_in->tunneling_mode = true;

        __tunneling_flush_upstream_data( conn_in );
    }
}

static void __client_est_cb( http_id_t      http_id,
                             ptr_id_t       udata_id )
{
    conn_out_t     *conn_out;
    conn_in_t      *conn_in;
    int             r;

    assert( http_id && udata_id );

    conn_out = PTRID_GET_PTR( udata_id );

    assert( conn_out->http_id == http_id &&
            conn_out->udata_id == udata_id &&
            conn_out->conn_in_id &&
            conn_out->host &&
            conn_out->msg_id );

    conn_in = PTRID_GET_PTR( conn_out->conn_in_id );

    assert( conn_in->udata_id == conn_out->conn_in_id );

    LOG( "http_id:0x%llx udata_id:0x%llx msg_id:0x%llx", 
         PTRID_FMT( http_id ), PTRID_FMT( udata_id ),
         PTRID_FMT( conn_out->msg_id ) );

    r = http_post_data( conn_out->http_id,
                        conn_out->http_msg,
                        0, NULL );

    assert( !r );
}

static void __client_clo_cb( http_id_t      http_id,
                             ptr_id_t       udata_id,
                             int            code )
{
    conn_out_t     *conn_out;
    conn_in_t      *conn_in;

    assert( http_id && udata_id );

    conn_out = PTRID_GET_PTR( udata_id );

    assert( conn_out->http_id == http_id &&
            conn_out->udata_id == udata_id );

    LOG( "http_id:0x%llx udata_id:0x%llx code:%d", 
         PTRID_FMT( http_id ), PTRID_FMT( udata_id ), code );
 
    if( conn_out->conn_in_id )
    {
        conn_in = PTRID_GET_PTR( conn_out->conn_in_id );
        assert( conn_in->udata_id == conn_out->conn_in_id );

        if( !conn_out->sent_reply )
        {
            http_msg_t          http_msg = {0};
            http_post_state_t   state = HTTP_POST_STATE_DEFAULT;
            int                 r;

            http_msg.status_code = 502;
            http_msg.connection_close = true;

            r = http_post_data( conn_in->http_id,
                                &http_msg,
                                conn_out->msg_id,
                                &state );

            if( r )
            {
                LOGE( "server_http_id:0x%llx "
                      "http_errno:%u net_errno:%u",
                      PTRID_FMT( conn_in->http_id ),
                      G_http_errno, G_net_errno );

                http_shutdown( conn_in->http_id, false );
            }

            LOG( "http_id:0x%llx udata_id:0x%llx", 
                 PTRID_FMT( http_id ), PTRID_FMT( udata_id ) );
        }

        LL_DEL_NODE( &conn_in->conn_out_list, conn_out->id );
    }
 
    http_free_msg( conn_out->http_msg );
    net_free_host( conn_out->host );
    free( conn_out->host );

    memset( conn_out, 0, sizeof(conn_out_t) );
    free( conn_out );
}

static int __raw_client_r_cb( conn_id_t     conn_id,
                              ptr_id_t      udata_id,
                              char         *buf,
                              int           len,
                              bool          is_closed )
{
    conn_raw_t     *conn_raw;
    conn_in_t      *conn_in;
    int             r;

    assert( conn_id && udata_id &&
            buf && (len || is_closed) );

    conn_raw = PTRID_GET_PTR( udata_id );

    assert( conn_raw->conn_id == conn_id &&
            conn_raw->udata_id == udata_id &&
            conn_raw->conn_in_id );

    conn_in = PTRID_GET_PTR( conn_raw->conn_in_id );
    assert( conn_in->udata_id == conn_raw->conn_in_id );

    if( !len )
        return 0;

    if( !conn_in->tunneling_mode )
    {
        assert( !conn_raw->pending_data_sent );

        conn_raw->pending_data = buf;
        conn_raw->pending_data_len = len;
        return 0;
    }

    http_msg_t      http_msg = {0};
    char           *out_buf;
    int             out_len;

    if( conn_raw->pending_data_sent )
    {
        assert( conn_raw->pending_data &&
                conn_raw->pending_data_len &&
                conn_raw->pending_data_len <= len );

        out_buf = buf + conn_raw->pending_data_len;
        out_len = len - conn_raw->pending_data_len;
    }
    else
    {
        out_buf = buf;
        out_len = len;
    }

    if( conn_raw->pending_data_len )
    {
        conn_raw->pending_data_sent = false;
        conn_raw->pending_data = NULL;
        conn_raw->pending_data_len = 0;
    }

    http_msg.raw_body = out_buf;
    http_msg.raw_body_len = out_len;

    r = http_post_raw_data( conn_in->http_id, &http_msg );

    if( r )
    {
        LOGE( "server_http_id:0x%llx http_errno:%u net_errno:%u",
              PTRID_FMT( conn_in->http_id ),
              G_http_errno, G_net_errno );

        http_shutdown( conn_in->http_id, false );
        return 0;
    }

    LOG( "server_http_id:0x%llx server_udata_id:0x%llx "
         "client_conn_id:0x%llx client_udata_id:0x%llx "
         "is_closed:%d host:%s:%s",
         PTRID_FMT( conn_in->http_id ),
         PTRID_FMT( conn_in->udata_id ),
         PTRID_FMT( conn_raw->conn_id ),
         PTRID_FMT( conn_raw->udata_id ),
         is_closed,
         conn_raw->host->hostname,
         conn_raw->host->port );

    return len;
}

static void __raw_client_est_cb( conn_id_t  conn_id,
                                 ptr_id_t   udata_id )
{
    conn_raw_t         *conn_raw;
    conn_in_t          *conn_in;
    http_msg_t          http_msg = {0};
    http_post_state_t   state = HTTP_POST_STATE_DEFAULT;
    int                 r;

    assert( conn_id && udata_id );

    conn_raw = PTRID_GET_PTR( udata_id );

    assert( conn_raw->conn_id == conn_id &&
            conn_raw->udata_id == udata_id &&
            conn_raw->conn_in_id );

    conn_in = PTRID_GET_PTR( conn_raw->conn_in_id );
    assert( conn_in->udata_id == conn_raw->conn_in_id );

    http_msg.status_code = 200;

    r = http_post_data( conn_in->http_id, &http_msg,
                        conn_raw->msg_id, &state );

    conn_raw->est_reply_sent = true;

    if( r )
    {
        LOGE( "server_http_id:0x%llx http_errno:%u net_errno:%u",
              PTRID_FMT( conn_in->http_id ),
              G_http_errno, G_net_errno );

        http_shutdown( conn_in->http_id, false );
    }

    conn_in->tunneling_mode = ( state == HTTP_POST_STATE_TUNNELING );

    LOG( "server_http_id:0x%llx server_udata_id:0x%llx "
         "client_conn_id:0x%llx client_udata_id:0x%llx "
         "host:%s:%s",
         PTRID_FMT( conn_in->http_id ),
         PTRID_FMT( conn_in->udata_id ),
         PTRID_FMT( conn_raw->conn_id ),
         PTRID_FMT( conn_raw->udata_id ),
         conn_raw->host->hostname,
         conn_raw->host->port );
}

static void __raw_client_clo_cb( conn_id_t  conn_id,
                                 ptr_id_t   udata_id,
                                 int        code )
{
    conn_raw_t     *conn_raw;
    conn_in_t      *conn_in;

    assert( conn_id && udata_id );

    conn_raw = PTRID_GET_PTR( udata_id );

    assert( conn_raw->conn_id == conn_id &&
            conn_raw->udata_id == udata_id );

    LOG( "conn_id:0x%llx udata_id:0x%llx code:%d", 
         PTRID_FMT( conn_id ), PTRID_FMT( udata_id ), code );
 
    if( conn_raw->conn_in_id )
    {
        conn_in = PTRID_GET_PTR( conn_raw->conn_in_id );
        assert( conn_in->udata_id == conn_raw->conn_in_id );

        conn_in->conn_raw_id = 0;

        if( !conn_raw->est_reply_sent )
        {
            http_msg_t          http_msg = {0};
            http_post_state_t   state = HTTP_POST_STATE_DEFAULT;
            int                 r;

            assert( !conn_in->tunneling_mode );

            http_msg.status_code = 502;
            http_msg.connection_close = true;

            r = http_post_data( conn_in->http_id,
                                &http_msg,
                                conn_raw->msg_id,
                                &state );

            if( r )
            {
                LOGE( "server_http_id:0x%llx "
                      "http_errno:%u net_errno:%u",
                      PTRID_FMT( conn_in->http_id ),
                      G_http_errno, G_net_errno );

                http_shutdown( conn_in->http_id, false );
            }

            LOG( "conn_id:0x%llx udata_id:0x%llx", 
                 PTRID_FMT( conn_id ), PTRID_FMT( udata_id ) );
        }
        else
            http_shutdown( conn_in->http_id, true );
    }

    net_free_host( conn_raw->host );
    free( conn_raw->host );

    memset( conn_raw, 0, sizeof(conn_raw_t) );
    free( conn_raw );
}

static void __server_r_cb( http_id_t        http_id,
                           ptr_id_t         udata_id,
                           ptr_id_t         msg_id,
                           http_msg_t      *http_msg )
{
    conn_in_t      *conn_in;
    net_host_t     *host;
    int             r;

    assert( http_id && udata_id );

    conn_in = PTRID_GET_PTR( udata_id );

    assert( conn_in->http_id == http_id &&
            conn_in->udata_id == udata_id );

    if( !msg_id && !conn_in->tunneling_mode )
    {
        LOGE( "server_http_id:0x%llx server_udata_id:0x%llx",
              PTRID_FMT( http_id ), PTRID_FMT( udata_id ) );

        return;
    }

    if( conn_in->tunneling_mode )
    {
        __tunneling_write_to_upstream( conn_in, http_msg );
        return;
    }

    host = http_msg_get_host( http_msg, conn_in->is_ssl );

    if( !host || !host->addr )
    {
        LOGE( "server_http_id:0x%llx server_udata_id:0x%llx "
              "msg_id:0x%llx", PTRID_FMT( http_id ),
              PTRID_FMT( udata_id ), PTRID_FMT( msg_id ) );

        if( host )
            net_free_host( host );

        r = http_shutdown( http_id, false );
        assert( !r );

        return;
    }

    if( http_msg->is_connect_method )
        __make_conn_raw( conn_in, host, msg_id );
    else
        __make_conn_out( conn_in, host, msg_id, http_msg );
}

static void __server_est_cb( http_id_t      http_id,
                             ptr_id_t       udata_id )
{
    conn_in_t      *conn_in;

    assert( http_id && udata_id );

    conn_in = PTRID_GET_PTR( udata_id );

    assert( conn_in->http_id == http_id &&
            conn_in->udata_id == udata_id );

    LOG( "http_id:0x%llx udata_id:0x%llx", 
         PTRID_FMT( http_id ), PTRID_FMT( udata_id ) );
 
    /* doing nothing more */
}

static void __server_clo_cb( http_id_t      http_id,
                             ptr_id_t       udata_id,
                             int            code )
{
    conn_in_t  *conn_in;

    assert( http_id && udata_id );

    conn_in = PTRID_GET_PTR( udata_id );

    assert( conn_in->http_id == http_id &&
            conn_in->udata_id == udata_id );

    LOG( "http_id:0x%llx udata_id:0x%llx code:%d",
         PTRID_FMT( http_id ), PTRID_FMT( udata_id ), code );

    __shutdown_child_conns( conn_in );

    memset( conn_in, 0, sizeof(conn_in_t) );
    free( conn_in );
}

static ptr_id_t __dup_udata_cb( http_id_t   http_id,
                                ptr_id_t    udata_id )
{
    listen_t   *listen;
    conn_in_t  *conn_in;

    assert( http_id && udata_id );

    listen = PTRID_GET_PTR( udata_id );

    assert( listen->http_id != http_id &&
            listen->udata_id == udata_id );

    conn_in = malloc( sizeof(conn_in_t) );
    memset( conn_in, 0, sizeof(conn_in_t) );

    conn_in->http_id = http_id;
    conn_in->udata_id = PTRID( conn_in );

    conn_in->port = listen->port;
    conn_in->is_ssl = listen->is_ssl;

    LOG( "http_id:0x%llx udata_id:0x%llx",
         PTRID_FMT( http_id ), PTRID_FMT( udata_id ) );

    return conn_in->udata_id;
}

static void __listen_clo_cb( http_id_t      http_id,
                             ptr_id_t       udata_id,
                             int            code )
{
    listen_t   *listen;

    assert( http_id && udata_id );

    listen = PTRID_GET_PTR( udata_id );

    assert( listen->http_id == http_id &&
            listen->udata_id == udata_id );

    LOG( "http_id:0x%llx udata_id:0x%llx code:%d",
         PTRID_FMT( http_id ), PTRID_FMT( udata_id ), code );

    memset( listen, 0, sizeof(listen_t) );
    free( listen );
}

/******************* Init *****************************************************/

static void __default_config_init()
{
    if( !cfg_proxy_listen_port )
    {
        cfg_proxy_listen_port = malloc( sizeof(int) );

        *cfg_proxy_listen_port = 1111;
    }

    if( !cfg_proxy_listen_port_ssl )
    {
        cfg_proxy_listen_port_ssl = malloc( sizeof(int) );

        *cfg_proxy_listen_port_ssl = 2222;
    }
}

void proxy_cfg_init()
{
    config_add_file( "proxy/proxy.cfg" );

    config_add_cmd( "proxy_listen_port",
                    CONFIG_CMD_TYPE_INTEGER,
                    (void **) &cfg_proxy_listen_port );

    config_add_cmd( "proxy_listen_port_ssl",
                    CONFIG_CMD_TYPE_INTEGER,
                    (void **) &cfg_proxy_listen_port_ssl );
}

void proxy_init()
{
    listen_t   *listen;
    listen_t   *listen_ssl;

    __default_config_init();

    listen = malloc( sizeof(listen_t) );
    memset( listen, 0, sizeof(listen_t) );

    listen->port = *cfg_proxy_listen_port;
    listen->is_ssl = false;

    listen->udata_id = PTRID( listen );

    listen->http_id = http_make_listen( __server_r_cb,
                                        __server_est_cb,
                                        __server_clo_cb,
                                        __dup_udata_cb,
                                        __listen_clo_cb,
                                        listen->udata_id,
                                        listen->port, 
                                        listen->is_ssl );

    assert( listen->http_id );

    listen_ssl = malloc( sizeof(listen_t) );
    memset( listen_ssl, 0, sizeof(listen_t) );

    listen_ssl->port = *cfg_proxy_listen_port_ssl;
    listen_ssl->is_ssl = true;

    listen_ssl->udata_id = PTRID( listen_ssl );

    listen_ssl->http_id = http_make_listen( __server_r_cb,
                                            __server_est_cb,
                                            __server_clo_cb,
                                            __dup_udata_cb,
                                            __listen_clo_cb,
                                            listen_ssl->udata_id,
                                            listen_ssl->port, 
                                            listen_ssl->is_ssl );

    assert( listen_ssl->http_id );
}

