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

typedef int64_t                     fxp_num_t;

#define FXP_DEFAULT_PREC            1000000

#define FXP_MAKE_VP( i, f, p )      ( (i) < 0 ? ((i) * (p)) - (f) : \
                                                ((i) * (p)) + (f) )

#define FXP_INT_VP( x, p )          ( ( (x) - ((x) % (p)) ) / (p) )

#define FXP_FRAC_VP( x, p )         ( (x) < 0 ? -((x) % (p)) : (x) % (p) )

#define FXP_MUL_VP( x, y, p )       ( ((x) * (y)) / (p) )

#define FXP_DIV_VP( x, y, p )       ( ((x) * (p)) / (y) )

#define FXP_CONV_PREC( f, fp, tp )  ( (fp) < (tp) ? (f) * ((tp) / (fp)) : \
                                                    (f) / ((fp) / (tp)) )

#define FXP_MAKE( i, f )            FXP_MAKE_VP( i, f, FXP_DEFAULT_PREC )
#define FXP_INT( x )                FXP_INT_VP( x, FXP_DEFAULT_PREC )
#define FXP_FRAC( x )               FXP_FRAC( x, FXP_DEFAULT_PREC )
#define FXP_MUL( x, y )             FXP_MUL_VP( x, y, FXP_DEFAULT_PREC )
#define FXP_DIV( x, y )             FXP_DIV_VP( x, y, FXP_DEFAULT_PREC )
#define FXP_2PREC( f, p )           FXP_CONV_PREC( f, p, FXP_DEFAULT_PREC )

#define str2fxp( fxp, i_part, f_part, s, len )  str2fxp_vp( fxp,               \
                                                            i_part, f_part,    \
                                                            s, len,            \
                                                            FXP_DEFAULT_PREC )

typedef enum {
    FXP_OK = 0,
    FXP_LOST_PREC,
    FXP_ERR
} fxp_rc_t;

int fxp_prec2digits( int prec );

int fxp_digits2prec( int digits );

fxp_rc_t str2fxp_vp( fxp_num_t     *fxp,
                     int64_t       *i_part,
                     int64_t       *f_part,
                     char          *s,
                     int            len,
                     int            max_prec );

