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

#define MAX_LOG_FILE_LEN        255

FILE               *G_log_fh;

char               *cfg_logger_logfile = NULL;
struct timeval     *cfg_logger_rotate_interval = NULL;
struct timeval     *cfg_logger_debug_rotate_interval = NULL;

#ifdef  DEBUG
bool                G_logger_no_debug = false;
#endif

static void __open_log_file()
{
    if( G_log_fh )
    {
        if( G_log_fh != stdout && fclose( G_log_fh ) )
        {
            printf( "%s:%d filename:%s errno:%d strerror:%s\n",
                    __FUNCTION__, __LINE__,
                    cfg_logger_logfile ? cfg_logger_logfile : "stdout",
                    errno, strerror( errno ) );
        }

        G_log_fh = NULL;
    }

    printf( "%s:%d filename:%s\n", __FUNCTION__, __LINE__,
            cfg_logger_logfile ? cfg_logger_logfile : "stdout" );

    if( cfg_logger_logfile )
    {
        assert( strlen( cfg_logger_logfile ) <= MAX_LOG_FILE_LEN );

        G_log_fh = fopen( cfg_logger_logfile, "a+" );

        if( !G_log_fh )
        {
            printf( "%s:%d filename:%s errno:%d strerror:%s\n",
                    __FUNCTION__, __LINE__,
                    cfg_logger_logfile ? cfg_logger_logfile : "stdout",
                    errno, strerror( errno ) );

            G_log_fh = stdout;
        }
    }
    else
        G_log_fh = stdout;
}

static void __rotate_tmr( conn_id_t    conn_id,
                          ptr_id_t     conn_udata_id,
                          tmr_id_t     tmr_id,
                          ptr_id_t     tmr_udata_id )
{
    char   *logfile;

    c_assert( !conn_id && !conn_udata_id &&
              tmr_id && tmr_udata_id );

    logfile = PTRID_GET_PTR( tmr_udata_id );

    c_assert( logfile == cfg_logger_logfile );

    LOG( "filename:%s", cfg_logger_logfile ? cfg_logger_logfile : "stdout" );

    if( G_log_fh == stdout )
    {
        if( fflush( G_log_fh ) )
        {
            LOGE( "errno:%d strerror:%s",
                  errno, strerror( errno ) );
        }

        return;
    }

    c_assert( G_log_fh && cfg_logger_logfile );

    if( fclose( G_log_fh ) )
    {
        printf( "%s:%d filename:%s errno:%d strerror:%s\n",
                __FUNCTION__, __LINE__, cfg_logger_logfile,
                errno, strerror( errno ) );
    }

    G_log_fh = NULL;

    char    prev_file[MAX_LOG_FILE_LEN + 8];

    snprintf( prev_file, sizeof(prev_file), "%s.prev", cfg_logger_logfile );

    if( remove( prev_file ) )
    {
        printf( "%s:%d filename:%s prev_filename:%s "
                "errno:%d strerror:%s\n",
                __FUNCTION__, __LINE__,
                cfg_logger_logfile, prev_file,
                errno, strerror( errno ) );
    }

    if( rename( cfg_logger_logfile, prev_file ) )
    {
        printf( "%s:%d filename:%s prev_filename:%s "
                "errno:%d strerror:%s\n",
                __FUNCTION__, __LINE__,
                cfg_logger_logfile, prev_file,
                errno, strerror( errno ) );
    }

    __open_log_file();
}

static void __default_config_init()
{
    if( !cfg_logger_rotate_interval )
    {
        cfg_logger_rotate_interval = malloc( sizeof(struct timeval) );

        cfg_logger_rotate_interval->tv_sec = 3600;
        cfg_logger_rotate_interval->tv_usec = 0;
    }

    if( !cfg_logger_debug_rotate_interval )
    {
        cfg_logger_debug_rotate_interval = malloc( sizeof(struct timeval) );

        cfg_logger_debug_rotate_interval->tv_sec = 60;
        cfg_logger_debug_rotate_interval->tv_usec = 0;
    }
}

/* NOTE: prefer to use tmpfs for log files */
void logger_init()
{
    __default_config_init();

    __open_log_file();

#ifdef  DEBUG
    if( !G_logger_no_debug )
    {
        net_make_global_tmr( PTRID( cfg_logger_logfile ),
                             __rotate_tmr,
                             cfg_logger_debug_rotate_interval );
    }
    else
#endif
    {
        net_make_global_tmr( PTRID( cfg_logger_logfile ),
                             __rotate_tmr,
                             cfg_logger_rotate_interval );
    }
}

