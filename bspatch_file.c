/*-
 * Copyright 2003-2005 Colin Percival
 * Copyright 2012 Matthew Endsley
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions 
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "bspatch_file.h"

#ifndef MIN
#  define MIN(a,b)                   (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#  define MAX(a,b)                   (((a) > (b)) ? (a) : (b))
#endif

static int64_t offtin(uint8_t *buf)
{
	int64_t y;

	y=buf[7]&0x7F;
	y=y*256;y+=buf[6];
	y=y*256;y+=buf[5];
	y=y*256;y+=buf[4];
	y=y*256;y+=buf[3];
	y=y*256;y+=buf[2];
	y=y*256;y+=buf[1];
	y=y*256;y+=buf[0];

	if(buf[7]&0x80) y=-y;

	return y;
}

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>

#define MAX_STREAM_RW_SIZE      1024		//2x this size is actually used
//#define USE_STATIC_BUFFERS

int bspatch_file( struct bs_rw_file *old_file, struct bs_rw_file *new_file, struct bspatch_stream *patch_stream)
{
	uint8_t buf[8];
	int64_t oldpos,newpos;
	int64_t ctrl[3];
	int64_t i;

	oldpos=0; newpos=0;

	uint32_t oldsize = old_file->size;
	uint32_t newsize = new_file->size;

	int ret;


#ifdef USE_STATIC_BUFFERS
	uint8_t rw_buffer[MAX_STREAM_RW_SIZE];
	uint8_t diff_buffer[MAX_STREAM_RW_SIZE];
#else
	uint8_t* rw_buffer;
	uint8_t* diff_buffer;
	rw_buffer = (uint8_t*)malloc( MAX_STREAM_RW_SIZE );
	diff_buffer = (uint8_t*)malloc( MAX_STREAM_RW_SIZE );
#endif


	while(newpos<newsize) {
		/* Read control data */
		for(i=0;i<=2;i++) {
			if (patch_stream->read(patch_stream, buf, 8)) {
//				return -1;
				ret = -1;
				goto error;
			}
			ctrl[i]=offtin(buf);
		};

		/* Sanity-check */
		if(newpos+ctrl[0]>newsize)
			return -1;


		uint32_t subpos = 0;

		/* Read diff string */
		// To avoid buffering the entire chunk in memory, we will will read/write in smaller pieces
		while( subpos < ctrl[0] )
		{
			int32_t bytes = MIN( ctrl[0] - subpos, MAX_STREAM_RW_SIZE );

			//read patch bytes into working buffer
			if (patch_stream->read(patch_stream, rw_buffer, bytes)) {
                ret = -1;
                goto error;
			}
			// Also read bytes from the old file
			if(old_file->read(old_file, diff_buffer, oldpos + subpos, bytes)) {
                ret = -1;
                goto error;
			}

			// Apply diff to the buffers
			for( int j=0; j < bytes; j++ ){
				rw_buffer[j] += diff_buffer[j];
			}

			// write bytes out to the file being constructed
			if ( new_file->write( new_file, rw_buffer, newpos + subpos, bytes )) {
                ret = -1;
                goto error;
			}

			subpos += bytes;
		}

		/* Adjust pointers */
		newpos+=ctrl[0];
		oldpos+=ctrl[0];

		/* Sanity-check */
		if(newpos+ctrl[1]>newsize) {
            ret = -1;
            goto error;
        }

		/* Read extra string */
        subpos  = 0;
		while( subpos < ctrl[1]) {

            int32_t bytes = MIN( ctrl[1] - subpos, MAX_STREAM_RW_SIZE );

			if (patch_stream->read(patch_stream, rw_buffer, bytes)) {
                ret = -1;
                goto error;
			}

			// write bytes out
			// TODO: does the write stream encode oldsize?
			if (new_file->write( new_file, rw_buffer, newpos + subpos, bytes ) ){
                ret = -1;
                goto error;
			}

            subpos += bytes;
		}

		/* Adjust pointers */
		newpos+=ctrl[1];
		oldpos+=ctrl[2];
	};

	ret = 0;

error:
#ifndef USE_STATIC_BUFFERS
    if( rw_buffer ) {
        free( rw_buffer );
    }
    if( diff_buffer ) {
        free( diff_buffer );
    }
#endif
	return ret;
}

#if defined(BSPATCH_FILE_EXECUTABLE)

// #include <bzlib.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

int read_old_file(const struct bs_rw_file *stream, void *buffer, int offset, int length);
int write_new_file(const struct bs_rw_file *stream, void *buffer, int offset, int length);

// static int bz2_read(const struct bspatch_stream* stream, void* buffer, int length)
// {
// 	int n;
// 	int bz2err;
// 	BZFILE* bz2;

// 	bz2 = (BZFILE*)stream->opaque;
// 	n = BZ2_bzRead(&bz2err, bz2, buffer, length);
// 	if (n != length)
// 		return -1;

// 	return 0;
// }

static int patch_read(const struct bspatch_stream* stream, void* buffer, int length)
{
	int n;
	int bz2err;
	FILE* f = (FILE*)stream->opaque;

	n = fread( buffer, 1, length, f );

	if (n != length)
		return -1;

	return 0;
}

int read_old_file(const struct bs_rw_file *stream, void *buffer, int offset, int length)
{
    int ret;
    FILE* f = (FILE*)stream->opaque;
    ret = fseek( f, offset, SEEK_SET);
    if( ret != 0){
        printf("old_file seek error\n");
        return -1;
    }

    ret = fread( buffer, 1, length, f );
    if( ret <= 0){
        printf("old_file read error\n");
        return -1;
    }
    return 0;
}

int write_new_file(const struct bs_rw_file *stream, void *buffer, int offset, int size)
{
    int ret;
    FILE* f = (FILE*)stream->opaque;
    ret = fseek( f, offset, SEEK_SET);
    if( ret != 0){
        printf("recon_file seek error\n");
        return -1;
    }

    ret = fwrite( buffer, 1, size, f );
    if( ret <= 0){
        printf("error\n");
        return -1;
    }
    return 0;
}

int main(int argc,char * argv[])
{
	FILE  *f_old, *f_new, *f_patch;
	int fd;
	int ret;
//	int bz2err;
	uint8_t header[24];
	uint8_t *old, *new;
	int64_t oldsize, newsize;
//	BZFILE* bz2;
	struct bspatch_stream stream;
	struct stat sb;

	if(argc!=4) errx(1,"usage: %s oldfile newfile patchfile\n",argv[0]);

	/* Open patch file */
	if ((f_patch = fopen(argv[3], "r")) == NULL)
		err(1, "fopen(%s)", argv[3]);

	/* Read header */
	if (fread(header, 1, 24, f_patch) != 24) {
		if (feof(f_patch))
			errx(1, "Corrupt patch\n");
		err(1, "fread(%s)", argv[3]);
	}

	/* Check for appropriate magic */
	if (memcmp(header, "ENDSLEY/BSDIFF43", 16) != 0)
		errx(1, "Corrupt patch\n");

	/* Read lengths from header */
	newsize=offtin(header+16);
	if(newsize<0)
		errx(1,"Corrupt patch\n");

	/* Close patch file and re-open it via libbzip2 at the right places */
	if(((fd=open(argv[1],O_RDONLY,0))<0) ||
		((oldsize=lseek(fd,0,SEEK_END))==-1) ||
		((old=malloc(oldsize+1))==NULL) ||
		(lseek(fd,0,SEEK_SET)!=0) ||
		(read(fd,old,oldsize)!=oldsize) ||
		(fstat(fd, &sb)) ||
		(close(fd)==-1)) err(1,"%s",argv[1]);
	if((new=malloc(newsize+1))==NULL) err(1,NULL);

//	if (NULL == (bz2 = BZ2_bzReadOpen(&bz2err, f, 0, 0, NULL, 0)))
//		errx(1, "BZ2_bzReadOpen, bz2err=%d", bz2err);

//	stream.read = bz2_read;
//	stream.opaque = bz2;
	stream.read = patch_read;
	stream.opaque = f_patch;

    struct bs_rw_file old_rw_stream;
    struct bs_rw_file new_rw_stream;

    f_old = fopen(argv[1], "rb" );
    f_new = fopen(argv[2], "wb" );

    old_rw_stream.opaque = f_old;
    old_rw_stream.read = read_old_file;
    old_rw_stream.write = NULL;
    old_rw_stream.size = oldsize;

    new_rw_stream.opaque = f_new;
    new_rw_stream.read = NULL;
    new_rw_stream.write = write_new_file;
    new_rw_stream.size = newsize;

    ret = bspatch_file( &old_rw_stream, &new_rw_stream, &stream);
	if ( ret )
		errx(1, "bspatch");

	/* Clean up the bzip2 reads */
//	BZ2_bzReadClose(&bz2err, bz2);
    fclose(f_patch);

	fclose(f_old);

	/* Write the new file */
	if(((fd=open(argv[2],O_CREAT|O_TRUNC|O_WRONLY,sb.st_mode))<0) ||
		(write(fd,new,newsize)!=newsize) || (close(fd)==-1))
		err(1,"%s",argv[2]);

	free(new);
	free(old);

	return 0;
}

#endif
