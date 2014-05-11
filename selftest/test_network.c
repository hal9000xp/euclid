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

/* NOTE: this code should be stupid as it can be */
/* NOTE: assert is used here instead of c_assert */
/* TODO: need real time stats, may be output stats through accepted conn? */

#include "main.h"
#include "linked_list.h"
#include "module.h"
#include "config.h"
#include "logger.h"
#include "network.h"
#include "http.h"
#include "test_network.h"
#include "test_network_internal.h"

struct timeval *cfg_net_test_update_hosts_interval = NULL;
ll_t           *cfg_net_test_hosts = NULL;
int            *cfg_net_test_max_calls = NULL;
int            *cfg_net_test_total_conns = NULL;
int            *cfg_net_test_total_tmrs = NULL;
int            *cfg_net_test_listen_port = NULL;
int            *cfg_net_test_listen_port_ssl = NULL;

/* TODO: move to config! */
#define CHK_DATA_ARRAY_SIZE   sizeof(g_chk_array)/sizeof(crc_check_t)
static crc_check_t      g_chk_array[] =
{
    { .crc32 = 0x9988f955, .len = 1277952   },
    { .crc32 = 0x70410245, .len = 3538944   },
    { .crc32 = 0x5c2a5a7a, .len = 5271      },
    { .crc32 = 0x7b82f1f1, .len = 187       },
    { .crc32 = 0x83fb9098, .len = 26699     },
    { .crc32 = 0xeeaf5291, .len = 295853    },
    { .crc32 = 0xffc61aa6, .len = 13860864  },
    { .crc32 = 0xc1088725, .len = 1146880   },
    { .crc32 = 0x4ce522a8, .len = 786432    },
    { .crc32 = 0xae1d3503, .len = 127       }
};

static conn_elt_t      *g_conn_array;
static int              g_total_opened_conns = 0;
static bool             g_is_listening = false;

static int              g_max_checked_responses = 0;
static int              g_max_requests_queue_size = 0;

static tmr_elt_t       *g_tmr_array;
static int              g_total_conn_tmrs = 0;
static int              g_total_global_tmrs = 0;

static void             __random_calls();

/******************* Misc util functions **************************************/

static net_host_t *__get_rand_host()
{
    net_host_node_t        *node;
    net_host_node_t        *node_next;
    net_host_t             *host = NULL;
    int                     rnd;
    int                     i = 0;

    LL_CHECK( cfg_net_test_hosts, cfg_net_test_hosts->head );
    node = PTRID_GET_PTR( cfg_net_test_hosts->head );

    rnd = rand() % cfg_net_test_hosts->total;

    while( node )
    {
        LL_CHECK( cfg_net_test_hosts, node->id );
        node_next = PTRID_GET_PTR( node->next );

        assert( node->host.hostname &&
                node->host.port &&
                i <= cfg_net_test_hosts->total );

        if( i == rnd )
            host = &node->host;

        i++;
        node = node_next;
    }

    assert( host && i == cfg_net_test_hosts->total );

    return host;
}

static conn_elt_t *__get_conn_free_elt()
{
    conn_elt_t         *conn_elt = NULL;
    int                 i;

    for( i = 0; i < *cfg_net_test_total_conns; i++ )
    {
        assert( !g_conn_array[i].http_id || g_conn_array[i].udata_id );

        if( !conn_elt && !g_conn_array[i].http_id )
        {
            conn_elt = &g_conn_array[i];
        }
    }

    g_total_opened_conns++;

    assert( conn_elt && g_total_opened_conns <= *cfg_net_test_total_conns );

    return conn_elt;
}

static conn_elt_t *__get_rand_conn_elt( bool want_est_conn )
{
    conn_elt_t         *conn_elt = NULL;
    int                 opened_conns = 0;
    http_state_t        is_est;
    int                 init;
    int                 i;

    init = i = rand() % *cfg_net_test_total_conns;

    do
    {
        if( g_conn_array[i].http_id )
        {
            assert( g_conn_array[i].udata_id );

            opened_conns++;

            is_est = http_is_est( g_conn_array[i].http_id );

            if( !conn_elt &&
                ( !want_est_conn || is_est == HTTP_STATE_EST ) )
            {
                conn_elt = &g_conn_array[i];
            }
        }
        else
        {
            assert( !g_conn_array[i].udata_id );
        }

        i++;

        if( i == *cfg_net_test_total_conns )
            i = 0;
    }
    while( i != init );

    assert( opened_conns && opened_conns == g_total_opened_conns &&
            opened_conns <= *cfg_net_test_total_conns );

    return conn_elt;
}

static tmr_elt_t *__get_rand_tmr_id( http_id_t *http_id )
{
    tmr_elt_t          *tmr_elt = NULL;
    int                 conn_tmrs = 0;
    int                 global_tmrs = 0;
    int                 init;
    int                 i;

    assert( (http_id && g_total_conn_tmrs > 0)    ||
            (!http_id && g_total_global_tmrs > 0) );

    init = i = rand() % *cfg_net_test_total_tmrs;

    do
    {
        assert( !g_tmr_array[i].http_id || g_tmr_array[i].tmr_id );

        if( g_tmr_array[i].tmr_id && g_tmr_array[i].http_id )
            conn_tmrs++;
        else
        if( g_tmr_array[i].tmr_id && !g_tmr_array[i].http_id )
            global_tmrs++;

        if( g_tmr_array[i].tmr_id && !tmr_elt &&
            !!g_tmr_array[i].http_id == !!http_id )
        {
            tmr_elt = &g_tmr_array[i];
        }

        i++;

        if( i == *cfg_net_test_total_tmrs )
            i = 0;
    }
    while( i != init );

    assert( tmr_elt && conn_tmrs == g_total_conn_tmrs &&
            global_tmrs == g_total_global_tmrs );

    if( http_id )
        *http_id = tmr_elt->http_id;

    return tmr_elt;
}

static void __make_rand_timeout( struct timeval *timeout )
{
    if( rand() % 50 == 0 )
        timeout->tv_sec = 0;
    else
        timeout->tv_sec = rand() % 5;

    if( rand() % 3 == 0 )
        timeout->tv_usec = 0;
    else
        timeout->tv_usec = rand() % 1000000;
}

/* NOTE: __save_tmr_id is the same for http and net_ functions, *
 * so use generic ptr_id_t instead of http_tmr_id_t             */

static void __save_tmr_id( conn_elt_t       *conn_elt,
                           ptr_id_t          tmr_id )
{
    bool                is_saved = false;
    int                 i;

    assert( tmr_id );

    for( i = 0; i < *cfg_net_test_total_tmrs; i++ )
    {
        assert( !g_tmr_array[i].http_id || g_tmr_array[i].tmr_id );
        assert( g_tmr_array[i].tmr_id != tmr_id );

        if( !g_tmr_array[i].tmr_id && !is_saved )
        {
            if( conn_elt )
                g_tmr_array[i].http_id = conn_elt->http_id;

            g_tmr_array[i].tmr_id = tmr_id;

            is_saved = true;
        }
    }

    if( conn_elt )
        g_total_conn_tmrs++;
    else
        g_total_global_tmrs++;

    assert( is_saved &&
            g_total_conn_tmrs + g_total_global_tmrs <=
                    *cfg_net_test_total_tmrs );
}

static void __cleanup_requests_queue( conn_elt_t   *conn_elt )
{
    requests_queue_elt_t       *req;
    requests_queue_elt_t       *req_next;

    if( conn_elt->requests_queue.total )
    {
        LL_CHECK( &conn_elt->requests_queue,
                  conn_elt->requests_queue.head );

        req = PTRID_GET_PTR( conn_elt->requests_queue.head );

        while( req )
        {
            req_next = PTRID_GET_PTR( req->next );

            LL_DEL_NODE( &conn_elt->requests_queue, req->id );

            assert( req->http_id == conn_elt->http_id );

            memset( req, 0, sizeof(requests_queue_elt_t) );
            free( req );

            req = req_next;
        }
    }

    assert( !conn_elt->requests_queue.total &&
            !conn_elt->requests_queue.head  &&
            !conn_elt->requests_queue.tail );
}

static void __cleanup_conn_data( http_id_t http_id )
{
    bool                is_conn_deleted = false;
    int                 i;

    assert( http_id && g_total_opened_conns >= 0 &&
            g_total_opened_conns <= *cfg_net_test_total_conns );

    for( i = 0; i < *cfg_net_test_total_conns; i++ )
    {
        assert( !g_conn_array[i].http_id || g_conn_array[i].udata_id );

        if( g_conn_array[i].http_id == http_id )
        {
            g_total_opened_conns--;

            assert( !is_conn_deleted && g_total_opened_conns >= 0 );

            __cleanup_requests_queue( &g_conn_array[i] );

            /* NOTE: handling in close callbacks *
             * must be changed if use free()     */

            memset( &g_conn_array[i], 0, sizeof(conn_elt_t) );

            is_conn_deleted = true;
        }
    }

    assert( is_conn_deleted );

    for( i = 0; i < *cfg_net_test_total_tmrs; i++ )
    {
        assert( !g_tmr_array[i].http_id || g_tmr_array[i].tmr_id );

        if( g_tmr_array[i].tmr_id &&
            g_tmr_array[i].http_id == http_id )
        {
            g_tmr_array[i].http_id = 0;
            g_tmr_array[i].tmr_id = 0;

            g_total_conn_tmrs--;
            assert( g_total_conn_tmrs >= 0 );
        }
    }
}

static void __crc_check( crc_check_t   *check,
                         char          *buf,
                         int            len )
{
    unsigned int            crc32;

    crc32 = xcrc32( (unsigned char *) buf, len, CRC_INIT );

    assert( check->crc32 == crc32 && check->len == len );
}

static void __requests_queue_push( conn_elt_t      *conn_elt,
                                   crc_check_t     *check )
{
    requests_queue_elt_t   *req;

    req = malloc( sizeof(requests_queue_elt_t) );
    memset( req, 0, sizeof(requests_queue_elt_t) );

    req->http_id = conn_elt->http_id;

    req->check = check;

    LL_ADD_NODE( &conn_elt->requests_queue, req );

    if( conn_elt->requests_queue.total > g_max_requests_queue_size )
        g_max_requests_queue_size = conn_elt->requests_queue.total;
}

static void __requests_queue_pop( conn_elt_t   *conn_elt,
                                  char         *buf,
                                  int           len )
{
    requests_queue_elt_t   *req;

    assert( conn_elt && conn_elt->requests_queue.total &&
            buf && len );

    req = PTRID_GET_PTR( conn_elt->requests_queue.head );

    assert( req->http_id == conn_elt->http_id );

    LL_DEL_NODE( &conn_elt->requests_queue,
                 conn_elt->requests_queue.head );

    if( conn_elt->to_itself )
    {
        char    str[64];
        int     num;

        snprintf( str, sizeof(str), "%.*s", len, buf );

        num = atoi( str );

        assert( num == conn_elt->outcoming_expected_num );

        conn_elt->outcoming_expected_num++;

        LOG( "http_id:0x%llx is_ssl:%d responses:%d num:%d",
             PTRID_FMT( conn_elt->http_id ),
             conn_elt->is_ssl,
             conn_elt->checked_responses,
             conn_elt->outcoming_expected_num );
    }
    else
    {
        __crc_check( req->check, buf, len );

        LOG( "http_id:0x%llx is_ssl:%d responses:%d len:%d",
             PTRID_FMT( conn_elt->http_id ),
             conn_elt->is_ssl,
             conn_elt->checked_responses, len );
    }

    memset( req, 0, sizeof(requests_queue_elt_t) );
    free( req );

    conn_elt->checked_responses++;

    if( conn_elt->checked_responses > g_max_checked_responses )
        g_max_checked_responses = conn_elt->checked_responses;
}

static void __init_http_msg( http_msg_t *http_msg )
{
    memset( http_msg, 0, sizeof(http_msg_t) );

    http_msg->host = DEFAULT_HOST;
    http_msg->host_len = DEFAULT_HOST_LEN;

    http_msg->user_agent = HTTP_USER_AGENT;
    http_msg->user_agent_len = HTTP_USER_AGENT_LEN;
}

static void __post_reply( conn_elt_t           *conn_elt,
                          incoming_msg_t       *msg,
                          http_post_state_t    *state )
{
    http_msg_t      http_msg;
    char            buf[64];
    int             r;

    snprintf( buf, sizeof(buf), "%u\n", msg->n );

    __init_http_msg( &http_msg );

    http_msg.status_code = 200;
    http_msg.raw_body = buf;
    http_msg.raw_body_len = strlen( buf );

    if( msg->connection_close )
        http_msg.connection_close = msg->connection_close;
    else
        http_msg.connection_close = rand() % 30 ? false : true;

    r = http_post_data( conn_elt->http_id, &http_msg,
                        msg->msg_id, state );

    assert( !r && *state != HTTP_POST_STATE_TUNNELING );

    memset( msg, 0, sizeof(incoming_msg_t) );
}

static bool __handle_incoming_msgs( conn_elt_t     *conn_elt,
                                    ptr_id_t        msg_id,
                                    bool            connection_close )
{
    bool                is_saved = false;
    http_post_state_t   state = HTTP_POST_STATE_DEFAULT;
    int                 i;

    assert( msg_id );

    for( i = 0; i < TOTAL_INCOMING_MSGS; i++ )
    {
        if( !conn_elt->incoming_msgs[i].msg_id )
        {
            conn_elt->incoming_msgs[i].msg_id = msg_id;
            conn_elt->incoming_msgs[i].n = conn_elt->incoming_req_num;
            conn_elt->incoming_msgs[i].connection_close = connection_close;

            is_saved = true;
            break;
        }
    }

    if( !is_saved )
    {
        incoming_msg_t  msg = { .msg_id = msg_id,
                                .n = conn_elt->incoming_req_num,
                                .connection_close = connection_close };

        __post_reply( conn_elt, &msg, &state );
    }

    conn_elt->incoming_req_num++;

    if( state == HTTP_POST_STATE_DEFAULT )
    {
        for( i = 0; i < TOTAL_INCOMING_MSGS; i++ )
        {
            if( conn_elt->incoming_msgs[i].msg_id &&
                rand() % (TOTAL_INCOMING_MSGS / 2) == 0 )
            {
                __post_reply( conn_elt,
                              &conn_elt->incoming_msgs[i],
                              &state );

                if( state == HTTP_POST_STATE_SENT_CLOSE )
                    break;
            }
        }
    }

    return state == HTTP_POST_STATE_SENT_CLOSE;
}

/******************* Callback functions ***************************************/

static void __rand_r_cb( http_id_t      http_id,
                         ptr_id_t       udata_id,
                         http_msg_t    *http_msg )
{
    conn_elt_t     *conn_elt;
    char           *body;
    int             body_len;

    assert( http_id && udata_id &&
            http_msg->raw_body && http_msg->raw_body_len > 0 &&
            http_msg->status_code >= HTTP_STATUS_CODE_MIN &&
            http_msg->status_code <= HTTP_STATUS_CODE_MAX );

    assert( (http_msg->body && http_msg->body_len) ||
            (!http_msg->body && !http_msg->body_len ) );

    body = http_msg->body ? http_msg->body :
                            http_msg->raw_body;

    body_len = http_msg->body_len ? http_msg->body_len :
                                    http_msg->raw_body_len;

    conn_elt = PTRID_GET_PTR( udata_id );

    assert( conn_elt->http_id == http_id &&
            conn_elt->udata_id == udata_id &&
            conn_elt->type == CONN_CONNECT &&
            http_is_est( conn_elt->http_id ) == HTTP_STATE_EST );

    LOGD( "http_id:0x%llx udata_id:0x%llx body_len:%d "
          "msg_queue_size:%d checked_responses:%d "
          "opened_conns:%d conn_tmrs:%d global_tmrs:%d",
          PTRID_FMT( http_id ), PTRID_FMT( udata_id ),
          body_len, conn_elt->requests_queue.total,
          conn_elt->checked_responses, g_total_opened_conns,
          g_total_conn_tmrs, g_total_global_tmrs );

    __requests_queue_pop( conn_elt, body, body_len );

    if( http_msg->connection_close )
        __cleanup_conn_data( http_id );

    __random_calls();
}

static void __rand_est_cb( http_id_t     http_id,
                           ptr_id_t      udata_id )
{
    conn_elt_t     *conn_elt;

    LOGD( "http_id:0x%llx udata_id:0x%llx opened_conns:%d "
          "conn_tmrs:%d global_tmrs:%d",
          PTRID_FMT( http_id ), PTRID_FMT( udata_id ),
          g_total_opened_conns, g_total_conn_tmrs, g_total_global_tmrs );

    conn_elt = PTRID_GET_PTR( udata_id );

    assert( conn_elt->http_id == http_id &&
            conn_elt->udata_id == udata_id &&
            conn_elt->type == CONN_CONNECT &&
            http_is_est( conn_elt->http_id ) == HTTP_STATE_EST );

    __random_calls();
}

static void __rand_clo_cb( http_id_t     http_id,
                           ptr_id_t      udata_id,
                           int           code )
{
    conn_elt_t     *conn_elt;

    LOGD( "http_id:0x%llx udata_id:0x%llx code:%d opened_conns:%d "
          "conn_tmrs:%d global_tmrs:%d",
          PTRID_FMT( http_id ), PTRID_FMT( udata_id ),
          code, g_total_opened_conns,
          g_total_conn_tmrs, g_total_global_tmrs );

    conn_elt = PTRID_GET_PTR( udata_id );

    /* NOTE: this is possible because it is never freed */
    if( conn_elt->http_id == http_id )
    {
        assert( conn_elt->udata_id == udata_id &&
                conn_elt->type == CONN_CONNECT &&
                http_is_est( conn_elt->http_id ) != HTTP_STATE_EST );

        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );

        __cleanup_conn_data( http_id );
    }
    else
    {
        assert( conn_elt->udata_id != udata_id );

        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );
    }

    __random_calls();
}

/* NOTE: __rand_tmr_cb is the same for http and net_ callbacks,   *
 * so use generic ptr_id_t instead of http_id_t and http_tmr_id_t */

static void __rand_tmr_cb( ptr_id_t         http_id,
                           ptr_id_t         conn_udata_id,
                           ptr_id_t         tmr_id,
                           ptr_id_t         tmr_udata_id )
{
    conn_elt_t     *conn_elt;

    LOGD( "http_id:0x%llx conn_udata_id:0x%llx "
          "tmr:0x%llx tmr_udata_id:0x%llx "
          "opened_conns:%d conn_tmrs:%d global_tmrs:%d",
          PTRID_FMT( http_id ), PTRID_FMT( conn_udata_id ),
          PTRID_FMT( tmr_id ), PTRID_FMT( tmr_udata_id ),
          g_total_opened_conns, g_total_conn_tmrs, g_total_global_tmrs );

    assert( tmr_udata_id == UDATA_LABEL );

    if( http_id )
    {
        conn_elt = PTRID_GET_PTR( conn_udata_id );

        assert( conn_elt->http_id == http_id &&
                conn_elt->udata_id == conn_udata_id &&
                conn_elt->type == CONN_CONNECT );
    }
    else
    {
        assert( !conn_udata_id );
    }

    __random_calls();
}

static ptr_id_t __dup_udata_cb( http_id_t   http_id,
                                ptr_id_t    udata_id )
{
    conn_elt_t         *l_conn_elt;
    conn_elt_t         *conn_elt;

    assert( http_id && udata_id );

    l_conn_elt = PTRID_GET_PTR( udata_id );

    assert( l_conn_elt->http_id != http_id &&
            l_conn_elt->udata_id == udata_id &&
            l_conn_elt->type == CONN_LISTEN );

    l_conn_elt->keep_alive = rand() % 10 ? true : false;

    if( g_total_opened_conns == *cfg_net_test_total_conns )
    {
        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );
        return 0;
    }

    assert( g_total_opened_conns < *cfg_net_test_total_conns );

    conn_elt = __get_conn_free_elt();

    conn_elt->http_id = http_id;
    conn_elt->udata_id = PTRID( conn_elt );

    conn_elt->type = CONN_ACCEPTED;
    conn_elt->keep_alive = rand() % 10 ? true : false;

    LOG( "l_udata_id:0x%llx udata_id:0x%llx",
         PTRID_FMT( udata_id ),
         PTRID_FMT( conn_elt->udata_id ) );

    return conn_elt->udata_id;
}

static void __accepted_r_cb( http_id_t      http_id,
                             ptr_id_t       udata_id,
                             ptr_id_t       msg_id,
                             http_msg_t    *http_msg )
{
    conn_elt_t     *conn_elt = NULL;

    assert( http_id && udata_id &&
            http_msg->url && http_msg->url_len > 0 );

    LOGD( "http_id:0x%llx udata_id:0x%llx "
          "url_len:%d body_len:%d connection_close:%d",
          PTRID_FMT( http_id ), PTRID_FMT( udata_id ),
          http_msg->url_len, http_msg->raw_body_len,
          http_msg->connection_close );

    if( http_is_est( http_id ) == HTTP_STATE_SENT_CLOSE )
    {
        assert( !msg_id );

        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );
        return;
    }

    conn_elt = PTRID_GET_PTR( udata_id );

    assert( conn_elt->http_id == http_id &&
            conn_elt->udata_id == udata_id &&
            conn_elt->type == CONN_ACCEPTED &&
            http_is_est( conn_elt->http_id ) == HTTP_STATE_EST );

    conn_elt->keep_alive = rand() % 10 ? true : false;

    if( __handle_incoming_msgs( conn_elt, msg_id,
                                http_msg->connection_close ) )
    {
        __cleanup_conn_data( http_id );
    }

    __random_calls();
}

static void __accepted_est_cb( http_id_t     http_id,
                               ptr_id_t      udata_id )
{
    conn_elt_t     *conn_elt;
    int             r;

    assert( http_id );

    if( !udata_id )
    {
        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );

        assert( http_is_est( http_id ) == HTTP_STATE_EST );

        r = http_shutdown( http_id, false );
        assert( !r );
        return;
    }

    LOGD( "http_id:0x%llx udata_id:0x%llx",
          PTRID_FMT( http_id ), PTRID_FMT( udata_id ) );

    conn_elt = PTRID_GET_PTR( udata_id );

    assert( conn_elt->http_id == http_id &&
            conn_elt->udata_id == udata_id &&
            conn_elt->type == CONN_ACCEPTED &&
            http_is_est( conn_elt->http_id ) == HTTP_STATE_EST );

    __random_calls();
}

static void __accepted_clo_cb( http_id_t     http_id,
                               ptr_id_t      udata_id,
                               int           code )
{
    conn_elt_t     *conn_elt = NULL;

    assert( http_id );

    if( !udata_id )
    {
        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );

        /* NOTE: don't need to check return value   *
         * because it may call at __accepted_est_cb */
        http_shutdown( http_id, false );

        assert( (G_http_errno == HTTP_ERRNO_OK &&
                 G_net_errno == NET_ERRNO_OK) ||
                (G_http_errno == HTTP_ERRNO_GENERAL_ERR &&
                 G_net_errno == NET_ERRNO_CONN_WRONG_STATE) );

        return;
    }

    LOGD( "http_id:0x%llx udata_id:0x%llx code:%d",
          PTRID_FMT( http_id ), PTRID_FMT( udata_id ), code );

    conn_elt = PTRID_GET_PTR( udata_id );

    /* NOTE: this is possible because it is never freed */
    if( conn_elt->http_id == http_id )
    {
        assert( conn_elt->udata_id == udata_id &&
                conn_elt->type == CONN_ACCEPTED &&
                http_is_est( conn_elt->http_id ) != HTTP_STATE_EST );

        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );

        __cleanup_conn_data( http_id );
    }
    else
    {
        /* NOTE: can't assert conn_elt->udata_id != udata_id,  *
         * because it may be udata_id of listen conn, accepted *
         * conn may be closed before replacing udata_id        */

        LOGD( "http_id:0x%llx", PTRID_FMT( http_id ) );
    }

    __random_calls();
}

static void __listen_clo_cb( http_id_t     http_id,
                             ptr_id_t      udata_id,
                             int           code )
{
    conn_elt_t     *conn_elt = NULL;

    assert( http_id && udata_id && g_is_listening );

    LOGD( "http_id:0x%llx udata_id:0x%llx code:%d",
          PTRID_FMT( http_id ), PTRID_FMT( udata_id ), code );

    conn_elt = PTRID_GET_PTR( udata_id );

    /* NOTE: this is possible because it is never freed */
    if( conn_elt->http_id == http_id )
    {
        assert( conn_elt->udata_id == udata_id &&
                conn_elt->type == CONN_LISTEN );

        __cleanup_conn_data( http_id );
    }
    else
    {
        assert( conn_elt->udata_id != udata_id &&
                conn_elt->type != CONN_LISTEN );
    }

    __random_calls();

    g_is_listening = false;
}

/******************* Network interface callers ********************************/

static bool __rand_make_conn()
{
    conn_elt_t         *conn_elt;
    net_host_t         *rand_host;
    ptr_id_t            udata_id;
    int                 port;

    if( g_total_opened_conns == *cfg_net_test_total_conns )
        return false;

    conn_elt = __get_conn_free_elt();

    rand_host = __get_rand_host();

    port = atoi( rand_host->port );

    if( port == *cfg_net_test_listen_port ||
        port == *cfg_net_test_listen_port_ssl )
    {
        conn_elt->to_itself = true;
    }

    udata_id = PTRID( conn_elt );

    conn_elt->http_id = http_make_conn( rand_host,
                                        __rand_r_cb,
                                        __rand_est_cb,
                                        __rand_clo_cb,
                                        udata_id );

    assert( conn_elt->http_id );

    conn_elt->type = CONN_CONNECT;

    conn_elt->udata_id = udata_id;

    conn_elt->keep_alive = rand() % 10 ? true : false;

    conn_elt->is_ssl = rand_host->use_ssl;

    LOGD( "http_id:0x%llx", PTRID_FMT( conn_elt->http_id ) );

    return true;
}

static bool __rand_make_listen()
{
    conn_elt_t         *conn_elt;
    ptr_id_t            udata_id;
    int                 port;

    if( g_total_opened_conns == *cfg_net_test_total_conns ||
        g_is_listening )
    {
        return false;
    }

    assert( !g_is_listening );

    conn_elt = __get_conn_free_elt();

    udata_id = PTRID( conn_elt );

    port = rand() % 2 ? *cfg_net_test_listen_port :
                        *cfg_net_test_listen_port_ssl;

    conn_elt->http_id = http_make_listen( __accepted_r_cb,
                                          __accepted_est_cb,
                                          __accepted_clo_cb,
                                          __dup_udata_cb,
                                          __listen_clo_cb,
                                          udata_id,
                                          port,
                                          port ==
                                              *cfg_net_test_listen_port_ssl ?
                                              true : false );

    assert( conn_elt->http_id );

    conn_elt->type = CONN_LISTEN;

    conn_elt->udata_id = udata_id;

    conn_elt->keep_alive = rand() % 10 ? true : false;

    g_is_listening = true;

    LOGD( "http_id:0x%llx port:%d",
          PTRID_FMT( conn_elt->http_id ), port );

    return true;
}

static bool __rand_shutdown_conn()
{
    conn_elt_t         *conn_elt;
    int                 r;

    if( !g_total_opened_conns || rand() % 20 )
        return false;

    conn_elt = __get_rand_conn_elt( false );

    if( conn_elt->keep_alive )
        return false;

    r = http_shutdown( conn_elt->http_id, false );
    assert( !r );

    LOGD( "rand_conn_id:0x%llx",
          PTRID_FMT( conn_elt->http_id ) );

    __cleanup_conn_data( conn_elt->http_id );

    return true;
}

static bool __rand_post_data()
{
    conn_elt_t     *conn_elt;

    if( !g_total_opened_conns )
        return false;

    conn_elt = __get_rand_conn_elt( true );

    if( !conn_elt || conn_elt->type != CONN_CONNECT )
        return false;

    http_msg_t      http_msg;
    char            url[64];
    int             chk_index;
    int             r;

    __init_http_msg( &http_msg );

    chk_index = rand() % CHK_DATA_ARRAY_SIZE;

    http_msg.url_len = snprintf( url, sizeof(url),
                                 "/%u", chk_index );

    http_msg.url = url;

    http_msg.connection_close = rand() % 10 ? false : true;

    http_msg.accept_encoding = HTTP_ACCEPT_ENCODING_GZIP;
    http_msg.accept_encoding_len = HTTP_ACCEPT_ENCODING_GZIP_LEN;

    r = http_post_data( conn_elt->http_id, &http_msg, 0, NULL );
    assert( !r );

    __requests_queue_push( conn_elt,
                           conn_elt->to_itself ?
                               NULL : &g_chk_array[chk_index] );

    LOGD( "rand_conn_id:0x%llx url:%s "
          "msg_queue_size:%d checked_responses:%d",
          PTRID_FMT( conn_elt->http_id ), http_msg.url,
          conn_elt->requests_queue.total,
          conn_elt->checked_responses );

    return true;
}

static bool __rand_make_conn_tmr()
{
    conn_elt_t         *conn_elt;
    http_tmr_id_t       new_tmr_id;
    struct timeval      rand_timeout;
    ptr_id_t            udata_id = UDATA_LABEL;

    if( !g_total_opened_conns ||
        g_total_conn_tmrs + g_total_global_tmrs == *cfg_net_test_total_tmrs )
    {
        return false;
    }

    conn_elt = __get_rand_conn_elt( false );

    if( conn_elt->type != CONN_CONNECT )
        return false;

    __make_rand_timeout( &rand_timeout );

    new_tmr_id = http_make_tmr( conn_elt->http_id,
                                udata_id,
                                __rand_tmr_cb,
                                &rand_timeout );

    assert( new_tmr_id );

    __save_tmr_id( conn_elt, new_tmr_id );

    LOGD( "rand_conn_id:0x%llx tmr:0x%llx tv_sec:%ld tv_usec:%ld",
          PTRID_FMT( conn_elt->http_id ), PTRID_FMT( new_tmr_id ),
          rand_timeout.tv_sec, rand_timeout.tv_usec );

    return true;
}

static bool __rand_make_global_tmr()
{
    ptr_id_t            new_tmr_id;
    struct timeval      rand_timeout;
    ptr_id_t            udata_id = UDATA_LABEL;

    if( g_total_conn_tmrs + g_total_global_tmrs == *cfg_net_test_total_tmrs )
        return false;

    __make_rand_timeout( &rand_timeout );

    new_tmr_id = net_make_global_tmr( udata_id,
                                      __rand_tmr_cb,
                                      &rand_timeout );

    assert( new_tmr_id );

    __save_tmr_id( NULL, new_tmr_id );

    LOGD( "tmr:0x%llx tv_sec:%ld tv_usec:%ld",
          PTRID_FMT( new_tmr_id ), rand_timeout.tv_sec,
          rand_timeout.tv_usec );

    return true;
}

static bool __rand_del_conn_tmr()
{
    tmr_elt_t          *tmr_elt;
    http_id_t           http_id;
    int                 r;

    if( !g_total_conn_tmrs )
        return false;

    tmr_elt = __get_rand_tmr_id( &http_id );

    r = http_del_tmr( http_id, tmr_elt->tmr_id );
    assert( !r );

    LOGD( "rand_conn_id:0x%llx tmr:0x%llx",
          PTRID_FMT( http_id ), PTRID_FMT( tmr_elt->tmr_id ) );

    memset( tmr_elt, 0, sizeof(tmr_elt_t) );

    g_total_conn_tmrs--;
    assert( g_total_conn_tmrs >= 0 );

    return true;
}

static bool __rand_del_global_tmr()
{
    tmr_elt_t          *tmr_elt;
    int                 r;

    if( !g_total_global_tmrs )
        return false;

    tmr_elt = __get_rand_tmr_id( NULL );

    r = net_del_global_tmr( tmr_elt->tmr_id );
    assert( !r );

    LOGD( "tmr:0x%llx", PTRID_FMT( tmr_elt->tmr_id ) );

    memset( tmr_elt, 0, sizeof(tmr_elt_t) );

    g_total_global_tmrs--;
    assert( g_total_global_tmrs >= 0 );

    return true;
}

/******************* Head of random interface calls ***************************/

static int __random_function()
{
    int     rnd = rand() % NET_TOTAL_API;
    bool    done = false;

    assert( g_total_conn_tmrs >= 0 && g_total_global_tmrs >= 0 &&
            g_total_conn_tmrs + g_total_global_tmrs <=
                    *cfg_net_test_total_tmrs &&
            g_total_opened_conns <= *cfg_net_test_total_conns &&
            (!g_total_conn_tmrs || g_total_opened_conns) );

    LOGD( "rnd:%d opened_conns:%d conn_tmrs:%d "
          "global_tmrs:%d max_checked_responses:%d "
          "max_requests_queue_size:%d",
          rnd, g_total_opened_conns, g_total_conn_tmrs,
          g_total_global_tmrs, g_max_checked_responses,
          g_max_requests_queue_size );

    switch( rnd )
    {
        case NET_MAKE_CONN:
            done = __rand_make_conn();

            break;
        case NET_MAKE_LISTEN:
            done = __rand_make_listen();

            break;
        case NET_POST_DATA:
            done = __rand_post_data();

            break;
        case NET_MAKE_CONN_TMR:
            done = __rand_make_conn_tmr();

            break;
        case NET_MAKE_GLOBAL_TMR:
            done = __rand_make_global_tmr();

            break;
        case NET_DEL_CONN_TMR:
            done = __rand_del_conn_tmr();

            break;
        case NET_DEL_GLOBAL_TMR:
            done = __rand_del_global_tmr();

            break;
        case NET_CLOSE_CONN:
            done = __rand_shutdown_conn();

            break;
        default:
            assert( 0 );

            break;
    }

    LOGD( "rnd:%d done:%d", rnd, done );

    return done;
}

static void __random_calls()
{
    int     to;
    int     t = 0;
    int     i = 0;

    if( !g_total_global_tmrs )
        to = rand() % *cfg_net_test_max_calls + 1;
    else
        to = rand() % *cfg_net_test_max_calls;

    LOGD( "global_tmrs:%d calls:%d", g_total_global_tmrs, to );

    while( i < to || !g_total_global_tmrs )
    {
        if( !__random_function() && t < *cfg_net_test_max_calls )
        {
            to++;
            t++;
        }

        i++;

        /* NOTE: to detect rare cases */
        assert( i < 1000 );
    }
}

/******************* Init *****************************************************/

static void __default_config_init()
{
    if( !cfg_net_test_update_hosts_interval )
    {
        cfg_net_test_update_hosts_interval = malloc( sizeof(struct timeval) );

        cfg_net_test_update_hosts_interval->tv_sec = 10;
        cfg_net_test_update_hosts_interval->tv_usec = 0;
    }

    /* NOTE: real assert for checking cfg */
    assert( cfg_net_test_hosts );

    if( !cfg_net_test_max_calls )
    {
        cfg_net_test_max_calls = malloc( sizeof(int) );

        *cfg_net_test_max_calls = 3;
    }

    if( !cfg_net_test_total_conns )
    {
        cfg_net_test_total_conns = malloc( sizeof(int) );

        *cfg_net_test_total_conns = 10;
    }

    if( !cfg_net_test_total_tmrs )
    {
        cfg_net_test_total_tmrs = malloc( sizeof(int) );

        *cfg_net_test_total_tmrs = 100;
    }

    if( !cfg_net_test_listen_port )
    {
        cfg_net_test_listen_port = malloc( sizeof(int) );

        *cfg_net_test_listen_port = 8888;
    }

    if( !cfg_net_test_listen_port_ssl )
    {
        cfg_net_test_listen_port_ssl = malloc( sizeof(int) );

        *cfg_net_test_listen_port_ssl = 9999;
    }
}

void net_test_cfg_init()
{
    config_add_file( "selftest/selftest.cfg" );

    config_add_cmd( "net_test_update_hosts_interval",
                    CONFIG_CMD_TYPE_TIMEVAL,
                    (void **) &cfg_net_test_update_hosts_interval );

    config_add_cmd( "net_test_hosts",
                    CONFIG_CMD_TYPE_MAIN_HOSTS,
                    (void **) &cfg_net_test_hosts );

    config_add_cmd( "net_test_max_calls",
                    CONFIG_CMD_TYPE_INTEGER,
                    (void **) &cfg_net_test_max_calls );

    config_add_cmd( "net_test_total_conns",
                    CONFIG_CMD_TYPE_INTEGER,
                    (void **) &cfg_net_test_total_conns );

    config_add_cmd( "net_test_total_tmrs",
                    CONFIG_CMD_TYPE_INTEGER,
                    (void **) &cfg_net_test_total_tmrs );

    config_add_cmd( "net_test_listen_port",
                    CONFIG_CMD_TYPE_INTEGER,
                    (void **) &cfg_net_test_listen_port );

    config_add_cmd( "net_test_listen_port_ssl",
                    CONFIG_CMD_TYPE_INTEGER,
                    (void **) &cfg_net_test_listen_port_ssl );
}

void net_test_init()
{
    ptr_id_t    main_host_array_id;
    int         conn_array_size;
    int         tmr_array_size;

    __default_config_init();

    conn_array_size = sizeof(conn_elt_t) * *cfg_net_test_total_conns;
    g_conn_array = malloc( conn_array_size );
    memset( g_conn_array, 0, conn_array_size );

    tmr_array_size = sizeof(tmr_elt_t) * *cfg_net_test_total_tmrs;
    g_tmr_array = malloc( tmr_array_size );
    memset( g_tmr_array, 0, tmr_array_size );

    main_host_array_id = PTRID( cfg_net_test_hosts );

    net_update_main_hosts( 0, 0, 0, main_host_array_id );

    net_make_global_tmr( main_host_array_id,
                         net_update_main_hosts,
                         cfg_net_test_update_hosts_interval );

    __random_calls();
}

