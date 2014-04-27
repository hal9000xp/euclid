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
#include <errno.h>
#include "crc32.h"

int main( int argc, char **argv )
{
    FILE           *fh;
    char           *buf;
    long            lsize;
    int             size;
    unsigned int    crc32;

    if( argc != 2 )
    {
        printf( "wrong arg\n" );
        return 0;
    }

    errno = 0;
    fh = fopen( argv[1], "r" );

    if( !fh )
    {
        printf( "fopen err: %s\n", strerror( errno ) );

        return 0;
    }

    errno = 0;

    fseek( fh, 0, SEEK_END );

    lsize = ftell( fh );

    rewind( fh );

    if( errno )
    {
        printf( "fh lsize err: %s\n", strerror( errno ) );

        return 0;
    }

    if( lsize > 1024*1024*1024 )
    {
        printf( "lsize too long: %ld", lsize );

        return 0;
    }

    size = (int) lsize;

    buf = malloc( size );

    errno = 0;
    if( fread( buf, size, 1, fh ) != 1 )
    {
        printf( "fread err: %s\n", strerror( errno ) );

        return 0;
    }

    crc32 = xcrc32( (unsigned char *) buf, size, CRC_INIT );

    printf( "xcrc32 == 0x%.8x\n", crc32 );

    return 0;
}
