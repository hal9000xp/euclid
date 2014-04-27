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

extern FILE                *G_log_fh;

extern char                *cfg_logger_logfile;
extern struct timeval      *cfg_logger_rotate_interval;
extern struct timeval      *cfg_logger_debug_rotate_interval;

#ifdef  DEBUG
extern bool                 G_logger_no_debug;
#endif

#define LOGT(level, fmt, ...) fprintf(G_log_fh, "%ld:%ld %.8s %s:%d %s " level \
                                  fmt "\n", G_now.tv_sec, G_now.tv_usec,       \
                                  G_gitrev, __FILE__, __LINE__, __FUNCTION__,  \
                                  ##__VA_ARGS__)

#define LOG( fmt, ... )     LOGT( "NOTICE => ", fmt,  ##__VA_ARGS__ )

#define LOGE( fmt, ... )    LOGT( "ERROR => ",  fmt,  ##__VA_ARGS__ )

#define LOGA( cond )        LOGT( "ALERT => ",  "%s", #cond )

#ifdef  DEBUG

#define LOGD( fmt, ... )    if( !G_logger_no_debug ) \
                                LOGT( "DEBUG => ",  fmt,  ##__VA_ARGS__ )

#else

#define LOGD( fmt, ... )    /* do nothing */

#endif

void logger_init();

