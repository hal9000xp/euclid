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

typedef struct {
    char           *hostname;
    char           *port;
    bool            use_ssl;
    char           *label;
    void           *addr;
} net_host_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t        id;
    ptr_id_t        prev;
    ptr_id_t        next;

    net_host_t      host;
} net_host_node_t;

/* codes for clo_uh_cb or logging */
typedef enum {
    NET_CODE_SUCCESS = 0,
    NET_CODE_ERR_EST,
    NET_CODE_ERR_SHUT,
    NET_CODE_ERR_ACCEPT,
    NET_CODE_ERR_WRITE,
    NET_CODE_ERR_READ
} net_code_t;

typedef enum {
    NET_ERRNO_OK = 0,
    NET_ERRNO_WRONG_PARAMS,
    NET_ERRNO_CONN_MAX,
    NET_ERRNO_TMR_MAX,
    NET_ERRNO_CONN_WRONG_STATE,
    NET_ERRNO_GENERAL_ERR
} net_errno_t;

typedef enum {
    NET_STATE_ERROR = -1,
    NET_STATE_NOT_EST,
    NET_STATE_EST,
    NET_STATE_FLUSH_AND_CLOSE
} net_state_t;

typedef ptr_id_t    conn_id_t;
typedef ptr_id_t    tmr_id_t;

/* uh == user handler */
typedef int  ( *net_r_uh_t )(   conn_id_t    conn_id,
                                ptr_id_t     udata_id,
                                char        *buf,
                                int          len,
                                bool         is_closed );

typedef void ( *net_est_uh_t )( conn_id_t    conn_id,
                                ptr_id_t     udata_id );

typedef void ( *net_clo_uh_t )( conn_id_t    conn_id,
                                ptr_id_t     udata_id,
                                int          code );

typedef void ( *net_tmr_cb_t )( conn_id_t    conn_id,
                                ptr_id_t     conn_udata_id,
                                tmr_id_t     tmr_id,
                                ptr_id_t     tmr_udata_id );

typedef ptr_id_t ( *net_dup_udata_t )( conn_id_t    conn_id,
                                       ptr_id_t     udata_id );

unsigned    G_net_errno;

void        net_init();
void        net_main_loop();

conn_id_t   net_make_conn( net_host_t          *host,
                           net_r_uh_t           r_uh_cb,
                           net_est_uh_t         est_uh_cb,
                           net_clo_uh_t         clo_uh_cb,
                           ptr_id_t             udata_id );

conn_id_t   net_make_listen( net_r_uh_t         child_r_uh_cb,
                             net_est_uh_t       child_est_uh_cb,
                             net_clo_uh_t       child_clo_uh_cb,
                             net_dup_udata_t    dup_udata_cb,
                             net_clo_uh_t       clo_uh_cb,
                             ptr_id_t           udata_id,
                             int                port,
                             bool               use_ssl );

int         net_post_data( conn_id_t            conn_id,
                           char                *data,
                           unsigned long        len,
                           bool                 flush_and_close );

tmr_id_t    net_make_conn_tmr( conn_id_t            conn_id,
                               ptr_id_t             udata_id,
                               net_tmr_cb_t         cb,
                               struct timeval      *timeout );

tmr_id_t    net_make_global_tmr( ptr_id_t           udata_id,
                                 net_tmr_cb_t       cb,
                                 struct timeval    *timeout );

int         net_del_conn_tmr( conn_id_t     conn_id,
                              tmr_id_t      tmr_id );

int         net_del_global_tmr( tmr_id_t    tmr_id );

int         net_shutdown_conn( conn_id_t    conn_id,
                               bool         flush_and_close );

net_state_t net_is_est_conn( conn_id_t      conn_id );

void        net_update_main_hosts( conn_id_t    conn_id,
                                   ptr_id_t     conn_udata_id,
                                   tmr_id_t     tmr_id,
                                   ptr_id_t     tmr_udata_id );

void        net_update_host( net_host_t    *host );

void        net_free_host( net_host_t      *host );

net_host_t *net_get_host( ll_t             *host_list,
                          char             *label );

/* NOTE: Normally we don't need many connections, so using O(n) */
/* NOTE: Some fds may be allocated by fopen or is still in ssl shutdown */
#ifdef  DEBUGMANYCONNS

#define NET_MAX_FD          4096

#else

#define NET_MAX_FD          128

#endif

/* NOTE: the full domain names are always <= 253 */
#define MAX_DOMAIN_LEN      256

/* NOTE: a port number always <= 65535 */
#define MAX_PORT_STR_LEN    6

extern char                *cfg_net_cert_file;
extern char                *cfg_net_key_file;

extern char                *cfg_net_cert_test_file;
extern char                *cfg_net_key_test_file;

extern struct timeval      *cfg_net_ssl_shutdown_timeout;
extern struct timeval      *cfg_net_ssl_establish_timeout;
extern struct timeval      *cfg_net_ssl_accept_timeout;
extern struct timeval      *cfg_net_establish_timeout;
extern struct timeval      *cfg_net_flush_and_close_timeout;

