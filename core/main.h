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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <stdbool.h>
#include <termios.h>
#include <assert.h>
#include <errno.h>
#include "crc32.h"
#include "gitrev.h"

struct timeval              G_now;

#if     __SIZEOF_POINTER__ == 4

typedef uint64_t            ptr_id_t;
typedef uint32_t            _id_t;

#define PTRID_FMT( p )      (long long unsigned int) (p)
#define PTRID_GET_PTR( p )  ((void *) (uint32_t) ( (p) & 0xffffffff ))
#define PTRID( ptr )        ( ((uint64_t) (++G_id) << 32) | (uint32_t) (ptr) )

#elif   __SIZEOF_POINTER__ == 8

typedef unsigned int        uint128_t   __attribute__((mode(TI)));
typedef uint128_t           ptr_id_t;
typedef uint64_t            _id_t;

#define PTRID_FMT( p )      (long long unsigned int)( (p) >> 64 )
#define PTRID_GET_PTR( p )  ((void *) (uint64_t) ( (p) & 0xffffffffffffffff ))
#define PTRID( ptr )        ( ((uint128_t) (++G_id) << 64) | (uint64_t) (ptr) )

#endif

extern _id_t                G_id;

/* c_ == customized assert */
/* NOTE: cond should NOT contain any useful *
 * code because of possible NDEBUG          */
#ifdef  DEBUG
#define c_assert( cond )    assert( cond )
#else
#define c_assert( cond )    do {                    \
                                if( !(cond) )       \
                                {                   \
                                    LOGA( cond );   \
                                }                   \
                            } while( false )
#endif

#define ARG_CONFIG_PREFIX       "config:"
#define ARG_CONFIG_PREFIX_LEN   (sizeof(ARG_CONFIG_PREFIX) - 1)

#define ARG_MODULE_PREFIX       "module:"
#define ARG_MODULE_PREFIX_LEN   (sizeof(ARG_MODULE_PREFIX) - 1)

#define ARG_NO_DEBUG_LOG        "no_debug_log"

#define DEFAULT_CONFIG_FILE     "core/core.cfg"

