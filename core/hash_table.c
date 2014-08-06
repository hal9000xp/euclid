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

static ll_t *__find_elt( hash_pair_t      **pair,
                         hash_table_t      *table,
                         char              *key,
                         int                key_len )
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

void hash_table_destroy( hash_table_t      *table,
                         void            ( *destr_cb )( void *val ) )
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

            if( pair->val && destr_cb )
                destr_cb( pair->val );

            memset( pair, 0, sizeof(hash_pair_t) );
            free( pair );

            pair = pair_next;
        }
    }

    free( table->t );

    memset( table, 0, sizeof(hash_table_t) );
    free( table );
}

void *hash_table_set_pair( hash_table_t    *table,
                           char            *key,
                           int              key_len,
                           void            *val )
{
    ll_t           *ll;
    hash_pair_t    *pair;
    void           *prev_val;

    c_assert( table && table->n > 0 &&
              table->t && key && key_len > 0 );

    ll = __find_elt( &pair, table, key, key_len );

    if( pair )
    {
        if( !pair->val && val )
        {
            table->val_count++;
        }
        else
        if( pair->val && !val )
        {
            table->val_count--;
            c_assert( table->val_count >= 0 );
        }

        prev_val = pair->val;
        pair->val = val;
    }
    else
    {
        if( val ) table->val_count++;

        pair = malloc( sizeof(hash_pair_t) );
        memset( pair, 0, sizeof(hash_pair_t) );

        pair->key = strndup( key, key_len );

        prev_val = NULL;
        pair->val = val;

        LL_ADD_NODE( ll, pair );
    }

    return prev_val;
}

void *hash_table_get_val( hash_table_t     *table,
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

hash_table_iter_t *hash_table_create_iter( hash_table_t *table )
{
    hash_table_iter_t  *iter;

    iter = malloc( sizeof(hash_table_iter_t) );
    memset( iter, 0, sizeof(hash_table_iter_t) );

    iter->table = table;

    return iter;
}

void *hash_table_get_next_val( hash_table_iter_t *iter )
{
    c_assert( iter && iter->table &&
              iter->ndx < iter->table->n );

    /* NOTE: it's already reached end of the table */
    if( iter->ndx == -1 )
    {
        c_assert( !iter->elt_id );
        return NULL;
    }

    c_assert( iter->elt_id || !iter->ndx );

    ll_t           *ll;
    hash_pair_t    *pr;

    pr = PTRID_GET_PTR( iter->elt_id );

    if( pr )
    {
        ll = &iter->table->t[iter->ndx];

        c_assert( ll->total && pr->key && pr->val );

        pr = PTRID_GET_PTR( pr->next );

        while( pr )
        {
            c_assert( pr->key );

            if( pr->val )
            {
                iter->elt_id = pr->id;
                return pr->val;
            }

            pr = PTRID_GET_PTR( pr->next );
        }

        iter->ndx++;
    }

    iter->elt_id = 0;

    while( iter->ndx < iter->table->n )
    {
        ll = &iter->table->t[iter->ndx];

        if( ll->total )
        {
            LL_CHECK( ll, ll->head );

            pr = PTRID_GET_PTR( ll->head );

            while( pr )
            {
                c_assert( pr->key );

                if( pr->val )
                {
                    iter->elt_id = pr->id;
                    return pr->val;
                }

                pr = PTRID_GET_PTR( pr->next );
            }
        }

        iter->ndx++;
    }

    /* NOTE: mark that it's reached end of the table */
    iter->ndx = -1;

    return NULL;
}

