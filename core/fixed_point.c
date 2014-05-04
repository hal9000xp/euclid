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
#include "fixed_point.h"

int fxp_prec2digits( int prec )
{
    switch( prec )
    {
        case 10:
            return 1;

        case 100:
            return 2;

        case 1000:
            return 3;

        case 10000:
            return 4;

        case 100000:
            return 5;

        case 1000000:
            return 6;

        default:
            return -1;
    }
}

int fxp_digits2prec( int digits )
{
    switch( digits )
    {
        case 1:
            return 10;

        case 2:
            return 100;

        case 3:
            return 1000;

        case 4:
            return 10000;

        case 5:
            return 100000;

        case 6:
            return 1000000;

        default:
            return -1;
    }
}

fxp_rc_t str2fxp_vp( fxp_num_t     *fxp,
                     int64_t       *i_part,
                     int64_t       *f_part,
                     char          *s,
                     int            len,
                     int            max_prec )
{
    c_assert( fxp && s && len > 0 && max_prec > 0 );

    *fxp = 0;

    if( i_part )
        *i_part = 0;

    if( f_part )
        *f_part = 0;

    enum {
        S_FXP_INT = 0,
        S_FXP_FRAC
    };

    int64_t     ipart = 0;
    int64_t     fpart = 0;
    int         digits = 0;

    int         prec;
    int         max_digits;

    int         state = S_FXP_INT;
    bool        i_has_num = false;
    int         frac_begin = 0;
    fxp_rc_t    rc = FXP_OK;
    int         i;
    char        c;

    max_digits = fxp_prec2digits( max_prec );
    c_assert( max_digits >= 1 );

    for( i = 0; i < len; i++ )
    {
        c = s[i];

        switch( state )
        {
            case S_FXP_INT:

                if( c == '.' )
                {
                    if( i )
                    {
                        if( !i_has_num )
                            return FXP_ERR;

                        char    i_buf[len + 1];

                        memcpy( i_buf, s, i );
                        i_buf[i] = '\0';

                        ipart = atoi( i_buf );

                        state = S_FXP_FRAC;
                    }
                    else
                    {
                        ipart = 0;

                        state = S_FXP_FRAC;
                    }
                }
                else
                if( c == '-' )
                {
                    if( i )
                        return FXP_ERR;
                }
                else
                if( c >= '0' && c <= '9' )
                {
                    i_has_num = true;
                }
                else
                    return FXP_ERR;

                break;

            case S_FXP_FRAC:

                if( !(c >= '0' && c <= '9') )
                    return FXP_ERR;

                if( !frac_begin )
                    frac_begin = i;

                if( i == len - 1 )
                {
                    char f_buf[len + 1];
                    int  f_len = (i - frac_begin) + 1;

                    if( f_len > max_digits )
                    {
                        f_len = max_digits;

                        rc = FXP_LOST_PREC;
                    }

                    digits = f_len;

                    memcpy( f_buf, &s[frac_begin], f_len );
                    f_buf[f_len] = '\0';

                    fpart = atoi( f_buf );

                    c_assert( fpart >= 0 );
                }

                break;

            default:

                c_assert( false );
        }
    }

    if( digits )
    {
        prec = fxp_digits2prec( digits );
        c_assert( prec >= 10 );

        fpart = FXP_CONV_PREC( fpart, prec, max_prec );
    }

    *fxp = FXP_MAKE_VP( ipart, fpart, max_prec );

    if( i_part )
        *i_part = ipart;

    if( f_part )
        *f_part = fpart;

    LOGD( "str:%.*s i_part:%lld f_part:%lld fxp:%lld rc:%d",
          len, s, FXP_FMT( ipart ), FXP_FMT( fpart ),
          FXP_FMT( *fxp ), rc );

    return rc;
}

