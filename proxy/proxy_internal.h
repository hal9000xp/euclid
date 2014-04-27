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
    conn_id_t       conn_id;
    ptr_id_t        udata_id;

    ptr_id_t        conn_in_id;

    net_host_t     *host;
    ptr_id_t        msg_id;

    char           *pending_data;
    int             pending_data_len;
    bool            pending_data_sent;

    bool            est_reply_sent;
} conn_raw_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t        id;
    ptr_id_t        prev;
    ptr_id_t        next;

    http_id_t       http_id;
    ptr_id_t        udata_id;

    ptr_id_t        conn_in_id;

    http_msg_t     *http_msg;
    net_host_t     *host;
    ptr_id_t        msg_id;

    bool            origin_connection_close;
    bool            sent_reply;
} conn_out_t;

typedef struct {
    http_id_t       http_id;
    ptr_id_t        udata_id;

    ll_t            conn_out_list;

    ptr_id_t        conn_raw_id;

    int             port;
    bool            is_ssl;

    bool            tunneling_mode;
} conn_in_t;

typedef struct {
    http_id_t       http_id;
    ptr_id_t        udata_id;

    int             port;
    bool            is_ssl;
} listen_t;

