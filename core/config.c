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
#include "network.h"
#include "http.h"
#include "config.h"
#include "config_internal.h"

static ll_t                 g_file_list = {0};

static int                 *g_test_scalar_val   = NULL;
static ll_t                *g_test_scalar_list  = NULL;
static struct timeval      *g_test_mapping      = NULL;
static ll_t                *g_test_mapping_list = NULL;

static config_t             g_cfg;
static int                  g_line_num  = 0;

static config_line_t *__handle_line( char    *line,
                                     int      nread );

static void __handle_cfg_state( config_line_t  *line );

enum {
    S_INITIAL = 0,
    S_SCALAR_LIST,
    S_MAPPINGS_BLOCK,
    S_MAPPINGS_BLOCKS_LIST
};

/******************* Misc util functions **************************************/

static char_type_t __get_char_type( char ch )
{
    if( ch == '\n' )
        return CH_NEWLINE;
    else
    if( ch == '#' )
        return CH_HASH;
    else
    if( ch == ' ' )
        return CH_SPACE;
    else
    if( ch == '-' )
        return CH_HYPHEN;
    else
    if( ch == ':' )
        return CH_COLON;
    else
    if( ch >= '0' && ch <= '9' )
        return CH_WORD;
    else
    if( ch >= 'a' && ch <= 'z' )
        return CH_WORD;
    else
    if( ch >= 'A' && ch <= 'Z' )
        return CH_WORD;
    else
    if( ch == '_' || ch == '.' || ch == '/' )
        return CH_WORD;

    return CH_UNKNOWN;
}

static void __cmd_change_state()
{
    switch( g_cfg.state.cmd->type )
    {
        case SCALAR_LIST:

            g_cfg.state.st = S_SCALAR_LIST;
            break;

        case MAPPINGS_BLOCK:

            g_cfg.state.st = S_MAPPINGS_BLOCK;
            break;

        case MAPPINGS_BLOCKS_LIST:

            g_cfg.state.st = S_MAPPINGS_BLOCKS_LIST;
            break;

        default:
            assert( false );
    }
}

static void __match_cmd( config_line_t *line )
{
    config_cmd_t   *cmd;
    config_cmd_t   *cmd_next;
    int             i = 0;

    assert( g_cfg.state.st == S_INITIAL &&
            !g_cfg.state.cmd &&
            line->key_len && line->key );

    assert( ( line->state == S_LINE_CMD &&
              !line->val_len && !line->val )    ||
            ( line->state == S_LINE_CMD_AND_SCALAR &&
              line->val_len && line->val ) );

    if( g_cfg.cmd_list.total )
    {
        LL_CHECK( &g_cfg.cmd_list, g_cfg.cmd_list.head );
        cmd = PTRID_GET_PTR( g_cfg.cmd_list.head );

        while( cmd )
        {
            LL_CHECK( &g_cfg.cmd_list, cmd->id );
            cmd_next = PTRID_GET_PTR( cmd->next );

            i++;
            assert( i <= g_cfg.cmd_list.total );

            if( line->key_len == cmd->name_len &&
                !strncmp( line->key, cmd->name, cmd->name_len ) )
            {
                g_cfg.state.cmd = cmd;
                break;
            }

            cmd = cmd_next;
        }
    }

    assert( g_cfg.state.cmd && !g_cfg.state.cmd->is_in_file &&
            !g_cfg.state.cmd->cfg_complex_val.head &&
            !g_cfg.state.cmd->cfg_complex_val.tail &&
            !g_cfg.state.cmd->cfg_complex_val.total );

    assert( ( line->state == S_LINE_CMD && cmd->type != SCALAR ) ||
            ( line->state == S_LINE_CMD_AND_SCALAR && cmd->type == SCALAR ) );

    g_cfg.state.cmd->is_in_file = true;
}

static void __start_mappings_block_in_list( config_line_t *line )
{
    blocks_list_t      *block;
    mapping_node_t     *node;

    assert( g_cfg.state.cmd &&
            g_cfg.state.cmd->type == MAPPINGS_BLOCKS_LIST &&
            line->key_len && line->key &&
            line->val_len && line->val );

    if( g_cfg.state.cmd->cfg_complex_val.total )
    {
        LL_CHECK( &g_cfg.state.cmd->cfg_complex_val,
                  g_cfg.state.cmd->cfg_complex_val.tail );

        block = PTRID_GET_PTR( g_cfg.state.cmd->cfg_complex_val.tail );

        assert( block->is_completed );
    }
    else
    {
        assert( !g_cfg.state.cmd->cfg_complex_val.head &&
                !g_cfg.state.cmd->cfg_complex_val.tail );
    }

    block = malloc( sizeof(blocks_list_t) );
    memset( block, 0, sizeof(blocks_list_t) );

    LL_ADD_NODE( &g_cfg.state.cmd->cfg_complex_val, block );

    node = malloc( sizeof(mapping_node_t) );
    memset( node, 0, sizeof(mapping_node_t) );

    node->key = strndup( line->key,
                         line->key_len );

    node->val = strndup( line->val,
                         line->val_len );

    LL_ADD_NODE( &block->mappings, node );
}

static void __read_mappings_block_in_list( config_line_t *line )
{
    blocks_list_t      *block;
    mapping_node_t     *node;

    assert( g_cfg.state.cmd &&
            g_cfg.state.cmd->type == MAPPINGS_BLOCKS_LIST &&
            g_cfg.state.cmd->cfg_complex_val.total &&
            line->key_len && line->key &&
            line->val_len && line->val );

    LL_CHECK( &g_cfg.state.cmd->cfg_complex_val,
              g_cfg.state.cmd->cfg_complex_val.tail );

    block = PTRID_GET_PTR( g_cfg.state.cmd->cfg_complex_val.tail );

    assert( !block->is_completed );

    node = malloc( sizeof(mapping_node_t) );
    memset( node, 0, sizeof(mapping_node_t) );

    node->key = strndup( line->key,
                         line->key_len );

    node->val = strndup( line->val,
                         line->val_len );

    LL_ADD_NODE( &block->mappings, node );
}

static blocks_list_t *__end_mappings_block_in_list()
{
    blocks_list_t      *block;

    assert( g_cfg.state.cmd->cfg_complex_val.total );

    LL_CHECK( &g_cfg.state.cmd->cfg_complex_val,
              g_cfg.state.cmd->cfg_complex_val.tail );

    block = PTRID_GET_PTR( g_cfg.state.cmd->cfg_complex_val.tail );

    return block;
}

static void __open_cfg_file( config_file_t *config_file )
{
    int     name_len = strlen( config_file->name );

    assert( name_len <= MAX_CFG_FILE_LEN );

    config_file->fh = fopen( config_file->name, "r" );

    if( !config_file->fh )
    {
        LOGE( "filename:%s errno:%d strerror:%s",
              config_file->name, errno, strerror( errno ) );

        assert( false );
    }

    LOG( "filename:%s", config_file->name );
}

static void __parse_simplified_yaml_file( FILE *fh )
{
    char                   *line = NULL;
    size_t                  n = 0;
    ssize_t                 nread;

    while( (nread = getline( &line, &n, fh )) != -1 )
    {
        assert( line && nread );

        g_line_num++;

        LOGD( "num:%d line:%.*s",
              g_line_num, (int) nread, line );

        __handle_cfg_state( __handle_line( line, nread ) );
    }

    if( g_cfg.state.cmd )
    {
        assert( g_cfg.state.st != S_INITIAL &&
                g_cfg.state.cmd->cfg_complex_val.total );

        LL_CHECK( &g_cfg.state.cmd->cfg_complex_val,
                  g_cfg.state.cmd->cfg_complex_val.tail );

        g_cfg.state.st = S_INITIAL;
        g_cfg.state.cmd = NULL;
    }
    else
    {
        assert( g_cfg.state.st == S_INITIAL );
    }

    assert( line && !errno );
    free( line );
}

static void __parse_simplified_yaml_files()
{
    config_file_t      *config_file;

    LL_CHECK( &g_file_list, g_file_list.head );
    config_file = PTRID_GET_PTR( g_file_list.head );

    while( config_file )
    {
        LL_CHECK( &g_file_list, config_file->id );

        __parse_simplified_yaml_file( config_file->fh );

        config_file = PTRID_GET_PTR( config_file->next );
    }
}

static void __add_cmd( char                *cmd_name,
                       cmd_type_t           cmd_type,
                       void               **result_val,
                       config_cmd_cb_t      cb )
{
    config_cmd_t   *cmd;

    assert( g_cfg.state.st == S_INITIAL &&
            !g_cfg.state.cmd );

    assert( cmd_name && cmd_type >= 0 &&
            cmd_type < CMD_TYPE_MAX &&
            result_val && *result_val == NULL && cb );

    cmd = malloc( sizeof(config_cmd_t) );
    memset( cmd, 0, sizeof(config_cmd_t) );

    cmd->name_len = strlen( cmd_name );
    assert( cmd->name_len );

    cmd->name = cmd_name;
    cmd->type = cmd_type;
    cmd->result_val = result_val;
    cmd->cb = cb;

    LL_ADD_NODE( &g_cfg.cmd_list, cmd );
}

static void __invoke_callbacks()
{
    config_cmd_t   *cmd;
    config_cmd_t   *cmd_next;
    int             i = 0;

    assert( g_cfg.state.st == S_INITIAL &&
            !g_cfg.state.cmd &&
            g_cfg.cmd_list.total );

    LL_CHECK( &g_cfg.cmd_list, g_cfg.cmd_list.head );
    cmd = PTRID_GET_PTR( g_cfg.cmd_list.head );

    while( cmd )
    {
        LL_CHECK( &g_cfg.cmd_list, cmd->id );
        cmd_next = PTRID_GET_PTR( cmd->next );

        i++;

        if( !cmd->is_in_file )
        {
            cmd = cmd_next;
            continue;
        }

        switch( cmd->type )
        {
            case SCALAR:

                cmd->cb( cmd->name, cmd->type,
                         cmd->cfg_scalar_val,
                         cmd->result_val );
                break;

            case SCALAR_LIST:
            case MAPPINGS_BLOCK:
            case MAPPINGS_BLOCKS_LIST:

                cmd->cb( cmd->name, cmd->type,
                         &cmd->cfg_complex_val,
                         cmd->result_val );
                break;

            default:
                assert( false );
        }

        cmd = cmd_next;
    }

    assert( g_cfg.cmd_list.total == i );
}

/******************* A state machine for a line *******************************/

static void __line_initial_state( config_line_t    *line,
                                  char_type_t       ch_type,
                                  int               i )
{
    assert( !i && !line->key && !line->key_len &&
            !line->val && !line->val_len );

    switch( ch_type )
    {
        case CH_NEWLINE:

            line->state = S_LINE_EMPTY;
            return;

        case CH_HASH:

            line->state = S_LINE_COMMENT;
            return;

        case CH_SPACE:

            line->state = S_LINE_INDENTATION;
            return;

        case CH_WORD:

            line->key = line->raw_line;
            line->state = S_LINE_CMD;
            return;

        default:
            assert( false );
    }
}

static void __line_comment_state( config_line_t    *line,
                                  char_type_t       ch_type,
                                  int               i )
{
    assert( !line->key && !line->key_len &&
            !line->val && !line->val_len );
}

static void __line_indentation_state( config_line_t    *line,
                                      char_type_t       ch_type,
                                      int               i )
{
    assert( i <= 4 && !line->key && !line->key_len &&
            !line->val && !line->val_len );

    switch( ch_type )
    {
        case CH_SPACE:

            assert( i < 4 );
            return;

        case CH_HYPHEN:

            assert( i == 2 );
            line->state = S_LINE_INDENTATION_IN_LIST;
            return;

        case CH_WORD:

            assert( i == 4 );

            line->key = &line->raw_line[i];
            line->state = S_LINE_MAPPING;
            return;

        default:
            assert( false );
    }
}

static void __line_indentation_in_list_state( config_line_t    *line,
                                              char_type_t       ch_type,
                                              int               i )
{
    assert( i > 2 && i <= 4 &&
            !line->key && !line->key_len &&
            !line->val && !line->val_len );

    switch( ch_type )
    {
        case CH_SPACE:

            assert( i == 3 );
            return;

        case CH_WORD:

            assert( i == 4 );
            line->key = &line->raw_line[i];

            /* NOTE: could be changed to S_LINE_SCALAR_IN_LIST */
            line->state = S_LINE_MAPPING_START_BLOCK;
            return;

        default:
            assert( false );
    }
}

static void __line_cmd_state( config_line_t    *line,
                              char_type_t       ch_type,
                              int               i )
{
    assert( i && line->key &&
            !line->val && !line->val_len );

    switch( ch_type )
    {
        case CH_NEWLINE:

            assert( line->key_len );
            return;

        case CH_SPACE:

            assert( line->key_len );
            line->state = S_LINE_CMD_AND_SCALAR;
            return;

        case CH_COLON:

            assert( !line->key_len );
            line->key_len = i;
            return;

        case CH_WORD:

            assert( !line->key_len );
            return;

        default:
            assert( false );
    }
}

static void __line_cmd_and_scalar_state( config_line_t *line,
                                         char_type_t    ch_type,
                                         int            i )
{
    assert( i && line->key && line->key_len && !line->val_len );

    switch( ch_type )
    {
        case CH_NEWLINE:

            assert( line->val );
            line->val_len = &line->raw_line[i] - line->val;
            return;

        case CH_WORD:

            if( !line->val )
                line->val = &line->raw_line[i];

            return;

        default:
            assert( false );
    }
}

static void __line_mapping_state( config_line_t    *line,
                                  char_type_t       ch_type,
                                  int               i )
{
    assert( i >= 4 && line->key && !line->val_len );

    switch( ch_type )
    {
        case CH_NEWLINE:

            assert( line->key_len && line->val );
            line->val_len = &line->raw_line[i] - line->val;
            return;

        case CH_SPACE:

            assert( line->key_len && !line->val );
            return;

        case CH_COLON:

            assert( !line->key_len && !line->val );
            line->key_len = &line->raw_line[i] - line->key;
            return;

        case CH_WORD:

            if( !line->val && line->key_len )
                line->val = &line->raw_line[i];

            return;

        default:
            assert( false );
    }
}

static void __line_mapping_start_block_state( config_line_t    *line,
                                              char_type_t       ch_type,
                                              int               i )
{
    assert( i >= 4 && line->key && !line->val_len );

    switch( ch_type )
    {
        case CH_NEWLINE:

            if( line->val )
            {
                assert( line->key_len );
                line->val_len = &line->raw_line[i] - line->val;
            }
            else
            {
                assert( !line->key_len );
                line->key_len = &line->raw_line[i] - line->key;

                line->state = S_LINE_SCALAR_IN_LIST;
            }
            return;

        case CH_SPACE:

            assert( line->key_len && !line->val );
            return;

        case CH_COLON:

            assert( !line->key_len && !line->val );
            line->key_len = &line->raw_line[i] - line->key;
            return;

        case CH_WORD:

            if( !line->val && line->key_len )
                line->val = &line->raw_line[i];

            return;

        default:
            assert( false );
    }
}

static config_line_t *__handle_line( char *line, int nread )
{
    static config_line_t    config_line;
    char_type_t             ch_type;
    int                     i;

    memset( &config_line, 0, sizeof(config_line_t) );

    config_line.state = S_LINE_INITIAL;
    config_line.raw_line = line;
    config_line.nread = nread;

    for( i = 0; i < nread; i++ )
    {
        ch_type = __get_char_type( line[i] );

        assert( (ch_type != CH_NEWLINE && i != nread - 1) ||
                (ch_type == CH_NEWLINE && i == nread - 1) );

        switch( config_line.state )
        {
            case S_LINE_INITIAL:
                __line_initial_state( &config_line, ch_type, i );
                break;

            case S_LINE_COMMENT:
                __line_comment_state( &config_line, ch_type, i );
                break;

            case S_LINE_INDENTATION:
                __line_indentation_state( &config_line, ch_type, i );
                break;

            case S_LINE_INDENTATION_IN_LIST:
                __line_indentation_in_list_state( &config_line, ch_type, i );
                break;

            case S_LINE_CMD:
                __line_cmd_state( &config_line, ch_type, i );
                break;

            case S_LINE_CMD_AND_SCALAR:
                __line_cmd_and_scalar_state( &config_line, ch_type, i );
                break;

            case S_LINE_MAPPING:
                __line_mapping_state( &config_line, ch_type, i );
                break;

            case S_LINE_MAPPING_START_BLOCK:
                __line_mapping_start_block_state( &config_line, ch_type, i );
                break;

            default:
                assert( false );
        }
    }

    LOGD( "num:%d line_state:%d "
          "key_len:%d key:%.*s "
          "val_len:%d val:%.*s",
          g_line_num, config_line.state,
          config_line.key_len, config_line.key_len,
          config_line.key_len ? config_line.key : "",
          config_line.val_len, config_line.val_len,
          config_line.val_len ? config_line.val : "" );

    return &config_line;
}

/******************* A state machine for simplified yaml **********************/

static void __init_state( config_line_t *line )
{
    switch( line->state )
    {
        case S_LINE_EMPTY:
        case S_LINE_COMMENT:

            break;

        case S_LINE_CMD:

            __match_cmd( line );
            __cmd_change_state();
            break;

        case S_LINE_CMD_AND_SCALAR:

            __match_cmd( line );

            g_cfg.state.cmd->cfg_scalar_val = strndup( line->val,
                                                       line->val_len );

            g_cfg.state.cmd = NULL;
            break;

        default:
            assert( false );
    }
}

static void __scalar_list_state( config_line_t *line )
{
    mapping_node_t     *node;

    switch( line->state )
    {
        case S_LINE_EMPTY:

            assert( g_cfg.state.cmd &&
                    g_cfg.state.cmd->type == SCALAR_LIST &&
                    g_cfg.state.cmd->cfg_complex_val.total &&
                    !line->key_len && !line->key &&
                    !line->val_len && !line->val );

            LL_CHECK( &g_cfg.state.cmd->cfg_complex_val,
                      g_cfg.state.cmd->cfg_complex_val.tail );

            g_cfg.state.st = S_INITIAL;
            g_cfg.state.cmd = NULL;
            break;

        case S_LINE_SCALAR_IN_LIST:

            assert( g_cfg.state.cmd &&
                    g_cfg.state.cmd->type == SCALAR_LIST &&
                    line->key_len && line->key &&
                    !line->val_len && !line->val );

            node = malloc( sizeof(mapping_node_t) );
            memset( node, 0, sizeof(mapping_node_t) );

            node->val = strndup( line->key,
                                 line->key_len );

            LL_ADD_NODE( &g_cfg.state.cmd->cfg_complex_val, node );
            break;

        default:
            assert( false );
    }
}

static void __mappings_block_state( config_line_t *line )
{
    mapping_node_t     *node;

    switch( line->state )
    {
        case S_LINE_EMPTY:

            assert( g_cfg.state.cmd &&
                    g_cfg.state.cmd->type == MAPPINGS_BLOCK &&
                    g_cfg.state.cmd->cfg_complex_val.total &&
                    !line->key_len && !line->key &&
                    !line->val_len && !line->val );

            LL_CHECK( &g_cfg.state.cmd->cfg_complex_val,
                      g_cfg.state.cmd->cfg_complex_val.tail );

            g_cfg.state.st = S_INITIAL;
            g_cfg.state.cmd = NULL;
            break;

        case S_LINE_MAPPING:

            assert( g_cfg.state.cmd &&
                    g_cfg.state.cmd->type == MAPPINGS_BLOCK &&
                    line->key_len && line->key &&
                    line->val_len && line->val );

            node = malloc( sizeof(mapping_node_t) );
            memset( node, 0, sizeof(mapping_node_t) );

            node->key = strndup( line->key,
                                 line->key_len );

            node->val = strndup( line->val,
                                 line->val_len );

            LL_ADD_NODE( &g_cfg.state.cmd->cfg_complex_val, node );
            break;

        default:
            assert( false );
    }
}

static void __mappings_blocks_list_state( config_line_t *line )
{
    blocks_list_t      *block;

    switch( line->state )
    {
        case S_LINE_EMPTY:
        case S_LINE_COMMENT:

            assert( g_cfg.state.cmd &&
                    g_cfg.state.cmd->type == MAPPINGS_BLOCKS_LIST &&
                    !line->key_len && !line->key &&
                    !line->val_len && !line->val );

            block = __end_mappings_block_in_list();

            if( block->is_completed )
            {
                g_cfg.state.st = S_INITIAL;
                g_cfg.state.cmd = NULL;
            }
            else
                block->is_completed = true;

            break;

        case S_LINE_MAPPING:

            __read_mappings_block_in_list( line );
            break;

        case S_LINE_MAPPING_START_BLOCK:

            __start_mappings_block_in_list( line );
            break;

        default:
            assert( false );
    }
}

static void __handle_cfg_state( config_line_t *line )
{
    assert( line->state != S_LINE_INITIAL       &&
            line->state != S_LINE_INDENTATION   &&
            line->state != S_LINE_INDENTATION_IN_LIST );

    switch( g_cfg.state.st )
    {
        case S_INITIAL:
            __init_state( line );
            break;

        case S_SCALAR_LIST:
            __scalar_list_state( line );
            break;

        case S_MAPPINGS_BLOCK:
            __mappings_block_state( line );
            break;

        case S_MAPPINGS_BLOCKS_LIST:
            __mappings_blocks_list_state( line );
            break;

        default:
            assert( false );
    }

    LOGD( "num:%d cfg_state:%d "
          "cmd_name:%.*s cmd_type:%d",
          g_line_num, g_cfg.state.st,
          g_cfg.state.cmd ? g_cfg.state.cmd->name_len : 0,
          g_cfg.state.cmd ?
              (g_cfg.state.cmd->name ? g_cfg.state.cmd->name : "") : "",
          g_cfg.state.cmd ? g_cfg.state.cmd->type : 0 );
}

/******************* Cmd callbacks ********************************************/

/*************** generic cmds ******************/

static void __integer_cb( char        *cmd_name,
                          cmd_type_t   cmd_type,
                          void        *cfg_val,
                          void       **result_val )
{
    char               *cfg_scalar_val = cfg_val;
    int                *val;

    assert( cmd_type == SCALAR );
    assert( *result_val == NULL );

    val = malloc( sizeof(int) );
    *val = atoi( cfg_scalar_val );

    LOG( "cmd_name:%s cfg_val:%s result_val:%d",
         cmd_name, cfg_scalar_val, *val );

    *result_val = val;
}

static void __string_cb( char        *cmd_name,
                         cmd_type_t   cmd_type,
                         void        *cfg_val,
                         void       **result_val )
{
    char               *cfg_scalar_val = cfg_val;
    char               *val;

    assert( cmd_type == SCALAR );
    assert( *result_val == NULL );

    val = strdup( cfg_scalar_val );

    LOG( "cmd_name:%s cfg_val:%s result_val:%s",
         cmd_name, cfg_scalar_val, val );

    *result_val = val;
}

static void __parse_timeval( ll_t              *list,
                             struct timeval    *tm )
{
#define TV_SEC          "tv_sec"
#define TV_USEC         "tv_usec"

    mapping_node_t     *node;
    mapping_node_t     *node_next;
    bool                tv_sec_flag = false;
    bool                tv_usec_flag = false;

    assert( !tm->tv_sec && !tm->tv_usec );

    LL_CHECK( list, list->head );
    node = PTRID_GET_PTR( list->head );

    while( node )
    {
        LL_CHECK( list, node->id );
        node_next = PTRID_GET_PTR( node->next );

        if( !strncmp( node->key, TV_SEC, sizeof(TV_SEC) ) )
        {
            assert( !tv_sec_flag );

            tm->tv_sec = atol( node->val );

            tv_sec_flag = true;

            LOG( "cfg_val:%s tv_sec:%ld",
                 node->val, tm->tv_sec );
        }
        else
        if( !strncmp( node->key, TV_USEC, sizeof(TV_USEC) ) )
        {
            assert( !tv_usec_flag );

            tm->tv_usec = atol( node->val );

            tv_usec_flag = true;

            LOG( "cfg_val:%s tv_usec:%ld",
                 node->val, tm->tv_usec );
        }
        else
        {
            assert( false );
        }

        node = node_next;
    }

    assert( tv_sec_flag && tv_usec_flag );

#undef TV_SEC
#undef TV_USEC
}

static ll_node_t *__parse_timeval_node( ll_t *list )
{
    tm_node_t  *tm_node;

    tm_node = malloc( sizeof(tm_node_t) );
    memset( tm_node, 0, sizeof(tm_node_t) );

    __parse_timeval( list, &tm_node->tm );

    return (ll_node_t *) tm_node;
}

static void __timeval_cb( char        *cmd_name,
                          cmd_type_t   cmd_type,
                          void        *cfg_val,
                          void       **result_val )
{
    ll_t               *cfg_complex_val = cfg_val;
    struct timeval     *tm;

    assert( cmd_type == MAPPINGS_BLOCK );
    assert( *result_val == NULL );

    LOG( "cmd_name:%s", cmd_name );

    tm = malloc( sizeof(struct timeval) );
    memset( tm, 0, sizeof(struct timeval) );

    __parse_timeval( cfg_complex_val, tm );

    *result_val = tm;
}

static void __generic_mapping_list( char        *cmd_name,
                                    cmd_type_t   cmd_type,
                                    void        *cfg_val,
                                    void       **result_val,
                                    ll_node_t   *( *parser )( ll_t *list ) )
{
    ll_t               *cfg_complex_val = cfg_val;
    blocks_list_t      *block;
    blocks_list_t      *block_next;
    ll_t               *res_mapping_list;
    ll_node_t          *res_node;

    assert( cmd_type == MAPPINGS_BLOCKS_LIST );
    assert( *result_val == NULL );

    LOG( "cmd_name:%s", cmd_name );

    res_mapping_list = malloc( sizeof(ll_t) );
    memset( res_mapping_list, 0, sizeof(ll_t) );

    LL_CHECK( cfg_complex_val, cfg_complex_val->head );
    block = PTRID_GET_PTR( cfg_complex_val->head );

    while( block )
    {
        LL_CHECK( cfg_complex_val, block->id );
        block_next = PTRID_GET_PTR( block->next );

        LOG( "-" );

        res_node = parser( &block->mappings );

        LL_ADD_NODE( res_mapping_list, res_node );

        block = block_next;
    }

    *result_val = res_mapping_list;
}

/*************** config_test cmds **************/

static void __test_scalar_list_cb( char        *cmd_name,
                                   cmd_type_t   cmd_type,
                                   void        *cfg_val,
                                   void       **result_val )
{
    ll_t               *cfg_complex_val = cfg_val;
    mapping_node_t     *node;
    mapping_node_t     *node_next;
    ll_t               *scalar_list;
    test_int_node_t    *int_node;

    assert( cmd_type == SCALAR_LIST );
    assert( *result_val == NULL );

    LOG( "cmd_name:%s", cmd_name );

    scalar_list = malloc( sizeof(ll_t) );
    memset( scalar_list, 0, sizeof(ll_t) );

    LL_CHECK( cfg_complex_val, cfg_complex_val->head );
    node = PTRID_GET_PTR( cfg_complex_val->head );

    while( node )
    {
        LL_CHECK( cfg_complex_val, node->id );
        node_next = PTRID_GET_PTR( node->next );

        int_node = malloc( sizeof(test_int_node_t) );
        int_node->val = atoi( node->val );

        LOG( "cfg_val:%s result_val:%d",
             node->val, int_node->val );

        LL_ADD_NODE( scalar_list, int_node );

        node = node_next;
    }

    *result_val = scalar_list;
}

static void __test_mapping_list_cb( char        *cmd_name,
                                    cmd_type_t   cmd_type,
                                    void        *cfg_val,
                                    void       **result_val )
{
    __generic_mapping_list( cmd_name, cmd_type,
                            cfg_val, result_val,
                            __parse_timeval_node );
}

/*************** network cmds ******************/

static void __parse_net_host( ll_t         *list,
                              net_host_t   *host )
{
#define HOST            "host"
#define PORT            "port"
#define SSL             "ssl"
#define LABEL           "label"

    mapping_node_t     *node;
    mapping_node_t     *node_next;
    bool                host_flag = false;
    bool                port_flag = false;
    bool                ssl_flag = false;
    bool                label_flag = false;

    assert( !host->hostname && !host->port &&
            !host->use_ssl );

    LL_CHECK( list, list->head );
    node = PTRID_GET_PTR( list->head );

    while( node )
    {
        LL_CHECK( list, node->id );
        node_next = PTRID_GET_PTR( node->next );

        if( !strncmp( node->key, HOST, sizeof(HOST) ) )
        {
            assert( !host_flag );

            host->hostname = strdup( node->val );

            host_flag = true;

            LOG( "cfg_val:%s host:%s",
                 node->val, host->hostname );
        }
        else
        if( !strncmp( node->key, PORT, sizeof(PORT) ) )
        {
            assert( !port_flag );

            host->port = strdup( node->val );

            port_flag = true;

            LOG( "cfg_val:%s port:%s",
                 node->val, host->port );
        }
        else
        if( !strncmp( node->key, SSL, sizeof(SSL) ) )
        {
            assert( !ssl_flag );

            host->use_ssl = atoi( node->val );

            ssl_flag = true;

            LOG( "cfg_val:%s ssl:%d",
                 node->val, host->use_ssl );
        }
        else
        if( !strncmp( node->key, LABEL, sizeof(LABEL) ) )
        {
            assert( !label_flag );

            host->label = strdup( node->val );

            label_flag = true;

            LOG( "cfg_val:%s label:%s",
                 node->val, host->label );
        }
        else
        {
            assert( false );
        }

        node = node_next;
    }

    assert( host_flag && port_flag && ssl_flag &&
            host->hostname[0] && host->port[0] &&
            (host->use_ssl == 0 || host->use_ssl == 1) );

#undef HOST
#undef PORT
#undef SSL
#undef LABEL
}

static ll_node_t *__parse_net_host_node( ll_t *list )
{
    net_host_node_t    *host_node;

    host_node = malloc( sizeof(net_host_node_t) );
    memset( host_node, 0, sizeof(net_host_node_t) );

    __parse_net_host( list, &host_node->host );

    return (ll_node_t *) host_node;
}

static void __main_hosts_cb( char          *cmd_name,
                             cmd_type_t     cmd_type,
                             void          *cfg_val,
                             void         **result_val )
{
    __generic_mapping_list( cmd_name, cmd_type,
                            cfg_val, result_val,
                            __parse_net_host_node );
}

/******************* Register config commands *********************************/

static void __register_cmds()
{
    /*************** config_test cmds **************/

    __add_cmd( "config_test_scalar", SCALAR,
               (void **) &g_test_scalar_val,
               __integer_cb );

    __add_cmd( "config_test_scalar_list", SCALAR_LIST,
               (void **) &g_test_scalar_list,
               __test_scalar_list_cb );

    __add_cmd( "config_test_mapping", MAPPINGS_BLOCK,
               (void **) &g_test_mapping,
               __timeval_cb );

    __add_cmd( "config_test_mapping_list", MAPPINGS_BLOCKS_LIST,
               (void **) &g_test_mapping_list,
               __test_mapping_list_cb );

    /*************** logger cmds *******************/

    __add_cmd( "logger_logfile", SCALAR,
               (void **) &cfg_logger_logfile,
               __string_cb );

    __add_cmd( "logger_rotate_interval", MAPPINGS_BLOCK,
               (void **) &cfg_logger_rotate_interval,
               __timeval_cb );

    __add_cmd( "logger_debug_rotate_interval", MAPPINGS_BLOCK,
               (void **) &cfg_logger_debug_rotate_interval,
               __timeval_cb );

    /*************** network cmds ******************/

    __add_cmd( "net_cert_file", SCALAR,
               (void **) &cfg_net_cert_file,
               __string_cb );

    __add_cmd( "net_key_file", SCALAR,
               (void **) &cfg_net_key_file,
               __string_cb );

    __add_cmd( "net_cert_test_file", SCALAR,
               (void **) &cfg_net_cert_test_file,
               __string_cb );

    __add_cmd( "net_key_test_file", SCALAR,
               (void **) &cfg_net_key_test_file,
               __string_cb );

    __add_cmd( "net_ssl_shutdown_timeout", MAPPINGS_BLOCK,
               (void **) &cfg_net_ssl_shutdown_timeout,
               __timeval_cb );

    __add_cmd( "net_ssl_establish_timeout", MAPPINGS_BLOCK,
               (void **) &cfg_net_ssl_establish_timeout,
               __timeval_cb );

    __add_cmd( "net_ssl_accept_timeout", MAPPINGS_BLOCK,
               (void **) &cfg_net_ssl_accept_timeout,
               __timeval_cb );

    __add_cmd( "net_establish_timeout", MAPPINGS_BLOCK,
               (void **) &cfg_net_establish_timeout,
               __timeval_cb );

    __add_cmd( "net_flush_and_close_timeout", MAPPINGS_BLOCK,
               (void **) &cfg_net_flush_and_close_timeout,
               __timeval_cb );

    /*************** http cmds *********************/

    __add_cmd( "http_response_timeout", MAPPINGS_BLOCK,
               (void **) &cfg_http_response_timeout,
               __timeval_cb );

    __add_cmd( "http_check_messages_queue_interval", MAPPINGS_BLOCK,
               (void **) &cfg_http_check_messages_queue_interval,
               __timeval_cb );
}

/******************* Interface functions **************************************/

void config_add_cmd( char                  *cmd_name,
                     config_cmd_type_t      cmd_type,
                     void                 **result_val )
{
    switch( cmd_type )
    {
        case CONFIG_CMD_TYPE_INTEGER:

            __add_cmd( cmd_name, SCALAR,
                       result_val, __integer_cb );

            break;

        case CONFIG_CMD_TYPE_STRING:

            __add_cmd( cmd_name, SCALAR,
                       result_val, __string_cb );

            break;

        case CONFIG_CMD_TYPE_TIMEVAL:

            __add_cmd( cmd_name, MAPPINGS_BLOCK,
                       result_val, __timeval_cb );

            break;

        case CONFIG_CMD_TYPE_MAIN_HOSTS:

            __add_cmd( cmd_name, MAPPINGS_BLOCKS_LIST,
                       result_val, __main_hosts_cb );

            break;

        default:

            assert( false );
            break;
    }
}

void config_add_file( char *filename )
{
    config_file_t      *config_file;

    config_file = malloc( sizeof(config_file_t) );
    memset( config_file, 0, sizeof(config_file_t) );

    config_file->name = strdup( filename );

    __open_cfg_file( config_file );

    LL_ADD_NODE( &g_file_list, config_file );
}

/******************* Config init **********************************************/

void config_init( char                     *filename,
                  module_cfg_init_cb_t      module_cfg_init_cb )
{
    memset( &g_cfg, 0, sizeof(config_t) );

    config_add_file( filename );

    __register_cmds();

    module_cfg_init_cb();

    if( !g_cfg.cmd_list.total )
        return;

    __parse_simplified_yaml_files();

    __invoke_callbacks();
}

