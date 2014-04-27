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
#include "module.h"
#include "config.h"
#include "network.h"
#include "http.h"

_id_t           G_id = 0;

static void main_init()
{
    int     r;

    r = gettimeofday( &G_now, NULL );
    assert( !r );

    /* NOTE: should be initialized *
     * before config_init()        */
    G_log_fh = stdout;

    r = signal( SIGPIPE, SIG_IGN ) != SIG_ERR;
    assert( r );
}

static void __get_args( char              **cfg_file,
                        char              **module_name,
                        int                 argc,
                        char              **argv )
{
    char   *arg;
    int     len;
    int     i;

    assert( argc >= 1 && argc <= 3 );

    *cfg_file = DEFAULT_CONFIG_FILE;
    *module_name = NULL;

    for( i = 1; i < argc; i++ )
    {
        arg = argv[i];
        len = strlen( arg );

        if( len > ARG_CONFIG_PREFIX_LEN &&
            !strncasecmp( arg, ARG_CONFIG_PREFIX,
                          ARG_CONFIG_PREFIX_LEN ) )
        {
            *cfg_file = arg + ARG_CONFIG_PREFIX_LEN;
        }
        else
        if( len > ARG_MODULE_PREFIX_LEN &&
            !strncasecmp( arg, ARG_MODULE_PREFIX,
                          ARG_MODULE_PREFIX_LEN ) )
        {
            *module_name = arg + ARG_MODULE_PREFIX_LEN;
        }
        else
#ifdef  DEBUG
        if( !strcasecmp( arg, ARG_NO_DEBUG_LOG ) )
        {
            G_logger_no_debug = true;
        }
        else
#endif
        {
            assert( false );
        }
    }
}

int main( int argc, char **argv )
{
    char               *cfg_file;
    char               *module_name;
    module_t           *module;

    __get_args( &cfg_file, &module_name, argc, argv );

    main_init();

    module = module_get( module_name );

    config_init( cfg_file, module->cfg_init_cb );

    logger_init();

    net_init();

    http_init();

    module->init_cb();

    net_main_loop();

    return 0;
}

