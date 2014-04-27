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

/* NOTE: Structs for simplified YAML config file. *
 * Something like this:                           *
 * http://www.yaml.org/spec/1.2/spec.html#Preview */

#define MAX_CFG_FILE_LEN    255

typedef enum {
    SCALAR = 0,
    SCALAR_LIST,
    MAPPINGS_BLOCK,
    MAPPINGS_BLOCKS_LIST,
    CMD_TYPE_MAX
} cmd_type_t;

typedef void ( *config_cmd_cb_t )( char        *cmd_name,
                                   cmd_type_t   cmd_type,
                                   void        *cfg_val,
                                   void       **result_val );

typedef struct {
    /* ll_node_t */
    ptr_id_t        id;
    ptr_id_t        prev;
    ptr_id_t        next;

    char           *key;
    char           *val;
} mapping_node_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t        id;
    ptr_id_t        prev;
    ptr_id_t        next;

    bool            is_completed;

    ll_t            mappings;
} blocks_list_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t            id;
    ptr_id_t            prev;
    ptr_id_t            next;

    int                 name_len;
    char               *name;

    bool                is_in_file;

    cmd_type_t          type;

    char               *cfg_scalar_val;
    ll_t                cfg_complex_val;

    void              **result_val;

    config_cmd_cb_t     cb;
} config_cmd_t;

typedef struct {
    int             st;
    config_cmd_t   *cmd;
} config_read_state_t;

typedef struct {
    config_read_state_t     state;
    ll_t                    cmd_list;
} config_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t        id;
    ptr_id_t        prev;
    ptr_id_t        next;

    char           *name;
    FILE           *fh;
} config_file_t;

typedef enum {
    S_LINE_INITIAL = 0,
    S_LINE_EMPTY,
    S_LINE_COMMENT,
    S_LINE_CMD,
    S_LINE_CMD_AND_SCALAR,
    S_LINE_INDENTATION,
    S_LINE_MAPPING,
    S_LINE_INDENTATION_IN_LIST,
    S_LINE_MAPPING_START_BLOCK,
    S_LINE_SCALAR_IN_LIST
} read_line_state_t;

typedef struct {
    read_line_state_t   state;

    char               *raw_line;
    int                 nread;

    int                 key_len;
    char               *key;
    int                 val_len;
    char               *val;
} config_line_t;

typedef enum {
    CH_NEWLINE = 0,
    CH_HASH,
    CH_SPACE,
    CH_HYPHEN,
    CH_COLON,
    CH_WORD,
    CH_UNKNOWN
} char_type_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t        id;
    ptr_id_t        prev;
    ptr_id_t        next;

    struct timeval  tm;
} tm_node_t;

typedef struct {
    /* ll_node_t */
    ptr_id_t        id;
    ptr_id_t        prev;
    ptr_id_t        next;

    int             val;
} test_int_node_t;

