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
#include "network_internal.h"

static ctx_t            g_ctx_array[NET_MAX_FD + 1];
static uint32_t         g_ctx_total = 0;
static int              g_epollfd;
static SSL_CTX         *g_ssl_client_ctx;
static SSL_CTX         *g_ssl_server_ctx;
static ll_t             g_tmr_list = {0};
static struct timeval   g_iter_time;

char                   *cfg_net_cert_file = NULL;
char                   *cfg_net_key_file = NULL;

char                   *cfg_net_cert_test_file = NULL;
char                   *cfg_net_key_test_file = NULL;

struct timeval         *cfg_net_ssl_shutdown_timeout = NULL;
struct timeval         *cfg_net_ssl_establish_timeout = NULL;
struct timeval         *cfg_net_ssl_accept_timeout = NULL;
struct timeval         *cfg_net_establish_timeout = NULL;
struct timeval         *cfg_net_flush_and_close_timeout = NULL;

/* NOTE: start at the next event from epoll_wait() */
static bool             g_skip_cb = false;

static void __ssl_start_shutdown( ctx_t *ctx, int code );
static void __ssl_start_accept( ctx_t *ctx );
static void __ssl_start_connect( ctx_t *ctx );

static void __ssl_write_cb( ctx_t *ctx );
static void __ssl_read_cb( ctx_t *ctx );

static void __ssl_shutdown_cb( ctx_t *ctx );
static void __ssl_accept_cb( ctx_t *ctx );
static void __ssl_connect_cb( ctx_t *ctx );

static void __write_cb( ctx_t *ctx );
static void __read_cb( ctx_t *ctx );

static void __listen_cb( ctx_t *ctx );
static void __connect_cb( ctx_t *ctx );

enum {
    S_LISTENING = 0,
    S_CONNECTING,
    S_ESTABLISHED,
    S_SSL_CONNECTING,
    S_SSL_ACCEPTING,
    S_SSL_ESTABLISHED,
    S_SSL_SHUTDOWN
};

/* S == STATE */

static state_t          g_ctx_state[] = 
{
    { .st = S_LISTENING,
      .ssl_rw_st = 0,
      .r_cb = __listen_cb,
      .w_cb = __listen_cb },

    { .st = S_CONNECTING,
      .ssl_rw_st = 0,
      .r_cb = __connect_cb,
      .w_cb = __connect_cb },

    { .st = S_ESTABLISHED,
      .ssl_rw_st = 0,
      .r_cb = __read_cb,
      .w_cb = __write_cb },

    { .st = S_SSL_CONNECTING,
      .ssl_rw_st = 0,
      .r_cb = __ssl_connect_cb,
      .w_cb = __ssl_connect_cb },

    { .st = S_SSL_ACCEPTING,
      .ssl_rw_st = 0,
      .r_cb = __ssl_accept_cb,
      .w_cb = __ssl_accept_cb },

    { .st = S_SSL_ESTABLISHED,
      .ssl_rw_st = 0,
      .r_cb = __ssl_read_cb,
      .w_cb = __ssl_write_cb },

    { .st = S_SSL_SHUTDOWN,
      .ssl_rw_st = 0,
      .r_cb = __ssl_shutdown_cb,
      .w_cb = __ssl_shutdown_cb }
};

/******************* Socket functions *****************************************/

static int __set_socket_params( int     fd,
                                bool    set_nonblock_by_fcntl )
{
    int                 on = 1;

    if( set_nonblock_by_fcntl )
    {
        if( fcntl( fd, F_SETFL, O_NONBLOCK ) == -1 )
        {
            LOGE( "fd:%x errno:%d strerror:%s",
                  fd, errno, strerror( errno ) );

            return -1;
        }
    }

    if( setsockopt( fd,
                    SOL_SOCKET,
                    SO_REUSEADDR,
                    (void *) &on, sizeof(on) ) )
    {
        LOGE( "fd:%x errno:%d strerror:%s",
              fd, errno, strerror( errno ) );

        return -1;
    }

    return 0;
}

static int __create_socket()
{
    int                 fd;
    int                 type;
    bool                set_nonblock_by_fcntl;

    /* NOTE: Since Linux 2.6.27, SOCK_NONBLOCK is available */
#ifdef SOCK_NONBLOCK

    type = SOCK_STREAM | SOCK_NONBLOCK;
    set_nonblock_by_fcntl = false;

#else

    type = SOCK_STREAM;
    set_nonblock_by_fcntl = true;

#endif

    if( (fd = socket(AF_INET, type, 0)) == -1 )
    {
        LOGE( "fd:%x errno:%d strerror:%s",
              fd, errno, strerror( errno ) );

        return -1;
    }

    if( fd > NET_MAX_FD )
    {
        LOGE( "fd:%x", fd );

        PROPER_CLOSE_FD( fd );

        G_net_errno = NET_ERRNO_CONN_MAX;
        return -1;
    }

    if( __set_socket_params( fd, set_nonblock_by_fcntl ) )
    {
        PROPER_CLOSE_FD( fd );
        return -1;
    }

    LOGD( "fd:%x", fd );

    return fd;
}

static void __shutdown_write( ctx_t *ctx )
{
    if( shutdown( ctx->fd, SHUT_WR ) == -1 )
    {
        LOGE( "id:0x%llx fd:%x host:%s:%s state:%d errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), ctx->fd, ctx->host, ctx->port,
              ctx->state->st, errno, strerror( errno ) );
    }

    ctx->is_shut_wr_done = true;
}

/******************* ctx_t misc functions *************************************/

static ctx_t *__init_new_ctx( int fd )
{
    ctx_t                  *ctx;

    c_assert( fd >= 0 && fd <= NET_MAX_FD );

    ctx = &g_ctx_array[fd];
    c_assert( !ctx->id );

    ctx->fd = fd;

    ctx->id = PTRID( ctx );

    g_ctx_total++;
    c_assert( g_ctx_total <= NET_MAX_FD );

    return ctx;
}

static ctx_t *__get_ctx( conn_id_t conn_id )
{
    ctx_t          *ctx;

    ctx = PTRID_GET_PTR( conn_id );

    c_assert( ctx >= g_ctx_array &&
              ctx <= g_ctx_array + sizeof(ctx_t) * NET_MAX_FD );

    c_assert( ctx->id == conn_id );

    c_assert( ctx->fd >= 0 && ctx->fd <= NET_MAX_FD &&
              ctx == &g_ctx_array[ctx->fd] && ctx->id );

    return ctx;
}

static void __cancel_ctx( ctx_t *ctx )
{
    memset( ctx, 0, sizeof(ctx_t) );

    g_ctx_total--;
    c_assert( g_ctx_total >= 0 );
}

/******************* epoll functions ******************************************/

static void __add_to_epoll( ctx_t *ctx )
{
    struct epoll_event      event;

    /* NOTE: EPOLLOUT will be disabled after established */
    ctx->ev = EPOLLIN | EPOLLOUT | EPOLLRDHUP;

    event.events = ctx->ev;
    event.data.fd = ctx->fd;

    if( epoll_ctl( g_epollfd,
                   EPOLL_CTL_ADD,
                   event.data.fd,
                   &event ) )
    {
        LOGE( "id:0x%llx host:%s:%s ev:0x%x errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              ctx->ev, errno, strerror( errno ) );
    }

    LOGD( "id:0x%llx host:%s:%s ev:0x%x",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port, ctx->ev );
}

static void __del_from_epoll( ctx_t *ctx )
{
    if( epoll_ctl( g_epollfd,
                   EPOLL_CTL_DEL,
                   ctx->fd,
                   NULL ) )
    {
        LOGE( "id:0x%llx host:%s:%s ev:0x%x errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              ctx->ev, errno, strerror( errno ) );
    }

    LOGD( "id:0x%llx host:%s:%s ev:0x%x",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port, ctx->ev );
}

static void __enable_write( ctx_t *ctx )
{
    struct epoll_event      event;

    if( ctx->ev & EPOLLOUT )
        return;

    event.events = ( ctx->ev | EPOLLOUT );
    event.data.fd = ctx->fd;

    if( epoll_ctl( g_epollfd,
                   EPOLL_CTL_MOD,
                   event.data.fd,
                   &event ) )
    {
        LOGE( "id:0x%llx host:%s:%s ev:0x%x errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              ctx->ev, errno, strerror( errno ) );
    }
    else
        ctx->ev = event.events;

    LOGD( "id:0x%llx host:%s:%s ev:0x%x",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port, ctx->ev );
}

static void __disable_write( ctx_t *ctx )
{
    struct epoll_event      event;

    if( !(ctx->ev & EPOLLOUT) )
    {
        /* NOTE: it may happens because of EPOLLRDHUP etc, *
         * so use LOGD instead of LOGE here                */
        LOGD( "id:0x%llx host:%s:%s ev:0x%x state:%d",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              ctx->ev, ctx->state->st );

        return;
    }

    event.events = (ctx->ev & ~EPOLLOUT);
    event.data.fd = ctx->fd;

    if( epoll_ctl( g_epollfd,
                   EPOLL_CTL_MOD,
                   event.data.fd,
                   &event ) )
    {
        /* NOTE: it's a catastrofic error, but don't use *
         * assert for sys calls, so just logging without *
         * handling an error                             */
        LOGE( "id:0x%llx host:%s:%s ev:0x%x errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              ctx->ev, errno, strerror( errno ) );
    }
    else
        ctx->ev = event.events;

    LOGD( "id:0x%llx host:%s:%s ev:0x%x",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port, ctx->ev );
}

/******************* tmr_t misc functions *************************************/

static tmr_t *__make_timer( net_tmr_cb_t    cb,
                            struct timeval *timeout )
{
    tmr_t                  *tmr;
    struct timeval          tv = G_now;

    tmr = malloc( sizeof(tmr_t) );
    memset( tmr, 0, sizeof(tmr_t) );

    tmr->uh_cb = cb;

    timeradd( &tv, timeout, &tv );

    tmr->timeout.tv_sec = tv.tv_sec;
    tmr->timeout.tv_usec = tv.tv_usec;

    tmr->shift.tv_sec = timeout->tv_sec;
    tmr->shift.tv_usec = timeout->tv_usec;

    return tmr;
}

/*  Can be used for internal network timers  *
 *  and user level timers.                   *
 *  Example of user level connection timers: *
 *      to control last read time            *
 *      to control last write time           */

static tmr_id_t __make_conn_tmr( ctx_t           *ctx,
                                 ptr_id_t         udata_id,
                                 net_tmr_cb_t     cb,
                                 struct timeval  *timeout )
{
    tmr_t                  *tmr;

    if( ctx->tmr_list.total > MAX_TIMERS )
    {
        LOGE( "ctx_id:0x%llx", PTRID_FMT( ctx->id ) );

        G_net_errno = NET_ERRNO_TMR_MAX;
        return 0;
    }

    tmr = __make_timer( cb, timeout );

    LL_ADD_NODE( &ctx->tmr_list, tmr );

    tmr->udata_id = udata_id;

    LOG( "id:0x%llx host:%s:%s tmr:0x%llx "
         "timeout.tv_sec:%ld timeout.tv_usec:%ld "
         "shift.tv_sec:%ld shift.tv_usec:%ld",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port, PTRID_FMT( tmr->id ),
         tmr->timeout.tv_sec, tmr->timeout.tv_usec,
         tmr->shift.tv_sec, tmr->shift.tv_usec );

    return tmr->id;
}

static int __del_timer( tmr_t      *tmr,
                        ll_t       *list )
{
    if( tmr->locked && !tmr->to_delete )
    {
        LOG( "tmr:0x%llx", PTRID_FMT( tmr->id ) );

        tmr->to_delete = true;

        return 0;
    }
    else
    if( tmr->locked && tmr->to_delete )
    {
        LOGE( "tmr:0x%llx", PTRID_FMT( tmr->id ) );

        G_net_errno = NET_ERRNO_GENERAL_ERR;
        return -1;
    }

    LL_DEL_NODE( list, tmr->id );

    /* NOTE: delete tmr label and pointers before free(), *
     * so asserts can work if something goes wrong        */
    memset( tmr, 0, sizeof(tmr_t) );
    free( tmr );

    return 0;
}

static int __del_conn_tmr( ctx_t      *ctx,
                           tmr_id_t    tmr_id )
{
    tmr_t                  *tmr;

    tmr = PTRID_GET_PTR( tmr_id );

    LOG( "id:0x%llx host:%s:%s tmr:0x%llx",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port,
         PTRID_FMT( tmr_id ) );

    return __del_timer( tmr,
                        &ctx->tmr_list );
}

/******************* Destroy ctx functions ************************************/

static void __cleanup_timers( ctx_t *ctx )
{
    tmr_t                  *tmr, *tmr_next;
    int                     r;

    if( !ctx->tmr_list.total )
    {
        c_assert( !ctx->tmr_list.head &&
                  !ctx->tmr_list.tail );

        return;
    }

    LL_CHECK( &ctx->tmr_list, ctx->tmr_list.head );
    tmr = PTRID_GET_PTR( ctx->tmr_list.head );

    while( tmr )
    {
        LOGD( "id:0x%llx host:%s:%s tmr:0x%llx",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              PTRID_FMT( tmr->id ) );

        tmr_next = PTRID_GET_PTR( tmr->next );

        r = __del_conn_tmr( ctx, tmr->id );
        c_assert( !r );

        tmr = tmr_next;
    }

    c_assert( !ctx->tmr_list.total &&
              !ctx->tmr_list.head &&
              !ctx->tmr_list.tail );
}

/* NOTE: can not call r_uh_cb because of *
 * destructor clo_uh_cb before           */
static void __cleanup_buffers( ctx_t *ctx )
{
    wbuf_t             *wbuf;
    wbuf_t             *wbuf_next;

    if( ctx->rb.buf )
    {
        B_GENERAL_CHECK( ctx->rb );

        if( B_HAS_USED( ctx->rb ) )
        {
            LOG( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu", 
                 PTRID_FMT( ctx->id ), ctx->host, ctx->port,
                 B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ) );
        }

        B_FREE( ctx->rb );
    }

    if( !ctx->wb_list.total )
    {
        c_assert( !ctx->wb_list.head &&
                  !ctx->wb_list.tail );

        return;
    }

    LL_CHECK( &ctx->wb_list, ctx->wb_list.head );
    wbuf = PTRID_GET_PTR( ctx->wb_list.head );

    while( wbuf )
    {
        LOGD( "id:0x%llx host:%s:%s msg_id:%llx",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              PTRID_FMT( wbuf->id ) );

        wbuf_next = PTRID_GET_PTR( wbuf->next );

        LL_DEL_NODE( &ctx->wb_list, wbuf->id );

        B_FREE( wbuf->b );

        memset( wbuf, 0, sizeof(wbuf_t) );
        free( wbuf );

        wbuf = wbuf_next;
    }

    c_assert( !ctx->wb_list.total &&
              !ctx->wb_list.head &&
              !ctx->wb_list.tail );
}

static void __cleanup_ctx( ctx_t *ctx )
{
    __cleanup_buffers( ctx );
    __cleanup_timers( ctx );

    if( ctx->ssl )
        SSL_free( ctx->ssl );

    memset( ctx, 0, sizeof(ctx_t) );
}

static void __destroy_ctx( ctx_t *ctx, int code )
{
    conn_id_t           prev_id = ctx->id;
    int                 how_shutdown;

    c_assert( ctx->id && !ctx->is_in_destroying );
    c_assert( !ctx->is_clo_uh_done || ctx->ssl );

    ctx->is_in_destroying = true;

    LOG( "id:0x%llx fd:%x host:%s:%s state:%d code:%d",
         PTRID_FMT( ctx->id ), ctx->fd, ctx->host, ctx->port,
         ctx->state->st, code );

    if( !ctx->is_clo_uh_done && ctx->clo_uh_cb )
    {
        ctx->clo_uh_cb( ctx->id, ctx->udata_id, code );

        c_assert( ctx->id == prev_id );
    }
    else
    if( !ctx->clo_uh_cb )
    {
        /* NOTE: only accepted and uninitialized *
         * conn don't have clo_uh_cb             */
        c_assert( ctx->dirn == D_INCOMING );
    }

    __del_from_epoll( ctx );

    if( ctx->is_shut_wr_done )
    {
#ifdef  DEBUG
        int siocoutq = 0;
        ioctl( ctx->fd, SIOCOUTQ, &siocoutq );

        if( siocoutq )
        {
            LOGE( "SIOCOUTQ:%d", siocoutq );
        }
#endif

        how_shutdown = SHUT_RD;
    }
    else
        how_shutdown = SHUT_RDWR;

    if( shutdown( ctx->fd, how_shutdown ) == -1 )
    {
        LOGE( "id:0x%llx fd:%x host:%s:%s state:%d errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), ctx->fd, ctx->host, ctx->port,
              ctx->state->st, errno, strerror( errno ) );
    }

    /* NOTE: close() can not be before clo_uh_cb because clo_uh_cb  *
     * can make a new connection which has the same 'fd' number,    *
     * so 'ctx' will be rewritten before it's completely cleaned up */
    PROPER_CLOSE_FD( ctx->fd );

    __cleanup_ctx( ctx );

    g_ctx_total--;
    c_assert( g_ctx_total >= 0 );
}

static void __shutdown_ctx( ctx_t *ctx, int code )
{
    if( ctx->state->st == S_SSL_ESTABLISHED )
        __ssl_start_shutdown( ctx, code );
    else
        __destroy_ctx( ctx, code );
}

/******************* Timeout functions ****************************************/

static void __ssl_shutdown_timeout_cb( conn_id_t    conn_id,
                                       ptr_id_t     conn_udata_id,
                                       tmr_id_t     tmr_id,
                                       ptr_id_t     tmr_udata_id )
{
    ctx_t      *ctx;

    ctx = __get_ctx( conn_id );

    c_assert( ctx->dirn != D_LISTEN &&
              ctx->state->st == S_SSL_SHUTDOWN &&
              ctx->ssl && tmr_id == ctx->state_tmr_id );

    LOG( "id:0x%llx host:%s:%s tmr:0x%llx",
         PTRID_FMT( ctx->id ), ctx->host,
         ctx->port, PTRID_FMT( tmr_id ) );

    ctx->to_shutdown = true;
}

static void __establish_timeout_cb( conn_id_t   conn_id,
                                    ptr_id_t    conn_udata_id,
                                    tmr_id_t    tmr_id,
                                    ptr_id_t    tmr_udata_id )
{
    ctx_t      *ctx;

    ctx = __get_ctx( conn_id );

    c_assert( ctx->dirn != D_LISTEN &&
              tmr_id == ctx->state_tmr_id );

    c_assert( ctx->state->st == S_CONNECTING ||
              ctx->state->st == S_SSL_CONNECTING ||
              ctx->state->st == S_SSL_ACCEPTING );

    LOG( "id:0x%llx host:%s:%s state:%d tmr:0x%llx",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port,
         ctx->state->st, PTRID_FMT( tmr_id ) );

    ctx->to_shutdown = true;
}

static void __flush_and_close_timeout_cb( conn_id_t     conn_id,
                                          ptr_id_t      conn_udata_id,
                                          tmr_id_t      tmr_id,
                                          ptr_id_t      tmr_udata_id )
{
    ctx_t      *ctx;

    ctx = __get_ctx( conn_id );

    c_assert( ctx->dirn != D_LISTEN &&
              tmr_id == ctx->state_tmr_id );

    c_assert( ctx->state->st == S_ESTABLISHED ||
              ctx->state->st == S_SSL_ESTABLISHED ||
              ctx->state->st == S_SSL_SHUTDOWN );

    LOG( "id:0x%llx host:%s:%s state:%d tmr:0x%llx",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port,
         ctx->state->st, PTRID_FMT( tmr_id ) );

    ctx->to_shutdown = true;
}

/******************* Buffer RW functions **************************************/

static int __call_read_handler( ctx_t      *ctx,
                                bool        is_closed )
{
    conn_id_t       prev_id = ctx->id;
    char           *host = ctx->host;
    char           *port = ctx->port;
    int             r;

    LOGD( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ) );

    r = ctx->r_uh_cb( ctx->id, ctx->udata_id,
                      B_USED_PTR( ctx->rb ),
                      B_USED_SIZE( ctx->rb ), is_closed );

    c_assert( ctx->id == prev_id );

    c_assert( (!ctx->ssl && ctx->state->st == S_ESTABLISHED) ||
              (ctx->ssl && ctx->state->st == S_SSL_ESTABLISHED) );

    if( ctx->to_shutdown )
    {
        if( is_closed )
        {
            LOGE( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu",
                  PTRID_FMT( ctx->id ), host, port,
                  B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ) );
        }
        else
        {
            LOG( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu",
                 PTRID_FMT( ctx->id ), host, port,
                 B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ) );
        }

        return -1;
    }

    if( r > 0 && r <= B_USED_SIZE( ctx->rb ) )
    {
        B_CUT_USED( ctx->rb, r );
    }
    else
    if( r )
    {
        LOGE( "id:0x%llx host:%s:%s "
              "rb.used:%lu rb.size:%lu ret:%d",
              PTRID_FMT( ctx->id ), host, port,
              B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ), r );
    }

    return 0;
}

static void __handle_write_buf( ctx_t *ctx )
{
    wbuf_t         *wbuf;
    char           *host = ctx->host, *port = ctx->port;

    LL_CHECK( &ctx->wb_list, ctx->wb_list.head );
    wbuf = PTRID_GET_PTR( ctx->wb_list.head );

    B_GENERAL_CHECK( wbuf->b );

    wbuf->tries++;

    if( wbuf->tries > MAX_WRITE_TRIES )
    {
        LOGE( "id:0x%llx host:%s:%s used:%lu size:%lu msg_id:%llx",
              PTRID_FMT( ctx->id ), host, port,
              B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
              PTRID_FMT( wbuf->id ) );
    }
    else
    {
        LOGD( "id:0x%llx host:%s:%s used:%lu size:%lu msg_id:%llx",
              PTRID_FMT( ctx->id ), host, port,
              B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
              PTRID_FMT( wbuf->id ) );
    }

    if( B_HAS_REMAINDER( wbuf->b ) )
        return;

    LOG( "id:0x%llx host:%s:%s msg_id:%llx wb_total:%d",
         PTRID_FMT( ctx->id ), host, port,
         PTRID_FMT( wbuf->id ), ctx->wb_list.total );

    LL_DEL_NODE( &ctx->wb_list, wbuf->id );

    B_FREE( wbuf->b );

    memset( wbuf, 0, sizeof(wbuf_t) );
    free( wbuf );

    if( !ctx->wb_list.total )
    {
        c_assert( !ctx->wb_list.head &&
                  !ctx->wb_list.tail );

        __disable_write( ctx );

        if( ctx->flush_and_close )
            __shutdown_write( ctx );
    }
}

/******************* SSL RW calls *********************************************/

static void __call_ssl_read( ctx_t *ctx )
{
    int                 r, e, syserr, i = 0, more = 1;
    char               *host = ctx->host, *port = ctx->port;
    unsigned long       ssl_e;
    char               *ssl_strerror;

    B_GENERAL_CHECK( ctx->rb );

    c_assert( B_HAS_REMAINDER( ctx->rb ) );

    c_assert( !ctx->state->ssl_rw_st ||
              ctx->state->ssl_rw_st == SSL_R_WANT_W );

    ctx->state->ssl_rw_st = 0;

    /* nginx-1.2.2 source says that:
     * SSL_read() may return data in parts, so try to read
     * until SSL_read() would return no data
     */

    while( more && B_HAS_REMAINDER( ctx->rb ) )
    {
        do
        {
            errno = 0;
            r = SSL_read( ctx->ssl,
                          B_REMAINDER_PTR( ctx->rb ),
                          B_REMAINDER_SIZE( ctx->rb ) );

            syserr = errno;
            e = SSL_get_error( ctx->ssl, r );
        }
        while( r < 0 && e == SSL_ERROR_SYSCALL && syserr == EINTR );

        ssl_e = ERR_get_error();
        ssl_strerror = ssl_e ? ERR_error_string(ssl_e, NULL) : "-";

        more = SSL_pending( ctx->ssl );

        /* NOTE: consider r == 0 as closed by peer */
        if( r >= 0 )
        {
            if( r > 0 )
            {
                B_INCREASE_USED( ctx->rb, r );
            }

            LOGD( "id:0x%llx host:%s:%s rb.used:%lu "
                  "rb.size:%lu iter:%d",
                  PTRID_FMT( ctx->id ), host, port,
                  B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ), i );

            if( B_REMAINDER_SIZE( ctx->rb ) < B_MIN_RDBUF_REMAINDER( ctx->rb ) )
            {
                B_INCREASE_BUF( ctx->rb, READ_BUFFER_SIZE );

                LOG( "id:0x%llx host:%s:%s rb.used:%lu "
                     "rb.size:%lu iter:%d",
                     PTRID_FMT( ctx->id ), host, port,
                     B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ), i );
            }

            if( __call_read_handler( ctx, !r ) )
            {
                LOG( "id:0x%llx host:%s:%s iter:%d",
                     PTRID_FMT( ctx->id ), host, port, i );

                return;
            }

            if( !r )
            {
                LOG( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu "
                     "iter:%d ssl_err:%d ssl_strerror:%s errno:%d strerror:%s",
                     PTRID_FMT( ctx->id ), host, port,
                     B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ),
                     i, e, ssl_strerror, syserr, strerror( syserr ) );

                __shutdown_ctx( ctx, NET_CODE_SUCCESS );
                return;
            }

            i++;
            continue;
        }

        LOGD( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu iter:%d "
              "ret:%d ssl_err:%d ssl_strerror:%s errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), host, port,
              B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ),
              i, r, e, ssl_strerror, syserr, strerror( syserr ) );

        switch( e )
        {
            case SSL_ERROR_WANT_READ:

                return;

            case SSL_ERROR_WANT_WRITE:

                ctx->state->ssl_rw_st = SSL_R_WANT_W;

                if( !(ctx->ev & EPOLLOUT) )
                {
                    LOGD( "id:0x%llx", PTRID_FMT( ctx->id ) );

                    __enable_write( ctx );
                }

                return;

            case SSL_ERROR_SYSCALL:

                if( syserr == EAGAIN )
                    return;

                /* fall through */

            default:

                LOGE( "id:0x%llx host:%s:%s "
                      "rb.used:%lu rb.size:%lu iter:%d ret:%d ssl_err:%d "
                      "ssl_strerror:%s errno:%d strerror:%s",
                      PTRID_FMT( ctx->id ), host, port,
                      B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ),
                      i, r, e, ssl_strerror, syserr, strerror( syserr ) );

                __shutdown_ctx( ctx, NET_CODE_ERR_READ );
                return;
        }
    }
}

static void __call_ssl_write( ctx_t *ctx )
{
    wbuf_t             *wbuf;
    int                 r, e, syserr;
    unsigned long       ssl_e;
    char               *ssl_strerror;

    /* NOTE: it may happened because of *
     * EPOLLRDHUP or after SSL_R_WANT_W */
    if( !ctx->wb_list.total )
    {
        c_assert( !ctx->wb_list.head &&
                  !ctx->wb_list.tail );

        LOGD( "id:0x%llx host:%s:%s",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port );

        __disable_write( ctx );
        return;
    }

    LL_CHECK( &ctx->wb_list, ctx->wb_list.head );
    wbuf = PTRID_GET_PTR( ctx->wb_list.head );

    B_GENERAL_CHECK( wbuf->b );

    c_assert( B_HAS_REMAINDER( wbuf->b ) );

    c_assert( !ctx->state->ssl_rw_st ||
              ctx->state->ssl_rw_st == SSL_W_WANT_R );

    ctx->state->ssl_rw_st = 0;

    do
    {
        errno = 0;
        r = SSL_write( ctx->ssl,
                       B_REMAINDER_PTR( wbuf->b ),
                       B_REMAINDER_SIZE( wbuf->b ) );

        syserr = errno;
        e = SSL_get_error( ctx->ssl, r );
    }
    while( r < 0 && e == SSL_ERROR_SYSCALL && syserr == EINTR );

    ssl_e = ERR_get_error();
    ssl_strerror = ssl_e ? ERR_error_string(ssl_e, NULL) : "-";

    if( r > 0 )
    {
        B_INCREASE_USED( wbuf->b, r );

        LOGD( "id:0x%llx host:%s:%s used:%lu size:%lu msg_id:%llx",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
              PTRID_FMT( wbuf->id ) );

        __handle_write_buf( ctx );
        return;
    }

    LOGD( "id:0x%llx host:%s:%s used:%lu size:%lu msg_id:%llx "
          "ret:%d ssl_err:%d ssl_strerror:%s errno:%d strerror:%s",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
          PTRID_FMT( wbuf->id ), r, e, ssl_strerror,
          syserr, strerror( syserr ) );

    switch( e )
    {
        case SSL_ERROR_WANT_READ:

            ctx->state->ssl_rw_st = SSL_W_WANT_R;
            return;

        case SSL_ERROR_WANT_WRITE:

            return;

        case SSL_ERROR_SYSCALL:

            if( syserr == EAGAIN )
                return;

        /* fall through */

        default:

            LOGE( "id:0x%llx host:%s:%s used:%lu size:%lu msg_id:%llx"
                  "ret:%d ssl_err:%d ssl_strerror:%s errno:%d strerror:%s",
                  PTRID_FMT( ctx->id ), ctx->host, ctx->port,
                  B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
                  PTRID_FMT( wbuf->id ), r, e, ssl_strerror,
                  syserr, strerror( syserr ) );

            __shutdown_ctx( ctx, NET_CODE_ERR_WRITE );
    }
}

static void __ssl_gen_rw_call( ctx_t      *ctx,
                               void     ( *primary_call )( ctx_t *ctx ),
                               void     ( *secondary_call )( ctx_t *ctx ),
                               int         primary_rw_state,
                               int         secondary_rw_state )
{
    c_assert( !ctx->state->ssl_rw_st ||
              ctx->state->ssl_rw_st == SSL_R_WANT_W ||
              ctx->state->ssl_rw_st == SSL_W_WANT_R );

    LOGD( "id:0x%llx host:%s:%s rw_state:%d",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          ctx->state->ssl_rw_st );

    if( ctx->state->ssl_rw_st == secondary_rw_state )
        return;
    else
    if( ctx->state->ssl_rw_st == primary_rw_state )
        secondary_call( ctx );

    if( !ctx->id ||
        ctx->state->ssl_rw_st == primary_rw_state ||
        ctx->to_shutdown )
    {
        return;
    }

    c_assert( ctx->state->st == S_SSL_ESTABLISHED );

    primary_call( ctx );
}

/******************* Changing state est/act functions *************************/

static void __call_est_handler( ctx_t *ctx )
{
    conn_id_t           prev_id = ctx->id;

    /* NOTE: user handler est_uh_cb may enable write by adding *
     * to write buffer, so disable EPOLLOUT before             */
    __disable_write( ctx );

    ctx->est_uh_cb( ctx->id, ctx->udata_id );

    c_assert( ctx->id == prev_id );

    c_assert( ctx->state->st == S_ESTABLISHED ||
              ctx->state->st == S_SSL_ESTABLISHED );

    if( ctx->to_shutdown )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ),
             ctx->host, ctx->port );

        return;
    }

    /* close uninitialized accepted conn */
    if( !ctx->r_uh_cb || !ctx->clo_uh_cb || !ctx->udata_id )
    {
        c_assert( ctx->dirn == D_INCOMING );

        __shutdown_ctx( ctx, NET_CODE_ERR_EST );
    }
}

static void __ssl_established( ctx_t *ctx )
{
    int                 r;

    c_assert( (ctx->dirn == D_OUTGOING &&
               ctx->state->st == S_SSL_CONNECTING) ||
              (ctx->dirn == D_INCOMING &&
               ctx->state->st == S_SSL_ACCEPTING) );

    LOG( "id:0x%llx host:%s:%s",
         PTRID_FMT( ctx->id ),
         ctx->host, ctx->port );

    r = __del_conn_tmr( ctx, ctx->state_tmr_id );
    c_assert( !r );

    ctx->state_tmr_id = 0;

    g_skip_cb = true;

    ctx->state = &g_ctx_state[S_SSL_ESTABLISHED];
    c_assert( ctx->state->st == S_SSL_ESTABLISHED );

    __call_est_handler( ctx );
}

static void __established( ctx_t *ctx )
{
    int                 r;

    c_assert( ctx->dirn != D_LISTEN );

    if( ctx->dirn == D_OUTGOING )
    {
        r = __del_conn_tmr( ctx, ctx->state_tmr_id );
        c_assert( !r );

        ctx->state_tmr_id = 0;
    }
    else
    {
        c_assert( !ctx->state_tmr_id );
    }

    if( ctx->ssl && ctx->dirn == D_OUTGOING )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ),
             ctx->host, ctx->port );

        __ssl_start_connect( ctx );
    }
    else
    if( ctx->ssl && ctx->dirn == D_INCOMING )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ),
             ctx->host, ctx->port );

        __ssl_start_accept( ctx );
    }
    else
    if( !ctx->ssl )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ),
             ctx->host, ctx->port );

        ctx->state = &g_ctx_state[S_ESTABLISHED];
        c_assert( ctx->state->st == S_ESTABLISHED );

        g_skip_cb = true;

        __call_est_handler( ctx );
    }
}

static void __call_dup_udata( ctx_t    *ctx,
                              ctx_t    *listen_ctx )
{
    conn_id_t           listen_prev_id = listen_ctx->id;
    conn_id_t           prev_id = ctx->id;

    c_assert( !ctx->udata_id && listen_ctx->udata_id );

    ctx->is_in_dup_udata = true;
    listen_ctx->is_in_dup_udata = true;

    ctx->udata_id = listen_ctx->dup_udata_cb( ctx->id,
                                              listen_ctx->udata_id );

    c_assert( listen_ctx->id == listen_prev_id &&
              listen_ctx->state->st == S_LISTENING &&
              ctx->id == prev_id && !ctx->state &&
              ctx->udata_id != listen_ctx->udata_id );

    ctx->is_in_dup_udata = false;
    listen_ctx->is_in_dup_udata = false;
}

static void __create_accepted( ctx_t               *listen_ctx,
                               int                  fd,
                               struct sockaddr_in  *peer )
{
    ctx_t                  *ctx;
    SSL                    *ssl;

    if( fd > NET_MAX_FD )
    {
        LOGE( "listen_id:0x%llx listen_port:%d new_fd:%x",
              PTRID_FMT( listen_ctx->id ),
              listen_ctx->listen_port, fd );

        PROPER_CLOSE_FD( fd );
        return;
    }

    /* NOTE: O_NONBLOCK should be set explicitly */
    if( __set_socket_params( fd, true ) )
    {
        LOGE( "listen_id:0x%llx listen_port:%d new_fd:%x",
              PTRID_FMT( listen_ctx->id ),
              listen_ctx->listen_port, fd );

        PROPER_CLOSE_FD( fd );
        return;
    }

    ctx = __init_new_ctx( fd );
    c_assert( ctx );

    if( listen_ctx->child_use_ssl )
    {
        ssl = SSL_new( g_ssl_server_ctx );
        c_assert( ssl );

        SSL_set_fd( ssl, ctx->fd );
        SSL_set_accept_state( ssl ); /* means doing as server */

        ctx->ssl = ssl;
    }

    if( peer )
    {
        inet_ntop( AF_INET, &peer->sin_addr,
                   ctx->host, sizeof(ctx->host) );

        snprintf( ctx->port, sizeof(ctx->port), "%d",
                  ntohs(peer->sin_port) );
    }
    else
    {
        ctx->host[0] = '\0';
        ctx->port[0] = '\0';
    }

    B_ALLOC( ctx->rb, READ_BUFFER_SIZE );

    ctx->r_uh_cb = listen_ctx->child_r_uh_cb;
    ctx->est_uh_cb = listen_ctx->child_est_uh_cb;
    ctx->clo_uh_cb = listen_ctx->child_clo_uh_cb;

    ctx->dirn = D_INCOMING;

    __call_dup_udata( ctx, listen_ctx );

    LOG( "listen_id:0x%llx new_id:0x%llx new_fd:%x "
         "host:%s:%s use_ssl:%d",
         PTRID_FMT( listen_ctx->id ), PTRID_FMT( ctx->id ),
         ctx->fd, ctx->host, ctx->port, !!ctx->ssl );

    /* NOTE: EPOLLOUT was enabled during EPOLL_CTL_ADD */
    __add_to_epoll( ctx );

    __established( ctx );
}

/******************* SSL changing state functions *****************************/

/* NOTE: it deletes all timers and makes one timer */

static void __ssl_start_shutdown( ctx_t *ctx, int code )
{
    conn_id_t           prev_id = ctx->id;

    c_assert( ctx->ssl && ctx->state->st == S_SSL_ESTABLISHED &&
              !ctx->is_clo_uh_done && ctx->dirn != D_LISTEN &&
              !ctx->to_shutdown );

    /* NOTE: in case of flush_and_close, state_tmr_id will be rewritten */
    c_assert( !ctx->state_tmr_id || ctx->flush_and_close );

    g_skip_cb = true;

    ctx->state = &g_ctx_state[S_SSL_SHUTDOWN];
    c_assert( ctx->state->st == S_SSL_SHUTDOWN );

    /* NOTE: only __ssl_shutdown_timeout_cb is needed during shutdown  */
    /* NOTE: also, to prevent to invoke __establish_timeout_cb several *
     * times during shutdown                                           */
    __cleanup_timers( ctx );

    ctx->state_tmr_id = __make_conn_tmr( ctx, 0,
                                         __ssl_shutdown_timeout_cb,
                                         cfg_net_ssl_shutdown_timeout );

    c_assert( ctx->state_tmr_id );

    __enable_write( ctx );

    LOG( "id:0x%llx host:%s:%s code:%d",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port, code );

    if( ctx->clo_uh_cb )
    {
        /* NOTE: clo_uh_cb should not call  *
         * close conn and another functions */
        ctx->clo_uh_cb( ctx->id, ctx->udata_id, code );

        /* NOTE: it's impossible to set to_shutdown *
         * with S_SSL_SHUTDOWN                      */
        c_assert( ctx->id == prev_id && !ctx->to_shutdown );

        ctx->is_clo_uh_done = true;
    }
    else
    {
        /* NOTE: only accepted and uninitialized *
         * conn don't have clo_uh_cb             */
        c_assert( ctx->dirn == D_INCOMING );
    }
}

/* NOTE: don't need to enable write because it was enabled */
static void __ssl_start_accept( ctx_t *ctx )
{
    c_assert( ctx->ssl && ctx->dirn == D_INCOMING &&
              !ctx->state && !ctx->state_tmr_id );

    ctx->state = &g_ctx_state[S_SSL_ACCEPTING];
    c_assert( ctx->state->st == S_SSL_ACCEPTING );

    ctx->state_tmr_id = __make_conn_tmr( ctx, 0,
                                         __establish_timeout_cb,
                                         cfg_net_ssl_accept_timeout );

    c_assert( ctx->state_tmr_id );

    LOG( "id:0x%llx host:%s:%s tmr:0x%llx",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port,
         PTRID_FMT( ctx->state_tmr_id ) );
}

/* NOTE: don't need to enable write because it was enabled */
static void __ssl_start_connect( ctx_t *ctx )
{
    c_assert( ctx->ssl && ctx->dirn == D_OUTGOING && !ctx->state_tmr_id );

    g_skip_cb = true;

    ctx->state = &g_ctx_state[S_SSL_CONNECTING];
    c_assert( ctx->state->st = S_SSL_CONNECTING );

    ctx->state_tmr_id = __make_conn_tmr( ctx, 0,
                                         __establish_timeout_cb,
                                         cfg_net_ssl_establish_timeout );

    c_assert( ctx->state_tmr_id );

    LOG( "id:0x%llx host:%s:%s tmr:0x%llx",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port,
         PTRID_FMT( ctx->state_tmr_id ) );
}

/******************* non-SSL changing state functions *************************/

static void __start_listen( ctx_t *ctx )
{
    c_assert( !ctx->ssl && ctx->dirn == D_LISTEN &&
              !ctx->state && !ctx->state_tmr_id );

    ctx->state = &g_ctx_state[S_LISTENING];
    c_assert( ctx->state->st == S_LISTENING );

    /* NOTE: EPOLLOUT was enabled during EPOLL_CTL_ADD */
    __add_to_epoll( ctx );

    /* NOTE: EPOLL_CTL_ADD adds EPOLLOUT, so disable it by        *
     * excessive call. If EPOLLOUT is not added by EPOLL_CTL_ADD  *
     * then regular connection should make excessive call.        *
     * So it would be better to make excessive call at listening. */
    __disable_write( ctx );

    LOG( "id:0x%llx listen_port:%d",
         PTRID_FMT( ctx->id ), ctx->listen_port );
}

static void __start_connect( ctx_t *ctx )
{
    c_assert( ctx->dirn == D_OUTGOING &&
              !ctx->state && !ctx->state_tmr_id );

    ctx->state = &g_ctx_state[S_CONNECTING];
    c_assert( ctx->state->st == S_CONNECTING );

    ctx->state_tmr_id = __make_conn_tmr( ctx, 0,
                                         __establish_timeout_cb,
                                         cfg_net_establish_timeout );

    c_assert( ctx->state_tmr_id );

    /* NOTE: EPOLLOUT was enabled during EPOLL_CTL_ADD */
    __add_to_epoll( ctx );

    LOG( "id:0x%llx host:%s:%s tmr:0x%llx",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port,
         PTRID_FMT( ctx->state_tmr_id ) );
}

/******************* SSL event callbacks **************************************/

static void __ssl_write_cb( ctx_t *ctx )
{
    __ssl_gen_rw_call( ctx,
                       __call_ssl_write,
                       __call_ssl_read,
                       SSL_R_WANT_W,
                       SSL_W_WANT_R );
}

static void __ssl_read_cb( ctx_t *ctx )
{
    __ssl_gen_rw_call( ctx,
                       __call_ssl_read,
                       __call_ssl_write,
                       SSL_W_WANT_R,
                       SSL_R_WANT_W );
}

static void __ssl_shutdown_cb( ctx_t *ctx )
{
    int                 r, e, syserr;
    unsigned long       ssl_e;
    char               *ssl_strerror;

    do
    {
        errno = 0;
        r = SSL_shutdown( ctx->ssl );

        syserr = errno;
        e = SSL_get_error( ctx->ssl, r );
    }
    while( r < 0 && e == SSL_ERROR_SYSCALL && syserr == EINTR );

    ssl_e = ERR_get_error();
    ssl_strerror = ssl_e ? ERR_error_string(ssl_e, NULL) : "-";

    if( r == 1 )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ), ctx->host, ctx->port );

        __shutdown_ctx( ctx, NET_CODE_SUCCESS );
        return;
    }

    LOGD( "id:0x%llx host:%s:%s ssl_err:%d "
          "ssl_strerror:%s errno:%d strerror:%s",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          e, ssl_strerror, syserr, strerror( syserr ) );

    switch( e )
    {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:

            g_skip_cb = true;
            return;

        case SSL_ERROR_SYSCALL:

            if( syserr == EAGAIN )
            {
                g_skip_cb = true;
                return;
            }

            /* fall through */

        default:

            LOGE( "id:0x%llx host:%s:%s ssl_err:%d "
                  "ssl_strerror:%s errno:%d strerror:%s",
                  PTRID_FMT( ctx->id ), ctx->host, ctx->port,
                  e, ssl_strerror, syserr, strerror( syserr ) );

            __shutdown_ctx( ctx, NET_CODE_ERR_SHUT );
    }
}

static void __ssl_accept_cb( ctx_t *ctx )
{
    int                 r, e, syserr;
    unsigned long       ssl_e;
    char               *ssl_strerror;

    do
    {
        errno = 0;
        r = SSL_accept( ctx->ssl );

        syserr = errno;
        e = SSL_get_error( ctx->ssl, r );
    }
    while( r < 0 && e == SSL_ERROR_SYSCALL && syserr == EINTR );

    ssl_e = ERR_get_error();
    ssl_strerror = ssl_e ? ERR_error_string(ssl_e, NULL) : "-";

    if( r == 1 )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ), ctx->host, ctx->port );

        __ssl_established( ctx );
        return;
    }

    LOGD( "id:0x%llx host:%s:%s ret:%d ssl_err:%d "
          "ssl_strerror:%s errno:%d strerror:%s",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          r, e, ssl_strerror, syserr, strerror( syserr ) );

    switch( e )
    {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:

            g_skip_cb = true;
            return;

        case SSL_ERROR_SYSCALL:

            if( syserr == EAGAIN )
            {
                g_skip_cb = true;
                return;
            }

            /* fall through */

        default:

            LOGE( "id:0x%llx host:%s:%s ret:%d ssl_err:%d "
                  "ssl_strerror:%s errno:%d strerror:%s",
                  PTRID_FMT( ctx->id ), ctx->host, ctx->port,
                  r, e, ssl_strerror, syserr, strerror( syserr ) );

            /* NOTE: close accepted conn */
            __shutdown_ctx( ctx, NET_CODE_ERR_ACCEPT );
    }
}

static void __ssl_connect_cb( ctx_t *ctx )
{
    int                 r, e, syserr;
    unsigned long       ssl_e;
    char               *ssl_strerror;

    do
    {
        errno = 0;
        r = SSL_connect( ctx->ssl );

        syserr = errno;
        e = SSL_get_error( ctx->ssl, r );
    }
    while( r < 0 && e == SSL_ERROR_SYSCALL && syserr == EINTR );

    ssl_e = ERR_get_error();
    ssl_strerror = ssl_e ? ERR_error_string(ssl_e, NULL) : "-";

    if( r == 1 )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ), ctx->host, ctx->port );

        __ssl_established( ctx );
        return;
    }

    LOGD( "id:0x%llx host:%s:%s ret:%d ssl_err:%d "
          "ssl_strerror:%s errno:%d strerror:%s",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          r, e, ssl_strerror, syserr, strerror( syserr ) );

    switch( e )
    {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:

            g_skip_cb = true;
            return;

        case SSL_ERROR_SYSCALL:

            if( syserr == EAGAIN )
            {
                g_skip_cb = true;
                return;
            }

            /* fall through */

        default:

            LOGE( "id:0x%llx host:%s:%s ret:%d ssl_err:%d "
                  "ssl_strerror:%s errno:%d strerror:%s",
                  PTRID_FMT( ctx->id ), ctx->host, ctx->port,
                  r, e, ssl_strerror, syserr, strerror( syserr ) );

            __shutdown_ctx( ctx, NET_CODE_ERR_EST );
    }
}

/******************* non-SSL event callbacks **********************************/

static void __write_cb( ctx_t *ctx )
{
    wbuf_t     *wbuf;
    int         r, syserr;

    /* NOTE: it may happened because of EPOLLRDHUP */
    if( !ctx->wb_list.total )
    {
        c_assert( !ctx->wb_list.head &&
                  !ctx->wb_list.tail );

        LOGD( "id:0x%llx host:%s:%s",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port );

        __disable_write( ctx );
        return;
    }

    LL_CHECK( &ctx->wb_list, ctx->wb_list.head );
    wbuf = PTRID_GET_PTR( ctx->wb_list.head );

    B_GENERAL_CHECK( wbuf->b );

    c_assert( B_HAS_REMAINDER( wbuf->b ) );

    errno = 0;
    while( (r = send( ctx->fd,
                      B_REMAINDER_PTR( wbuf->b ),
                      B_REMAINDER_SIZE( wbuf->b ),
                      MSG_DONTWAIT )) == -1 && errno == EINTR )
        errno = 0;

    syserr = errno;

    if( r > 0 )
    {
        B_INCREASE_USED( wbuf->b, r );

        LOGD( "id:0x%llx host:%s:%s used:%lu size:%lu msg_id:%llx",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
              PTRID_FMT( wbuf->id ) );

        __handle_write_buf( ctx );
        return;
    }

    if( syserr == EAGAIN )
    {
        LOGD( "id:0x%llx host:%s:%s used:%lu size:%lu msg_id:%llx",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port,
              B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
              PTRID_FMT( wbuf->id ) );

        return;
    }

    LOGE( "id:0x%llx host:%s:%s used:%lu size:%lu "
          "msg_id:%llx errno:%d strerror:%s",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          B_USED_SIZE( wbuf->b ), B_SIZE( wbuf->b ),
          PTRID_FMT( wbuf->id ), syserr, strerror( syserr ) );

    __shutdown_ctx( ctx, NET_CODE_ERR_WRITE );
}

static void __read_cb( ctx_t *ctx )
{
    int                 r, syserr;
    char               *host = ctx->host;
    char               *port = ctx->port;

    B_GENERAL_CHECK( ctx->rb );
    c_assert( B_HAS_REMAINDER( ctx->rb ) );

    errno = 0;
    while( (r = recv( ctx->fd,
                      B_REMAINDER_PTR( ctx->rb ),
                      B_REMAINDER_SIZE( ctx->rb ),
                      MSG_DONTWAIT )) == -1 && errno == EINTR )
        errno = 0;

    syserr = errno;

    if( r == -1 )
    {
        if( syserr == EAGAIN )
        {
            LOGD( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu "
                  "errno:%d strerror:%s",
                  PTRID_FMT( ctx->id ), host, port,
                  B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ),
                  syserr, strerror( syserr ) );

            return;
        }

        LOGE( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu "
              "errno:%d strerror:%s",
              PTRID_FMT( ctx->id ), host, port,
              B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ),
              syserr, strerror( syserr ) );

        /* unexpected error occured */
        __shutdown_ctx( ctx, NET_CODE_ERR_READ );
        return;
    }

    /* NOTE: consider r == 0 as closed by peer */

    if( r > 0 )
    {
        B_INCREASE_USED( ctx->rb, r );
    }

    if( B_REMAINDER_SIZE( ctx->rb ) < B_MIN_RDBUF_REMAINDER( ctx->rb ) )
    {
        B_INCREASE_BUF( ctx->rb, READ_BUFFER_SIZE );

        LOG( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu",
             PTRID_FMT( ctx->id ), host, port,
             B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ) );
    }

    LOGD( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu",
          PTRID_FMT( ctx->id ), host, port,
          B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ) );

    if( __call_read_handler( ctx, !r ) )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ), host, port );

        return;
    }

    if( !r )
    {
        LOG( "id:0x%llx host:%s:%s rb.used:%lu rb.size:%lu",
             PTRID_FMT( ctx->id ), host, port,
             B_USED_SIZE( ctx->rb ), B_SIZE( ctx->rb ) );

        __shutdown_ctx( ctx, NET_CODE_SUCCESS );
    }
}

static void __listen_cb( ctx_t *ctx )
{
    struct sockaddr_in      peer;
    socklen_t               peer_len = sizeof(peer);
    int                     fd;
    int                     syserr;

    errno = 0;
    while( (fd = accept( ctx->fd,
                         (struct sockaddr *) &peer,
                         &peer_len )) == -1 && errno == EINTR )
        errno = 0;

    syserr = errno;

    if( fd >= 0 )
    {
        LOG( "id:0x%llx listen_port:%d new_fd:%x",
             PTRID_FMT( ctx->id ), ctx->listen_port, fd );

        if( peer_len == sizeof(peer) )
        {
            __create_accepted( ctx, fd, &peer );
        }
        else
        {
            LOGE( "id:0x%llx listen_port:%d",
                  PTRID_FMT( ctx->id ), ctx->listen_port );

            __create_accepted( ctx, fd, NULL );
        }

        g_skip_cb = true;
        return;
    }

    switch( syserr )
    {
        /* transient errors */
        case EAGAIN:
        case ECONNABORTED:
        case ENETDOWN:
        case EPROTO:
        case ENOPROTOOPT:
        case EHOSTDOWN:
        case ENONET:
        case EHOSTUNREACH:
        case EOPNOTSUPP:
        case ENETUNREACH:

            LOGD( "id:0x%llx listen_port:%d ret:%d "
                  "errno:%d strerror:%s",
                  PTRID_FMT( ctx->id ), ctx->listen_port,
                  fd, syserr, strerror( syserr ) );

            g_skip_cb = true;
            return;

        /* fatal errors */
        default:

            LOGE( "id:0x%llx listen_port:%d ret:%d "
                  "errno:%d strerror:%s",
                  PTRID_FMT( ctx->id ), ctx->listen_port,
                  fd, syserr, strerror( syserr ) );

            /* NOTE: close listen conn */
            __shutdown_ctx( ctx, NET_CODE_ERR_ACCEPT );
    }
}

static void __connect_cb( ctx_t *ctx )
{
    int                     r, syserr;

    errno = 0;
    while( (r = connect( ctx->fd,
                         (struct sockaddr *) &ctx->peer,
                         sizeof(ctx->peer) )) == -1 && errno == EINTR )
        errno = 0;

    syserr = errno;

    if( !r || syserr == EISCONN )
    {
        LOG( "id:0x%llx host:%s:%s",
             PTRID_FMT( ctx->id ), ctx->host, ctx->port );

        __established( ctx );
        return;
    }

    if( syserr == EINPROGRESS || syserr == EALREADY )
    {
        LOGD( "id:0x%llx host:%s:%s",
              PTRID_FMT( ctx->id ), ctx->host, ctx->port );

        g_skip_cb = true;
        return;
    }

    LOGE( "id:0x%llx host:%s:%s ret:%d errno:%d strerror:%s",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          r, syserr, strerror( syserr ) );

    __shutdown_ctx( ctx, NET_CODE_ERR_EST );
}

/******************* Do-scheduled functions ***********************************/

static void __call_timers( ll_t    *list,
                           ctx_t   *ctx )
{
    tmr_t              *tmr;
    tmr_t              *tmr_next;
    int                 tmr_total;
    int                 i = 0;
    int                 r;

    c_assert( list->total > 0 && list->total <= MAX_TIMERS );

    LL_CHECK( list, list->head );
    tmr = PTRID_GET_PTR( list->head );

    tmr_total = list->total;

    /* NOTE: tmr->uh_cb can create new tmrs, *
     * but don't call timers >= tmr_total    *
     * at this iteration.                    */
    while( tmr && i < tmr_total )
    {
        i++;

        c_assert( ctx || tmr->udata_id );
        c_assert( !tmr->locked && !tmr->to_delete );

        LOGD( "tmr:0x%llx timeout.tv_sec:%ld timeout.tv_usec:%ld "
              "shift.tv_sec:%ld shift.tv_usec:%ld",
              PTRID_FMT( tmr->id ), tmr->timeout.tv_sec,
              tmr->timeout.tv_usec, tmr->shift.tv_sec, tmr->shift.tv_usec );

        if( (tmr->timeout.tv_sec == G_now.tv_sec &&
             tmr->timeout.tv_usec == G_now.tv_usec) ||
            timercmp( &tmr->timeout, &G_now, < ) )
        {
            LOGD( "tmr:0x%llx", PTRID_FMT( tmr->id ) );

            tmr->locked = true;

            tmr->uh_cb( ctx ? ctx->id : 0,
                        ctx ? ctx->udata_id : 0,
                        tmr->id,
                        tmr->udata_id );

            tmr->locked = false;

            if( ctx && ctx->to_shutdown )
            {
                LOG( "tmr:0x%llx", PTRID_FMT( tmr->id ) );

                return;
            }

            /* NOTE: tmr->next can be freed and replaced after *
             * tmr->uh_cb, so save tmr->next after tmr->uh_cb  */
            LL_CHECK( list, tmr->id );
            tmr_next = PTRID_GET_PTR( tmr->next );

            if( tmr->to_delete )
            {
                if( ctx )
                    r = __del_conn_tmr( ctx, tmr->id );
                else
                    r = net_del_global_tmr( tmr->id );

                c_assert( !r );
            }
            else
            {
                timeradd( &tmr->timeout, &tmr->shift, &tmr->timeout );
            }
        }
        else
        {
            LL_CHECK( list, tmr->id );
            tmr_next = PTRID_GET_PTR( tmr->next );
        }

        tmr = tmr_next;
    }
}

static void __call_conn_timers( ctx_t *ctx )
{
    conn_id_t           prev_id = ctx->id;

    if( !ctx->tmr_list.total )
    {
        c_assert( !ctx->tmr_list.head &&
                  !ctx->tmr_list.tail );

        return;
    }

    LOGD( "id:0x%llx host:%s:%s state:%d tmr_total:%d",
          PTRID_FMT( ctx->id ), ctx->host, ctx->port,
          ctx->state->st, ctx->tmr_list.total );

    __call_timers( &ctx->tmr_list, ctx );

    c_assert( ctx->id == prev_id );

    if( ctx->to_shutdown )
    {
        LOG( "id:0x%llx host:%s:%s state:%d",
             PTRID_FMT( ctx->id ), ctx->host, ctx->port,
             ctx->state->st );

        ctx->to_shutdown = false;

        __shutdown_ctx( ctx, NET_CODE_SUCCESS );
    }
}

static void __call_global_timers()
{
    if( !g_tmr_list.total )
    {
        c_assert( !g_tmr_list.head &&
                  !g_tmr_list.tail );

        return;
    }

    LOGD( "tmr_total:%d", g_tmr_list.total );

    __call_timers( &g_tmr_list, NULL );
}

static void __do_scheduled()
{
    ctx_t              *ctx;
    int                 fd;

    LOGD( "" );

    /* NOTE: Warning - O(n) */
    for( fd = 0; fd <= NET_MAX_FD; fd++ )
    {
        ctx = &g_ctx_array[fd];

        if( !ctx->id )
            continue;

        c_assert( ctx->id && ctx->fd == fd );

        if( ctx->to_shutdown )
        {
            ctx->to_shutdown = false;

            __shutdown_ctx( ctx, NET_CODE_SUCCESS );
            continue;
        }

        __call_conn_timers( ctx );
    }

    __call_global_timers();
}

/******************* Interface functions **************************************/

/* NOTE: this function can be called without a timer callback,  *
 * it's needed to call this function before first net_make_conn */

void net_update_main_hosts( conn_id_t    conn_id,
                            ptr_id_t     conn_udata_id,
                            tmr_id_t     tmr_id,
                            ptr_id_t     tmr_udata_id )
{
    net_host_node_t        *node;
    net_host_node_t        *node_next;
    ll_t                   *list;
    struct addrinfo         hints;
    struct addrinfo        *result;
    struct addrinfo        *addr;
    struct sockaddr_in     *ai_addr;
    int                     r, i = 0;

    G_net_errno = NET_ERRNO_OK;

    c_assert( !conn_id && !conn_udata_id && tmr_udata_id );

    list = PTRID_GET_PTR( tmr_udata_id );

    LL_CHECK( list, list->head );
    node = PTRID_GET_PTR( list->head );

    while( node )
    {
        LL_CHECK( list, node->id );
        node_next = PTRID_GET_PTR( node->next );

        i++;
        c_assert( node->host.hostname &&
                  node->host.port && i <= list->total );

        addr = (struct addrinfo *) node->host.addr;

        if( addr )
        {
            ai_addr = (struct sockaddr_in *)(addr->ai_addr);

            LOGD( "tmr:0x%llx host:%s:%s ip:%s",
                  PTRID_FMT( tmr_id ),
                  node->host.hostname,
                  node->host.port,
                  inet_ntoa( ai_addr->sin_addr ) );

            freeaddrinfo( addr );

            node->host.addr = NULL;
        }

        memset( &hints, 0, sizeof(struct addrinfo) );
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        r = getaddrinfo( node->host.hostname,
                         node->host.port,
                         &hints, &result );

        if( r )
        {
            LOGE( "tmr:0x%llx host:%s:%s error:%s",
                  PTRID_FMT( tmr_id ),
                  node->host.hostname,
                  node->host.port,
                  gai_strerror( r ) );

            node = node_next;
            continue;
        }

        node->host.addr = (void *) result;
        ai_addr = (struct sockaddr_in *)(result->ai_addr);

        LOG( "tmr:0x%llx host:%s:%s ip:%s",
             PTRID_FMT( tmr_id ),
             node->host.hostname,
             node->host.port,
             inet_ntoa( ai_addr->sin_addr ) );

        node = node_next;
    }

    c_assert( i == list->total );
}

void net_update_host( net_host_t   *host )
{
    struct addrinfo     hints;
    struct addrinfo    *result;
    int                 r;

    c_assert( host );

    if( host->addr )
    {
        freeaddrinfo( host->addr );
        host->addr = NULL;
    }

    memset( &hints, 0, sizeof(struct addrinfo) );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    r = getaddrinfo( host->hostname,
                     host->port,
                     &hints, &result );

    if( r )
    {
        LOGE( "host:%s:%s error:%s",
              host->hostname,
              host->port,
              gai_strerror( r ) );

        return;
    }

    host->addr = (void *) result;
}

void net_free_host( net_host_t     *host )
{
    c_assert( host->hostname && host->port );

    free( host->hostname );
    free( host->port );

    if( host->label )
        free( host->label );

    if( host->addr )
        freeaddrinfo( host->addr );
}

net_host_t *net_get_host( ll_t     *host_list,
                          char     *label )
{
    net_host_node_t        *node;
    net_host_node_t        *node_next;
    net_host_t             *host = NULL;
    int                     i = 0;

    c_assert( host_list && label );

    G_net_errno = NET_ERRNO_OK;

    LL_CHECK( host_list, host_list->head );
    node = PTRID_GET_PTR( host_list->head );

    while( node )
    {
        LL_CHECK( host_list, node->id );
        node_next = PTRID_GET_PTR( node->next );

        i++;
        c_assert( node->host.hostname &&
                  node->host.port &&
                  i <= host_list->total );

        if( node->host.label &&
            !strcasecmp( node->host.label, label ) )
        {
            host = &node->host;
            break;
        }

        node = node_next;
    }

    return host;
}

/*  It can be used for internal timers and user level timers.  *
 *  Example of user level global timers: to update main_hosts. */

tmr_id_t net_make_global_tmr( ptr_id_t           udata_id,
                              net_tmr_cb_t       cb,
                              struct timeval    *timeout )
{
    tmr_t                  *tmr;

    G_net_errno = NET_ERRNO_OK;

    if( !udata_id || !cb || !timeout )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return 0;
    }

    if( g_tmr_list.total > MAX_TIMERS )
    {
        LOGE( "udata_id:0x%llx", PTRID_FMT( udata_id ) );

        G_net_errno = NET_ERRNO_TMR_MAX;
        return 0;
    }

    tmr = __make_timer( cb, timeout );

    LL_ADD_NODE( &g_tmr_list, tmr );

    tmr->udata_id = udata_id;

    LOG( "tmr:0x%llx udata_id:0x%llx timeout.tv_sec:%ld timeout.tv_usec:%ld "
         "shift.tv_sec:%ld shift.tv_usec:%ld",
         PTRID_FMT( tmr->id ), PTRID_FMT( udata_id ),
         tmr->timeout.tv_sec, tmr->timeout.tv_usec,
         tmr->shift.tv_sec, tmr->shift.tv_usec );

    return tmr->id;
}

int net_del_global_tmr( tmr_id_t tmr_id )
{
    tmr_t                  *tmr;

    G_net_errno = NET_ERRNO_OK;

    if( !tmr_id )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return -1;
    }

    tmr = PTRID_GET_PTR( tmr_id );

    LOG( "tmr:0x%llx", PTRID_FMT( tmr->id ) );

    return __del_timer( tmr, &g_tmr_list );
}

tmr_id_t net_make_conn_tmr( conn_id_t        conn_id,
                            ptr_id_t         udata_id,
                            net_tmr_cb_t     cb,
                            struct timeval  *timeout )
{
    ctx_t                  *ctx;

    G_net_errno = NET_ERRNO_OK;

    if( !conn_id || !cb || !timeout )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return 0;
    }

    ctx = __get_ctx( conn_id );

    if( !ctx->state || ctx->is_in_dup_udata ||
        ctx->state->st == S_SSL_SHUTDOWN ||
        ctx->state->st == S_LISTENING ||
        ctx->state->st == S_SSL_ACCEPTING ||
        ctx->to_shutdown || ctx->is_in_destroying ||
        ctx->flush_and_close )
    {
        LOGE( "id:0x%llx state:%d",
              PTRID_FMT( conn_id ), ctx->state->st );

        G_net_errno = NET_ERRNO_CONN_WRONG_STATE;
        return 0;
    }

    return __make_conn_tmr( ctx, udata_id, cb, timeout );
}

int net_del_conn_tmr( conn_id_t       conn_id,
                      tmr_id_t        tmr_id )
{
    ctx_t                  *ctx;

    G_net_errno = NET_ERRNO_OK;

    if( !conn_id || !tmr_id )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return -1;
    }

    ctx = __get_ctx( conn_id );

    if( !ctx->state || ctx->is_in_dup_udata ||
        ctx->state->st == S_SSL_SHUTDOWN ||
        ctx->state->st == S_LISTENING ||
        ctx->state->st == S_SSL_ACCEPTING ||
        ctx->to_shutdown || ctx->is_in_destroying ||
        ctx->flush_and_close )
    {
        LOGE( "id:0x%llx state:%d",
              PTRID_FMT( conn_id ), ctx->state->st );

        G_net_errno = NET_ERRNO_CONN_WRONG_STATE;
        return -1;
    }

    return __del_conn_tmr( ctx, tmr_id );
}

int net_post_data( conn_id_t        conn_id,
                   char            *data,
                   unsigned long    len,
                   bool             flush_and_close )
{
    ctx_t                  *ctx;
    wbuf_t                 *wbuf;

    G_net_errno = NET_ERRNO_OK;

    if( !conn_id || !data || !len )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return -1;
    }

    ctx = __get_ctx( conn_id );

    if( !ctx->state || ctx->is_in_dup_udata ||
        (ctx->state->st != S_ESTABLISHED &&
         ctx->state->st != S_SSL_ESTABLISHED) ||
        ctx->to_shutdown || ctx->is_in_destroying ||
        ctx->flush_and_close )
    {
        LOGE( "id:0x%llx state:%d",
              PTRID_FMT( conn_id ), ctx->state->st );

        G_net_errno = NET_ERRNO_CONN_WRONG_STATE;
        return -1;
    }

    c_assert( !ctx->state_tmr_id );

    if( flush_and_close )
    {
        /* NOTE: since this moment, no app timers is needed */
        __cleanup_timers( ctx );

        ctx->state_tmr_id = __make_conn_tmr( ctx, 0,
                                             __flush_and_close_timeout_cb,
                                             cfg_net_flush_and_close_timeout );

        c_assert( ctx->state_tmr_id );

        ctx->flush_and_close = true;
    }

    wbuf = malloc( sizeof(wbuf_t) );
    memset( wbuf, 0, sizeof(wbuf_t) );

    B_ALLOC_FILL( wbuf->b, data, len );

    LL_ADD_NODE( &ctx->wb_list, wbuf );

    __enable_write( ctx );

    LOG( "id:0x%llx host:%s:%s size:%lu msg_id:%llx",
         PTRID_FMT( ctx->id ), ctx->host, ctx->port,
         B_SIZE( wbuf->b ), PTRID_FMT( wbuf->id ) );

    return 0;
}

conn_id_t net_make_listen( net_r_uh_t       child_r_uh_cb,
                           net_est_uh_t     child_est_uh_cb,
                           net_clo_uh_t     child_clo_uh_cb,
                           net_dup_udata_t  dup_udata_cb,
                           net_clo_uh_t     clo_uh_cb,
                           ptr_id_t         udata_id,
                           int              port,
                           bool             use_ssl )
{
    ctx_t                  *ctx;
    int                     fd;
    int                     r;

    G_net_errno = NET_ERRNO_OK;

    if( !child_r_uh_cb || !child_est_uh_cb ||
        !child_clo_uh_cb || !dup_udata_cb ||
        !clo_uh_cb || !udata_id || port <= 0 )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return 0;
    }

    fd = __create_socket();
    if( fd == -1 )
    {
        LOGE( "listen_port:%d use_ssl:%d", port, use_ssl );

        if( G_net_errno == NET_ERRNO_OK )
            G_net_errno = NET_ERRNO_GENERAL_ERR;

        return 0;
    }

    ctx = __init_new_ctx( fd );
    c_assert( ctx );

    ctx->serv.sin_family = AF_INET;
    ctx->serv.sin_addr.s_addr = INADDR_ANY;
    ctx->serv.sin_port = htons(port);

    r = bind( ctx->fd,
              (struct sockaddr *) &ctx->serv,
              sizeof(ctx->serv) );

    if( r == -1 )
    {
        LOGE( "listen_port:%d use_ssl:%d errno:%d strerror:%s",
              port, use_ssl, errno, strerror( errno ) );

        PROPER_CLOSE_FD( ctx->fd );
        __cancel_ctx( ctx );

        G_net_errno = NET_ERRNO_GENERAL_ERR;
        return 0;
    }

    r = listen( ctx->fd, BACKLOG );

    if( r == -1 )
    {
        LOGE( "listen_port:%d use_ssl:%d errno:%d strerror:%s",
              port, use_ssl, errno, strerror( errno ) );

        PROPER_CLOSE_FD( ctx->fd );
        __cancel_ctx( ctx );

        G_net_errno = NET_ERRNO_GENERAL_ERR;
        return 0;
    }

    ctx->child_use_ssl = use_ssl;

    ctx->host[0] = '\0';
    ctx->port[0] = '\0';

    ctx->listen_port = port;

    ctx->child_r_uh_cb = child_r_uh_cb;
    ctx->child_est_uh_cb = child_est_uh_cb;
    ctx->child_clo_uh_cb = child_clo_uh_cb;

    ctx->dup_udata_cb = dup_udata_cb;

    ctx->clo_uh_cb = clo_uh_cb;

    ctx->udata_id = udata_id;

    ctx->dirn = D_LISTEN;

    LOG( "id:0x%llx fd:%x listen_port:%d use_ssl:%d",
         PTRID_FMT( ctx->id ), ctx->fd,
         ctx->listen_port, use_ssl );

    __start_listen( ctx );

    return ctx->id;
}

conn_id_t net_make_conn( net_host_t    *host,
                         net_r_uh_t     r_uh_cb,
                         net_est_uh_t   est_uh_cb,
                         net_clo_uh_t   clo_uh_cb,
                         ptr_id_t       udata_id )
{
    ctx_t                  *ctx;
    SSL                    *ssl;
    int                     fd;

    G_net_errno = NET_ERRNO_OK;

    if( !host || !host->hostname || !host->port )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return 0;
    }

    if( !host->addr )
    {
        LOGE( "host:%s:%s", host->hostname, host->port );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return 0;
    }

    if( !r_uh_cb || !est_uh_cb || !clo_uh_cb || !udata_id )
    {
        LOGE( "host:%s:%s", host->hostname, host->port );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return 0;
    }

    fd = __create_socket();
    if( fd == -1 )
    {
        LOGE( "host:%s:%s", host->hostname, host->port );

        if( G_net_errno == NET_ERRNO_OK )
            G_net_errno = NET_ERRNO_GENERAL_ERR;

        return 0;
    }

    ctx = __init_new_ctx( fd );
    c_assert( ctx );

    if( host->use_ssl )
    {
        ssl = SSL_new( g_ssl_client_ctx );
        c_assert( ssl );

        SSL_set_fd( ssl, ctx->fd );
        SSL_set_connect_state( ssl ); /* means doing as client */

        ctx->ssl = ssl;
    }

    strncpy( ctx->host, host->hostname, sizeof(ctx->host) );
    strncpy( ctx->port, host->port, sizeof(ctx->port) );
    ctx->host[sizeof(ctx->host) - 1] = '\0';
    ctx->port[sizeof(ctx->port) - 1] = '\0';

    ctx->peer = *(struct sockaddr_in *)
                (((struct addrinfo *)host->addr)->ai_addr);

    B_ALLOC( ctx->rb, READ_BUFFER_SIZE );

    ctx->r_uh_cb = r_uh_cb;
    ctx->est_uh_cb = est_uh_cb;
    ctx->clo_uh_cb = clo_uh_cb;

    ctx->udata_id = udata_id;

    ctx->dirn = D_OUTGOING;

    LOG( "id:0x%llx udata_id:0x%llx fd:%x "
         "host:%s:%s use_ssl:%d",
         PTRID_FMT( ctx->id ), PTRID_FMT( udata_id ),
         ctx->fd, ctx->host, ctx->port, host->use_ssl );

    __start_connect( ctx );

    return ctx->id;
}

int net_shutdown_conn( conn_id_t    conn_id,
                       bool         flush_and_close )
{
    ctx_t                  *ctx;

    G_net_errno = NET_ERRNO_OK;

    if( !conn_id )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return -1;
    }

    ctx = __get_ctx( conn_id );

    if( !ctx->state || ctx->is_in_dup_udata ||
        ctx->state->st == S_SSL_SHUTDOWN ||
        ctx->to_shutdown || ctx->is_in_destroying ||
        ctx->flush_and_close )
    {
        if( ctx->flush_and_close )
        {
            LOG( "id:0x%llx", PTRID_FMT( conn_id ) );
        }
        else
        {
            LOGE( "id:0x%llx state:%d",
                  PTRID_FMT( conn_id ),
                  ctx->state ? ctx->state->st : 0 );
        }

        G_net_errno = NET_ERRNO_CONN_WRONG_STATE;
        return -1;
    }

    if( flush_and_close )
    {
        /* NOTE: since this moment, no app timers is needed */
        __cleanup_timers( ctx );

        ctx->state_tmr_id = __make_conn_tmr( ctx, 0,
                                             __flush_and_close_timeout_cb,
                                             cfg_net_flush_and_close_timeout );

        c_assert( ctx->state_tmr_id );

        ctx->flush_and_close = true;
    }
    else
        ctx->to_shutdown = true;

    LOG( "id:0x%llx flush_and_close:%d",
         PTRID_FMT( conn_id ), flush_and_close );

    return 0;
}

net_state_t net_is_est_conn( conn_id_t conn_id )
{
    ctx_t                  *ctx;

    G_net_errno = NET_ERRNO_OK;

    if( !conn_id )
    {
        LOGE( "" );

        G_net_errno = NET_ERRNO_WRONG_PARAMS;
        return NET_STATE_ERROR;
    }

    ctx = __get_ctx( conn_id );

    if( !ctx->state || ctx->is_in_dup_udata )
    {
        LOGE( "id:0x%llx", PTRID_FMT( conn_id ) );

        G_net_errno = NET_ERRNO_CONN_WRONG_STATE;
        return NET_STATE_ERROR;
    }

    if( ctx->to_shutdown || ctx->is_in_destroying )
    {
        LOGD( "id:0x%llx state:%d",
              PTRID_FMT( conn_id ), ctx->state->st );

        return NET_STATE_NOT_EST;
    }

    if( ctx->state->st == S_ESTABLISHED     ||
        ctx->state->st == S_SSL_ESTABLISHED ||
        ctx->state->st == S_LISTENING )
    {
        if( ctx->flush_and_close )
        {
            LOGD( "id:0x%llx state:%d",
                  PTRID_FMT( conn_id ), ctx->state->st );

            return NET_STATE_FLUSH_AND_CLOSE;
        }
        else
        {
            LOGD( "id:0x%llx state:%d",
                  PTRID_FMT( conn_id ), ctx->state->st );

            return NET_STATE_EST;
        }
    }

    LOGD( "id:0x%llx state:%d",
          PTRID_FMT( conn_id ), ctx->state->st );

    return NET_STATE_NOT_EST;
}

/******************* Init network & Main loop *********************************/

static void __default_config_init()
{
    if( !cfg_net_cert_test_file )
    {
        cfg_net_cert_test_file = strdup( "core/server_test.crt" );
    }

    if( !cfg_net_key_test_file )
    {
        cfg_net_key_test_file = strdup( "core/server_test.key" );
    }

    if( !cfg_net_ssl_shutdown_timeout )
    {
        cfg_net_ssl_shutdown_timeout = malloc( sizeof(struct timeval) );

        cfg_net_ssl_shutdown_timeout->tv_sec = 1;
        cfg_net_ssl_shutdown_timeout->tv_usec = 0;
    }

    if( !cfg_net_ssl_establish_timeout )
    {
        cfg_net_ssl_establish_timeout = malloc( sizeof(struct timeval) );

        cfg_net_ssl_establish_timeout->tv_sec = 1;
        cfg_net_ssl_establish_timeout->tv_usec = 0;
    }

    if( !cfg_net_ssl_accept_timeout )
    {
        cfg_net_ssl_accept_timeout = malloc( sizeof(struct timeval) );

        cfg_net_ssl_accept_timeout->tv_sec = 1;
        cfg_net_ssl_accept_timeout->tv_usec = 0;
    }

    if( !cfg_net_establish_timeout )
    {
        cfg_net_establish_timeout = malloc( sizeof(struct timeval) );

        cfg_net_establish_timeout->tv_sec = 1;
        cfg_net_establish_timeout->tv_usec = 0;
    }

    if( !cfg_net_flush_and_close_timeout )
    {
        cfg_net_flush_and_close_timeout = malloc( sizeof(struct timeval) );

        cfg_net_flush_and_close_timeout->tv_sec = 1;
        cfg_net_flush_and_close_timeout->tv_usec = 0;
    }
}

void net_init()
{
    int         r;

    G_net_errno = NET_ERRNO_OK;

    __default_config_init();

    memset( g_ctx_array, 0, sizeof(g_ctx_array) );

    g_epollfd = epoll_create( NET_MAX_FD + 1 );
    c_assert( g_epollfd > 0 );

    SSL_library_init();

    g_ssl_client_ctx = SSL_CTX_new( SSLv23_client_method() );
    c_assert( g_ssl_client_ctx );

    g_ssl_server_ctx = SSL_CTX_new( SSLv23_server_method() );
    c_assert( g_ssl_server_ctx );

    if( cfg_net_key_file )
    {
        c_assert( cfg_net_cert_file );

        r = SSL_CTX_use_certificate_file( g_ssl_server_ctx,
                                          cfg_net_cert_file,
                                          SSL_FILETYPE_PEM );

        c_assert( r == 1 );

        r = SSL_CTX_use_PrivateKey_file( g_ssl_server_ctx,
                                         cfg_net_key_file,
                                         SSL_FILETYPE_PEM );

        c_assert( r == 1 );
    }
    else
    {
        c_assert( cfg_net_key_test_file &&
                  cfg_net_cert_test_file );

        r = SSL_CTX_use_certificate_file( g_ssl_server_ctx,
                                          cfg_net_cert_test_file,
                                          SSL_FILETYPE_PEM );

        c_assert( r == 1 );

        r = SSL_CTX_use_PrivateKey_file( g_ssl_server_ctx,
                                         cfg_net_key_test_file,
                                         SSL_FILETYPE_PEM );

        c_assert( r == 1 );

        char        warn_buf[1024];

        snprintf( warn_buf, sizeof(warn_buf),
                "***********************************************************\n"
                "* WARNING: A PRODUCTION PRIVATE KEY FILE WAS NOT FOUND!!! *\n"
                "*               !!!USING THROWAWAY PRIVATE KEY!!!         *\n"
                "***********************************************************"
                );

        printf( "%s\n", warn_buf );

        /* NOTE: duplicate warning to log with ERROR log level */
        LOGE( "\n%s", warn_buf );
    }

    r = SSL_CTX_check_private_key( g_ssl_server_ctx );
    c_assert( r == 1 );

    LOG( "epollfd:%x", g_epollfd );
}

void net_main_loop()
{
    ctx_t              *ctx;
    int                 nfds;
    struct epoll_event  ready_events[MAX_EVENTS];
    uint32_t            ev;
    int                 fd;
    struct timeval      iter_time_diff;
    int                 i;
    int                 r;

    /* NOTE: need fresh G_now */
    r = gettimeofday( &G_now, NULL );
    assert( !r ); /* NOTE: real assert here */

    g_iter_time = G_now;

    while( true )
    {
        nfds = epoll_wait( g_epollfd, ready_events,
                           sizeof(ready_events)/sizeof(struct epoll_event),
                           WAIT_TIMEOUT );

        if( nfds < 0 )
        {
            if( errno == EINTR )
            {
                LOGD( "" );
                continue;
            }

            LOGE( "errno:%d strerror:%s", errno, strerror( errno ) );
            return;
        }

        LOGD( "nfds:%d ctx_total:%d", nfds, g_ctx_total );

        for( i = 0; i < nfds; i++ )
        {
            fd = ready_events[i].data.fd;
            ev = ready_events[i].events;

            c_assert( fd >= 0 && fd <= NET_MAX_FD );

            ctx = &g_ctx_array[fd];

            c_assert( ctx->id && ctx->fd == fd );

            LOGD( "id:0x%llx host:%s:%s state:%d fd:%x ev:0x%x "
                  "EPOLLIN:%d EPOLLOUT:%d EPOLLRDHUP:%d EPOLLPRI:%d "
                  "EPOLLERR:%d EPOLLHUP:%d EPOLLET:%d EPOLLONESHOT:%d",
                  PTRID_FMT( ctx->id ), ctx->host, ctx->port,
                  ctx->state->st, fd, ev,
                  ev & EPOLLIN, ev & EPOLLOUT, ev & EPOLLRDHUP, ev & EPOLLPRI,
                  ev & EPOLLERR,ev & EPOLLHUP, ev & EPOLLET, ev & EPOLLONESHOT);

            g_skip_cb = false;

            if( (ev & ( EPOLLHUP | EPOLLRDHUP | EPOLLERR )) ||
                (ev & EPOLLIN) )
            {
                if( !ctx->to_shutdown )
                    ctx->state->r_cb( ctx );
            }

            if( (ev & ( EPOLLHUP | EPOLLRDHUP | EPOLLERR )) ||
                (ev & EPOLLOUT) )
            {
                if( ctx->id && !ctx->to_shutdown && !g_skip_cb )
                    ctx->state->w_cb( ctx );
            }
        }

        /* NOTE: need fresh G_now */
        r = gettimeofday( &G_now, NULL );
        assert( !r ); /* NOTE: real assert here */

        __do_scheduled();

        /* NOTE: need fresh G_now */
        r = gettimeofday( &G_now, NULL );
        assert( !r ); /* NOTE: real assert here */

        timersub( &G_now, &g_iter_time, &iter_time_diff );
        g_iter_time = G_now;

        LOGD( "iter_time_diff:%ld:%ld",
              iter_time_diff.tv_sec, iter_time_diff.tv_usec );
    }
}

