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

/* NOTE: ll == linked list */

/* NOTE: this is the beginning of any node structure */
typedef struct {
    ptr_id_t    id;
    ptr_id_t    prev;
    ptr_id_t    next;
} ll_node_t;

typedef struct {
    ptr_id_t    head;
    ptr_id_t    tail;
    int         total;
} ll_t;

#define LL_ADD_NODE( l, n )     do {                                           \
                                    ptr_id_t    nid = PTRID(n);                \
                                    ll_node_t  *tail;                          \
                                                                               \
                                    c_assert( (l)->total >= 0 &&               \
                                              !(n)->prev && !(n)->next );      \
                                                                               \
                                    if( (l)->tail )                            \
                                    {                                          \
                                        c_assert( (l)->total >= 1 &&           \
                                                  (l)->head );                 \
                                                                               \
                                        tail = PTRID_GET_PTR( (l)->tail );     \
                                        c_assert( tail->id == (l)->tail );     \
                                                                               \
                                        tail->next = nid;                      \
                                        (n)->prev = (l)->tail;                 \
                                                                               \
                                        (l)->tail = nid;                       \
                                    }                                          \
                                    else                                       \
                                    {                                          \
                                        c_assert( !(l)->head );                \
                                                                               \
                                        (l)->head = nid;                       \
                                        (l)->tail = (l)->head;                 \
                                    }                                          \
                                                                               \
                                    (n)->id = (l)->tail;                       \
                                                                               \
                                    (l)->total++;                              \
                                } while( false )


#define LL_DEL_NODE( l, nid )   do {                                           \
                                    ll_node_t  *n = PTRID_GET_PTR(nid);        \
                                    ll_node_t  *prev, *next;                   \
                                                                               \
                                    c_assert( n->id == (nid) &&                \
                                              (l)->total > 0 );                \
                                                                               \
                                    if( n->prev && n->next )                   \
                                    {                                          \
                                        c_assert( (l)->total >= 3 &&           \
                                                  (nid) != (l)->head &&        \
                                                  (nid) != (l)->tail );        \
                                                                               \
                                        prev = PTRID_GET_PTR( n->prev );       \
                                        next = PTRID_GET_PTR( n->next );       \
                                                                               \
                                        c_assert( prev->id == n->prev &&       \
                                                  next->id == n->next );       \
                                                                               \
                                        c_assert( prev->next == (nid) &&       \
                                                  next->prev == (nid) );       \
                                                                               \
                                        prev->next = n->next;                  \
                                        next->prev = n->prev;                  \
                                                                               \
                                        n->prev = 0;                           \
                                        n->next = 0;                           \
                                    }                                          \
                                    else                                       \
                                    if( n->prev )                              \
                                    {                                          \
                                        c_assert( (l)->total >= 2 &&           \
                                                  (nid) != (l)->head &&        \
                                                  (nid) == (l)->tail );        \
                                                                               \
                                        prev = PTRID_GET_PTR( n->prev );       \
                                                                               \
                                        c_assert( prev->id == n->prev );       \
                                        c_assert( prev->next == (nid) );       \
                                                                               \
                                        prev->next = 0;                        \
                                        (l)->tail = n->prev;                   \
                                    }                                          \
                                    else                                       \
                                    if( n->next )                              \
                                    {                                          \
                                        c_assert( (l)->total >= 2 &&           \
                                                  (nid) == (l)->head &&        \
                                                  (nid) != (l)->tail );        \
                                                                               \
                                        next = PTRID_GET_PTR( n->next );       \
                                                                               \
                                        c_assert( next->id == n->next );       \
                                        c_assert( next->prev == (nid) );       \
                                                                               \
                                        next->prev = 0;                        \
                                        (l)->head = n->next;                   \
                                    }                                          \
                                    else                                       \
                                    {                                          \
                                        c_assert( (l)->total == 1 &&           \
                                                  (nid) == (l)->head &&        \
                                                  (nid) == (l)->tail );        \
                                                                               \
                                        (l)->head = 0;                         \
                                        (l)->tail = 0;                         \
                                    }                                          \
                                                                               \
                                    c_assert( (l)->total-- );                  \
                                } while( false )


#define LL_CHECK( l, nid )      do {                                           \
                                    ll_node_t  *n = PTRID_GET_PTR(nid);        \
                                    ll_node_t  *prev, *next;                   \
                                                                               \
                                    c_assert( n->id == (nid) &&                \
                                              (l)->total > 0 );                \
                                                                               \
                                    if( n->prev && n->next )                   \
                                    {                                          \
                                        c_assert( (l)->total >= 3 &&           \
                                                  (nid) != (l)->head &&        \
                                                  (nid) != (l)->tail );        \
                                                                               \
                                        prev = PTRID_GET_PTR( n->prev );       \
                                        next = PTRID_GET_PTR( n->next );       \
                                                                               \
                                        c_assert( prev->id == n->prev &&       \
                                                  next->id == n->next );       \
                                                                               \
                                        c_assert( prev->next == (nid) &&       \
                                                  next->prev == (nid) );       \
                                    }                                          \
                                    else                                       \
                                    if( n->prev )                              \
                                    {                                          \
                                        c_assert( (l)->total >= 2 &&           \
                                                  (nid) != (l)->head &&        \
                                                  (nid) == (l)->tail );        \
                                                                               \
                                        prev = PTRID_GET_PTR( n->prev );       \
                                                                               \
                                        c_assert( prev->id == n->prev );       \
                                        c_assert( prev->next == (nid) );       \
                                    }                                          \
                                    else                                       \
                                    if( n->next )                              \
                                    {                                          \
                                        c_assert( (l)->total >= 2 &&           \
                                                  (nid) == (l)->head &&        \
                                                  (nid) != (l)->tail );        \
                                                                               \
                                        next = PTRID_GET_PTR( n->next );       \
                                                                               \
                                        c_assert( next->id == n->next );       \
                                        c_assert( next->prev == (nid) );       \
                                    }                                          \
                                    else                                       \
                                    {                                          \
                                        c_assert( (l)->total == 1 &&           \
                                                  (nid) == (l)->head &&        \
                                                  (nid) == (l)->tail );        \
                                    }                                          \
                                } while( false )

