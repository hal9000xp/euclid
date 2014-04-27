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

/* NOTE: real asserts here */

#include "main.h"
#include "linked_list.h"
#include "logger.h"
#include "module.h"
#include "module_list.h"

ll_t                G_modules = {0};

void module_add( char                  *name,
                 module_cfg_init_cb_t   cfg_init_cb,
                 module_init_cb_t       init_cb )
{
    module_t       *module;

    module = malloc( sizeof(module_t) );
    memset( module, 0, sizeof(module_t) );

    module->name = strdup( name );
    module->cfg_init_cb = cfg_init_cb;
    module->init_cb = init_cb;

    LL_ADD_NODE( &G_modules, module );
}

module_t *module_get( char *name )
{
    module_t       *module;
    bool            found = false;

    module_list_init();

    LL_CHECK( &G_modules, G_modules.head );

    module = PTRID_GET_PTR( G_modules.head );

    if( !name )
        return module;

    while( module )
    {
        LL_CHECK( &G_modules, module->id );

        assert( module->name &&
                module->cfg_init_cb &&
                module->init_cb );

        if( !strcmp( module->name, name ) )
        {
            found = true;
            break;
        }

        module = PTRID_GET_PTR( module->next );
    }

    assert( found );

    return module;
}

