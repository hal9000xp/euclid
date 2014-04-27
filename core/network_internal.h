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

#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

typedef struct ctx_s        ctx_t;
typedef struct wbuf_s       wbuf_t;
typedef struct tmr_s        tmr_t;

typedef struct {
    char               *buf;
    unsigned long       used;
    unsigned long       len;
} buf_t;

struct wbuf_s {
    /* ll_node_t */
    ptr_id_t            id;
    ptr_id_t            prev;
    ptr_id_t            next;

    buf_t               b;
    int                 tries;
};

struct tmr_s {
    /* ll_node_t */
    ptr_id_t            id;
    ptr_id_t            prev;
    ptr_id_t            next;

    net_tmr_cb_t        uh_cb;
    struct timeval      timeout;
    struct timeval      shift;

    ptr_id_t            udata_id;

    bool                locked;
    bool                to_delete;
};

typedef struct {
    int                 st;
    int                 ssl_rw_st;
    void             ( *r_cb )( ctx_t *ctx );
    void             ( *w_cb )( ctx_t *ctx );
} state_t;

/* ctx == connection context (just context) *
 * uh  == user handler                      */
struct ctx_s {
    int                 fd;
    uint32_t            ev; /* EPOLLIN, EPOLLOUT etc */
    conn_id_t           id;

    char                host[MAX_DOMAIN_LEN];
    char                port[MAX_PORT_STR_LEN];

    int                 listen_port;

    struct sockaddr_in  peer;
    struct sockaddr_in  serv;

    SSL                *ssl;

    bool                child_use_ssl;

    buf_t               rb;
    ll_t                wb_list;

    net_r_uh_t          r_uh_cb;
    net_est_uh_t        est_uh_cb;
    net_clo_uh_t        clo_uh_cb;

    net_r_uh_t          child_r_uh_cb;
    net_est_uh_t        child_est_uh_cb;
    net_clo_uh_t        child_clo_uh_cb;

    net_dup_udata_t     dup_udata_cb;

    state_t            *state;
    tmr_id_t            state_tmr_id; 

    ptr_id_t            udata_id;

    ll_t                tmr_list;

    int                 dirn;

    bool                to_shutdown;
    bool                is_clo_uh_done;
    bool                is_in_destroying;
    bool                is_in_dup_udata;
    bool                flush_and_close;
    bool                is_shut_wr_done;
};

/* NOTE: EPOLLRDHUP is only available since Linux 2.6.17 */
#ifndef EPOLLRDHUP
#define EPOLLRDHUP                  EPOLLHUP
#endif

#define MAX_EVENTS                  16
#define MAX_TIMERS                  1024
#define MAX_WRITE_TRIES             1024

#define BACKLOG                     10

#define WAIT_TIMEOUT                10

/* D == Direction of connection */

#define D_LISTEN                    0
#define D_OUTGOING                  1
#define D_INCOMING                  2

/* R_WANT_W == SSL_read want write  *
 * W_WANT_R == SSL_write want read  */

#define SSL_R_WANT_W                1
#define SSL_W_WANT_R                2

#define PROPER_CLOSE_FD( fd )       do {                                \
                                        errno = 0;                      \
                                        while( close( fd ) == -1 &&     \
                                               errno == EINTR )         \
                                            errno = 0;                  \
                                    } while( false )

/* B_ macroses for buf_t */
#define B_GENERAL_CHECK( b )        do {                                \
                                        c_assert( (b).buf         &&    \
                                                  (b).len > 0     &&    \
                                                  (b).used >= 0   &&    \
                                                  (b).used <= (b).len );\
                                    } while( false )

#define B_SIZE( b )                 ( (b).len )

#define B_HAS_USED( b )             ( (b).used > 0 )
#define B_HAS_REMAINDER( b )        ( (b).used < (b).len )

#define B_USED_PTR( b )             ( (b).buf )
#define B_USED_SIZE( b )            ( (b).used )

#define B_REMAINDER_PTR( b )        ( (b).buf + (b).used )
#define B_REMAINDER_SIZE( b )       ( (b).len - (b).used )

#define B_INCREASE_USED( b, n )     do {                                \
                                        B_GENERAL_CHECK( b );           \
                                        c_assert( (n) > 0 );            \
                                        (b).used += (n);                \
                                        c_assert( (b).used <= (b).len );\
                                    } while( false )

/* TODO: memmove() is a slow solution */
#define B_CUT_USED( b, n )          do {                                \
                                        B_GENERAL_CHECK( b );           \
                                        c_assert( (n) > 0 &&            \
                                                  (n) <= (b).used );    \
                                        memmove( (b).buf, (b).buf + (n),\
                                                 (b).used - (n) );      \
                                        (b).used -= (n);                \
                                    } while( false )

/* TODO: may be do malloc and realloc through mem manager? */
#define B_INCREASE_BUF( b, n )      do {                                \
                                        B_GENERAL_CHECK( b );           \
                                        c_assert( (n) > 0 );            \
                                        (b).len += (n);                 \
                                        (b).buf = realloc( (b).buf,     \
                                                           (b).len );   \
                                        B_GENERAL_CHECK( b );           \
                                    } while( false )

#define B_ALLOC( b, n )             do {                                \
                                        c_assert( !(b).buf && (n) > 0 );\
                                        (b).len = (n);                  \
                                        (b).used = 0;                   \
                                        (b).buf = malloc( (b).len );    \
                                    } while( false )

#define B_FREE( b )                 do {                                \
                                        B_GENERAL_CHECK( b );           \
                                        free( (b).buf );                \
                                        (b).len = 0;                    \
                                        (b).used = 0;                   \
                                    } while( false )

#define B_ALLOC_FILL( b, data, n )  do {                                \
                                        B_ALLOC( b, n );                \
                                        memcpy( (b).buf, data, (n) );   \
                                    } while( false )

#define B_MIN_RDBUF_REMAINDER( b )  ( B_SIZE( b ) / 10 )


#define READ_BUFFER_SIZE            131072

