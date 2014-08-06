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
    int                     n;
    ll_t                   *t;
    int                     val_count;
} hash_table_t;

typedef struct {
    hash_table_t           *table;
    int                     ndx;
    ptr_id_t                elt_id;
} hash_table_iter_t;

hash_table_t   *hash_table_create( int              n_elts );

void            hash_table_destroy( hash_table_t   *table,
                                    void         ( *destr_cb )( void *val ) );

void           *hash_table_set_pair( hash_table_t  *table,
                                     char          *key,
                                     int            key_len,
                                     void          *val );

void           *hash_table_get_val( hash_table_t   *table,
                                    char           *key,
                                    int             key_len );

void           *hash_table_get_next_val( hash_table_iter_t *iter );

hash_table_iter_t *hash_table_create_iter( hash_table_t *table );

