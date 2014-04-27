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
#include "hash_table.h"
#include "hash_table_internal.h"

/******************* Mics functions *******************************************/

static ll_t *__find_elt( hash_pair_t  **pair,
                         hash_table_t  *table,
                         char          *key,
                         int            key_len )
{
    ll_t           *ll;
    hash_pair_t    *pr;
    int             ndx;

    *pair = NULL;

    ndx = xcrc32( (unsigned char *) key,
                  key_len, CRC_INIT ) % table->n;

    ll = &table->t[ndx];

    if( ll->total )
    {
        LL_CHECK( ll, ll->head );

        pr = PTRID_GET_PTR( ll->head );

        while( pr )
        {
            c_assert( pr->key );

            if( !strncmp( pr->key, key, key_len ) )
            {
                *pair = pr;
                break;
            }

            pr = PTRID_GET_PTR( pr->next );
        }
    }

    return ll;
}

/******************* Interface functions **************************************/

hash_table_t *hash_table_create( int    n_elts )
{
    hash_table_t   *table;
    int             size;

    c_assert( n_elts > 0 );

    table = malloc( sizeof(hash_table_t) );
    memset( table, 0, sizeof(hash_table_t) );

    table->n = n_elts;

    size = sizeof(ll_t) * n_elts;

    table->t = malloc( size );
    memset( table->t, 0, size );

    return table;
}

void hash_table_destroy( hash_table_t  *table )
{
    hash_pair_t    *pair;
    hash_pair_t    *pair_next;
    ll_t           *ll;
    int             i;

    c_assert( table && table->n > 0 && table->t );

    for( i = 0; i < table->n; i++ )
    {
        ll = &table->t[i];

        if( !ll->total )
            continue;

        LL_CHECK( ll, ll->head );

        pair = PTRID_GET_PTR( ll->head );

        while( pair )
        {
            pair_next = PTRID_GET_PTR( pair->next );

            LL_DEL_NODE( ll, pair->id );

            c_assert( pair->key );
            free( pair->key );

            if( pair->val )
                free( pair->val );

            memset( pair, 0, sizeof(hash_pair_t) );
            free( pair );

            pair = pair_next;
        }
    }

    free( table->t );

    memset( table, 0, sizeof(hash_table_t) );
    free( table );
}

bool hash_table_set_pair( hash_table_t     *table,
                          char             *key,
                          int               key_len,
                          char             *val,
                          int               val_len )
{
    ll_t           *ll;
    hash_pair_t    *pair;
    bool            is_rewritten = false;

    c_assert( table && table->n > 0 &&
              table->t && key && key_len > 0 );

    c_assert( (!val && !val_len) || (val && val_len > 0) );

    ll = __find_elt( &pair, table, key, key_len );

    if( pair )
    {
        if( pair->val )
        {
            free( pair->val );

            is_rewritten = true;
        }

        if( val )
            pair->val = strndup( val, val_len );
        else
            pair->val = NULL;
    }
    else
    {
        pair = malloc( sizeof(hash_pair_t) );
        memset( pair, 0, sizeof(hash_pair_t) );

        pair->key = strndup( key, key_len );

        if( val )
            pair->val = strndup( val, val_len );

        LL_ADD_NODE( ll, pair );
    }

    return is_rewritten;
}

char *hash_table_get_val( hash_table_t     *table,
                          char             *key,
                          int               key_len )
{
    hash_pair_t    *pair;

    c_assert( table && table->n > 0 &&
              table->t && key && key_len > 0 );

    __find_elt( &pair, table, key, key_len );

    if( !pair )
        return NULL;

    return pair->val;
}

