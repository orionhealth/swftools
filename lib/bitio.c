/*  bitio.c 
    part of swftools
    implementation of bitio.h.

    Copyright (C) 2003 Matthias Kramm <kramm@quiss.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_IO_H
#include <io.h>
#endif
#include <string.h>
#include <memory.h>
#include <fcntl.h>

#include "../config.h"

#ifdef HAVE_ZLIB
#include <zlib.h>
#define ZLIB_BUFFER_SIZE 16384
#endif
#include "./bitio.h"

/* ---------------------------- null reader ------------------------------- */

static int reader_nullread(reader_t*r, void* data, int len) 
{
    memset(data, 0, len);
    return len;
}
static void reader_nullread_dealloc(reader_t*r)
{
    memset(r, 0, sizeof(reader_t));
}
void reader_init_nullreader(reader_t*r)
{
    r->read = reader_nullread;
    r->dealloc = reader_nullread_dealloc;
    r->internal = 0;
    r->type = READER_TYPE_NULL;
    r->mybyte = 0;
    r->bitpos = 8;
    r->pos = 0;
}
/* ---------------------------- file reader ------------------------------- */

static int reader_fileread(reader_t*reader, void* data, int len) 
{
    int ret = read((int)reader->internal, data, len);
    if(ret>=0)
	reader->pos += ret;
    return ret;
}
void reader_init_filereader(reader_t*r, int handle)
{
    r->read = reader_fileread;
    r->internal = (void*)handle;
    r->type = READER_TYPE_FILE;
    r->mybyte = 0;
    r->bitpos = 8;
    r->pos = 0;
}

/* ---------------------------- mem reader ------------------------------- */

struct memread_t
{
    unsigned char*data;
    int length;
};
static int reader_memread(reader_t*reader, void* data, int len) 
{
    struct memread_t*mr = (struct memread_t*)reader->internal;

    if(mr->length - reader->pos > len) {
	memcpy(data, &mr->data[reader->pos], len);
	reader->pos += len;
	return len;
    } else {
	memcpy(data, &mr->data[reader->pos], mr->length - reader->pos);
	reader->pos += mr->length;
	return mr->length - reader->pos;
    }
}
static void reader_memread_dealloc(reader_t*reader)
{
    if(reader->internal)
	free(reader->internal);
    memset(reader, 0, sizeof(reader_t));
}
void reader_init_memreader(reader_t*r, void*newdata, int newlength)
{
    struct memread_t*mr = (struct memread_t*)malloc(sizeof(struct memread_t));
    mr->data = (unsigned char*)newdata;
    mr->length = newlength;
    r->read = reader_memread;
    r->dealloc = reader_memread_dealloc;
    r->internal = (void*)mr;
    r->type = READER_TYPE_MEM;
    r->mybyte = 0;
    r->bitpos = 8;
    r->pos = 0;
} 

/* ---------------------------- mem writer ------------------------------- */

struct memwrite_t
{
    unsigned char*data;
    int length;
};

static int writer_memwrite_write(writer_t*w, void* data, int len) 
{
    struct memwrite_t*mw = (struct memwrite_t*)w->internal;
    if(mw->length - w->pos > len) {
	memcpy(&mw->data[w->pos], data, len);
	w->pos += len;
	return len;
    } else {
	memcpy(&mw->data[w->pos], data, mw->length - w->pos);
	w->pos = mw->length;
	return mw->length - w->pos;
    }
}
static void writer_memwrite_finish(writer_t*w)
{
    if(w->internal) 
	free(w->internal);
    w->internal = 0;
}
void writer_init_memwriter(writer_t*w, void*data, int len)
{
    struct memwrite_t *mr;
    mr = (struct memwrite_t*)malloc(sizeof(struct memwrite_t));
    mr->data = (unsigned char *)data;
    mr->length = len;
    memset(w, 0, sizeof(writer_t));
    w->write = writer_memwrite_write;
    w->finish = writer_memwrite_finish;
    w->internal = (void*)mr;
    w->type = WRITER_TYPE_MEM;
    w->bitpos = 0;
    w->mybyte = 0;
    w->pos = 0;
}

/* ------------------------- growing mem writer ------------------------------- */

struct growmemwrite_t
{
    unsigned char*data;
    int length;
    U32 grow;
};
static int writer_growmemwrite_write(writer_t*w, void* data, int len) 
{
    struct growmemwrite_t*mw = (struct growmemwrite_t*)w->internal;
    if(!mw->data) {
	fprintf(stderr, "Illegal write operation: data already given away");
	exit(1);
    }
    if(mw->length - w->pos < len) {
	unsigned char*newmem;
	int newlength = mw->length;
	while(newlength - w->pos < len) {
	    newlength += mw->grow;
	}
#ifdef NO_REALLOC
	newmem = (unsigned char*)malloc(newlength);
	memcpy(newmem, mw->data, mw->length);
	free(mw->data);
	mw->data = newmem;
#else
	mw->data = (unsigned char*)realloc(mw->data, newlength);
#endif
	mw->length = newlength;
    }
    memcpy(&mw->data[w->pos], data, len);
    w->pos += len;
    return len;
}
static void writer_growmemwrite_finish(writer_t*w)
{
    struct growmemwrite_t*mw = (struct growmemwrite_t*)w->internal;
    if(mw->data)
	free(mw->data);
    mw->data = 0;
    mw->length = 0;
    free(w->internal);mw=0;
    memset(w, 0, sizeof(writer_t));
}
void* writer_growmemwrite_getmem(writer_t*w)
{
    struct growmemwrite_t*mw = (struct growmemwrite_t*)w->internal;
    void*ret = mw->data;
    /* remove own reference so that neither write() nor finish() can free it.
       It's property of the caller now.
    */
    mw->data = 0;
    return ret;
}
void writer_init_growingmemwriter(writer_t*w, U32 grow)
{
    struct growmemwrite_t *mr;
    mr = (struct growmemwrite_t *)malloc(sizeof(struct growmemwrite_t));
    mr->length = 4096;
    mr->data = (unsigned char *)malloc(mr->length);
    mr->grow = grow;
    memset(w, 0, sizeof(writer_t));
    w->write = writer_growmemwrite_write;
    w->finish = writer_growmemwrite_finish;
    w->internal = (void*)mr;
    w->type = WRITER_TYPE_GROWING_MEM;
    w->bitpos = 0;
    w->mybyte = 0;
    w->pos = 0;
}

/* ---------------------------- file writer ------------------------------- */

struct filewrite_t
{
    int handle;
    char free_handle;
};

static int writer_filewrite_write(writer_t*w, void* data, int len) 
{
    struct filewrite_t * fw= (struct filewrite_t*)w->internal;
    return write(fw->handle, data, len);
}
static void writer_filewrite_finish(writer_t*w)
{
    struct filewrite_t *mr = (struct filewrite_t*)w->internal;
    if(mr->free_handle)
	close(mr->handle);
    free(w->internal);
    memset(w, 0, sizeof(writer_t));
}
void writer_init_filewriter(writer_t*w, int handle)
{
    struct filewrite_t *mr = (struct filewrite_t *)malloc(sizeof(struct filewrite_t));
    mr->handle = handle;
    mr->free_handle = 0;
    memset(w, 0, sizeof(writer_t));
    w->write = writer_filewrite_write;
    w->finish = writer_filewrite_finish;
    w->internal = mr;
    w->type = WRITER_TYPE_FILE;
    w->bitpos = 0;
    w->mybyte = 0;
    w->pos = 0;
}
void writer_init_filewriter2(writer_t*w, char*filename)
{
    int fi = open("movie.swf",
#ifdef O_BINARY
	    O_BINARY|
#endif
	    O_WRONLY|O_CREAT|O_TRUNC, 0644);
    writer_init_filewriter(w, fi);
    ((struct filewrite_t*)w->internal)->free_handle = 1;
}

/* ---------------------------- null writer ------------------------------- */

static int writer_nullwrite_write(writer_t*w, void* data, int len) 
{
    w->pos += len;
    return len;
}
static void writer_nullwrite_finish(writer_t*w)
{
    memset(w, 0, sizeof(writer_t));
}
void writer_init_nullwriter(writer_t*w)
{
    memset(w, 0, sizeof(writer_t));
    w->write = writer_nullwrite_write;
    w->finish = writer_nullwrite_finish;
    w->internal = 0;
    w->type = WRITER_TYPE_NULL;
    w->bitpos = 0;
    w->mybyte = 0;
    w->pos = 0;
}
/* ---------------------------- zlibinflate reader -------------------------- */

struct zlibinflate_t
{
#ifdef HAVE_ZLIB
    z_stream zs;
    reader_t*input;
    unsigned char readbuffer[ZLIB_BUFFER_SIZE];
#endif
};

#ifdef HAVE_ZLIB
static void zlib_error(int ret, char* msg, z_stream*zs)
{
    fprintf(stderr, "%s: zlib error (%d): last zlib error: %s\n",
	  msg,
	  ret,
	  zs->msg?zs->msg:"unknown");
    perror("errno:");
    exit(1);
}
#endif

static int reader_zlibinflate(reader_t*reader, void* data, int len) 
{
#ifdef HAVE_ZLIB
    struct zlibinflate_t*z = (struct zlibinflate_t*)reader->internal;
    int ret;
    if(!z) {
	return 0;
    }
    if(!len)
	return 0;
    
    z->zs.next_out = (Bytef *)data;
    z->zs.avail_out = len;

    while(1) {
	if(!z->zs.avail_in) {
	    z->zs.avail_in = z->input->read(z->input, z->readbuffer, ZLIB_BUFFER_SIZE);
	    z->zs.next_in = z->readbuffer;
	}
	if(z->zs.avail_in)
	    ret = inflate(&z->zs, Z_NO_FLUSH);
	else
	    ret = inflate(&z->zs, Z_FINISH);
    
	if (ret != Z_OK &&
	    ret != Z_STREAM_END) zlib_error(ret, "bitio:inflate_inflate", &z->zs);

	if (ret == Z_STREAM_END) {
		int pos = z->zs.next_out - (Bytef*)data;
		ret = inflateEnd(&z->zs);
		if (ret != Z_OK) zlib_error(ret, "bitio:inflate_end", &z->zs);
		free(reader->internal);
		reader->internal = 0;
		reader->pos += pos;
		return pos;
	}
	if(!z->zs.avail_out) {
	    break;
	}
    }
    reader->pos += len;
    return len;
#else
    fprintf(stderr, "Error: swftools was compiled without zlib support");
    exit(1);
#endif
}
static void reader_zlibinflate_dealloc(reader_t*reader)
{
#ifdef HAVE_ZLIB
    struct zlibinflate_t*z = (struct zlibinflate_t*)reader->internal;
    /* test whether read() already did basic deallocation */
    if(reader->internal) {
	inflateEnd(&z->zs);
	free(reader->internal);
    }
    memset(reader, 0, sizeof(reader_t));
#endif
}
void reader_init_zlibinflate(reader_t*r, reader_t*input)
{
#ifdef HAVE_ZLIB
    struct zlibinflate_t*z;
    int ret;
    memset(r, 0, sizeof(reader_t));
    z = (struct zlibinflate_t*)malloc(sizeof(struct zlibinflate_t));
    memset(z, 0, sizeof(struct zlibinflate_t));
    r->internal = z;
    r->read = reader_zlibinflate;
    r->dealloc = reader_zlibinflate_dealloc;
    r->type = READER_TYPE_ZLIB;
    r->pos = 0;
    z->input = input;
    memset(&z->zs,0,sizeof(z_stream));
    z->zs.zalloc = Z_NULL;
    z->zs.zfree  = Z_NULL;
    z->zs.opaque = Z_NULL;
    ret = inflateInit(&z->zs);
    if (ret != Z_OK) zlib_error(ret, "bitio:inflate_init", &z->zs);
    reader_resetbits(r);
#else
    fprintf(stderr, "Error: swftools was compiled without zlib support");
    exit(1);
#endif
}

/* ---------------------------- zlibdeflate writer -------------------------- */

struct zlibdeflate_t
{
#ifdef HAVE_ZLIB
    z_stream zs;
    writer_t*output;
    unsigned char writebuffer[ZLIB_BUFFER_SIZE];
#endif
};

static int writer_zlibdeflate_write(writer_t*writer, void* data, int len) 
{
#ifdef HAVE_ZLIB
    struct zlibdeflate_t*z = (struct zlibdeflate_t*)writer->internal;
    int ret;
    if(writer->type != WRITER_TYPE_ZLIB) {
	fprintf(stderr, "Wrong writer ID (writer not initialized?)\n");
	return 0;
    }
    if(!z) {
	fprintf(stderr, "zlib not initialized!\n");
	return 0;
    }
    if(!len)
	return 0;
    
    z->zs.next_in = (Bytef *)data;
    z->zs.avail_in = len;

    while(1) {
	ret = deflate(&z->zs, Z_NO_FLUSH);
	
	if (ret != Z_OK) zlib_error(ret, "bitio:deflate_deflate", &z->zs);

	if(z->zs.next_out != z->writebuffer) {
	    z->output->write(z->output, z->writebuffer, z->zs.next_out - (Bytef*)z->writebuffer);
	    z->zs.next_out = z->writebuffer;
	    z->zs.avail_out = ZLIB_BUFFER_SIZE;
	}

	if(!z->zs.avail_in) {
	    break;
	}
    }
    writer->pos += len;
    return len;
#else
    fprintf(stderr, "Error: swftools was compiled without zlib support");
    exit(1);
#endif
}
static void writer_zlibdeflate_finish(writer_t*writer)
{
#ifdef HAVE_ZLIB
    struct zlibdeflate_t*z = (struct zlibdeflate_t*)writer->internal;
    writer_t*output;
    int ret;
    if(writer->type != WRITER_TYPE_ZLIB) {
	fprintf(stderr, "Wrong writer ID (writer not initialized?)\n");
	return;
    }
    if(!z)
	return;
    output= z->output;
    while(1) {
	ret = deflate(&z->zs, Z_FINISH);
	if (ret != Z_OK &&
	    ret != Z_STREAM_END) zlib_error(ret, "bitio:deflate_finish", &z->zs);

	if(z->zs.next_out != z->writebuffer) {
	    z->output->write(z->output, z->writebuffer, z->zs.next_out - (Bytef*)z->writebuffer);
	    z->zs.next_out = z->writebuffer;
	    z->zs.avail_out = ZLIB_BUFFER_SIZE;
	}

	if (ret == Z_STREAM_END) {
	    break;

	}
    }
    ret = deflateEnd(&z->zs);
    if (ret != Z_OK) zlib_error(ret, "bitio:deflate_end", &z->zs);
    free(writer->internal);
    memset(writer, 0, sizeof(writer_t));
    //output->finish(output); 
#else
    fprintf(stderr, "Error: swftools was compiled without zlib support");
    exit(1);
#endif
}
void writer_init_zlibdeflate(writer_t*w, writer_t*output)
{
#ifdef HAVE_ZLIB
    struct zlibdeflate_t*z;
    int ret;
    memset(w, 0, sizeof(writer_t));
    z = (struct zlibdeflate_t*)malloc(sizeof(struct zlibdeflate_t));
    memset(z, 0, sizeof(struct zlibdeflate_t));
    w->internal = z;
    w->write = writer_zlibdeflate_write;
    w->finish = writer_zlibdeflate_finish;
    w->type = WRITER_TYPE_ZLIB;
    w->pos = 0;
    z->output = output;
    memset(&z->zs,0,sizeof(z_stream));
    z->zs.zalloc = Z_NULL;
    z->zs.zfree  = Z_NULL;
    z->zs.opaque = Z_NULL;
    ret = deflateInit(&z->zs, 9);
    if (ret != Z_OK) zlib_error(ret, "bitio:deflate_init", &z->zs);
    w->bitpos = 0;
    w->mybyte = 0;
    z->zs.next_out = z->writebuffer;
    z->zs.avail_out = ZLIB_BUFFER_SIZE;
#else
    fprintf(stderr, "Error: swftools was compiled without zlib support");
    exit(1);
#endif
}

/* ----------------------- bit handling routines -------------------------- */

void writer_writebit(writer_t*w, int bit)
{    
    if(w->bitpos==8) 
    {
        w->write(w, &w->mybyte, 1);
	w->bitpos = 0;
	w->mybyte = 0;
    }
    if(bit&1)
	w->mybyte |= 1 << (7 - w->bitpos);
    w->bitpos ++;
}
void writer_writebits(writer_t*w, unsigned int data, int bits)
{
    int t;
    for(t=0;t<bits;t++)
    {
	writer_writebit(w, (data >> (bits-t-1))&1);
    }
}
void writer_resetbits(writer_t*w)
{
    if(w->bitpos)
	w->write(w, &w->mybyte, 1);
    w->bitpos = 0;
    w->mybyte = 0;
}
 
unsigned int reader_readbit(reader_t*r)
{
    if(r->bitpos==8) 
    {
	r->bitpos=0;
        r->read(r, &r->mybyte, 1);
    }
    return (r->mybyte>>(7-r->bitpos++))&1;
}
unsigned int reader_readbits(reader_t*r, int num)
{
    int t;
    int val = 0;
    for(t=0;t<num;t++)
    {
	val<<=1;
	val|=reader_readbit(r);
    }
    return val;
}
void reader_resetbits(reader_t*r)
{
    r->mybyte = 0;
    r->bitpos = 8;

}

U8 reader_readU8(reader_t*r)
{
    U8 b;
    r->read(r, &b, 1);
    return b;
}
U16 reader_readU16(reader_t*r)
{
    U8 b1,b2;
    r->read(r, &b1, 1);
    r->read(r, &b2, 1);
    return b1|b2<<8;
}
U32 reader_readU32(reader_t*r)
{
    U8 b1,b2,b3,b4;
    r->read(r, &b1, 1);
    r->read(r, &b2, 1);
    r->read(r, &b3, 1);
    r->read(r, &b4, 1);
    return b1|b2<<8|b3<<16|b4<<24;
}
float reader_readFloat(reader_t*r)
{
    U8 b1,b2,b3,b4;
    r->read(r, &b1, 1);
    r->read(r, &b2, 1);
    r->read(r, &b3, 1);
    r->read(r, &b4, 1);
    U32 w = (b1|b2<<8|b3<<16|b4<<24);
    return *(float*)&w;
}
double reader_readDouble(reader_t*r)
{
    double f;
    r->read(r, &f, 8);
    return f;

    U8 b[8];
    r->read(r, b, 8);
    U64 w = ((U64)b[0]|(U64)b[1]<<8|(U64)b[2]<<16|(U64)b[3]<<24|(U64)b[4]<<32|(U64)b[5]<<40|(U64)b[6]<<48|(U64)b[7]<<56);
    return *(double*)&w;
}
char*reader_readString(reader_t*r)
{
    writer_t g;
    writer_init_growingmemwriter(&g, 16);
    while(1) {
	U8 b = reader_readU8(r);
	writer_writeU8(&g, b);
	if(!b)
	    break;
    }
    char*string = (char*)writer_growmemwrite_getmem(&g);
    writer_growmemwrite_finish(&g);
    return string;
}

void writer_writeString(writer_t*w, const char*s)
{
    int l = strlen(s);
    char zero = 0;
    w->write(w, (void*)s, l);
    w->write(w, &zero, 1);
}
void writer_writeU8(writer_t*w, unsigned char b)
{
    w->write(w, &b, 1);
}
void writer_writeU16(writer_t*w, unsigned short v)
{
    unsigned char b1 = v;
    unsigned char b2 = v>>8;
    w->write(w, &b1, 1);
    w->write(w, &b2, 1);
}
void writer_writeU32(writer_t*w, unsigned long v)
{
    unsigned char b1 = v;
    unsigned char b2 = v>>8;
    unsigned char b3 = v>>16;
    unsigned char b4 = v>>24;
    w->write(w, &b1, 1);
    w->write(w, &b2, 1);
    w->write(w, &b3, 1);
    w->write(w, &b4, 1);
}
void writer_writeFloat(writer_t*w, float f)
{
    unsigned long v = *(unsigned long*)&f;
    unsigned char b1 = v;
    unsigned char b2 = v>>8;
    unsigned char b3 = v>>16;
    unsigned char b4 = v>>24;
    w->write(w, &b1, 1);
    w->write(w, &b2, 1);
    w->write(w, &b3, 1);
    w->write(w, &b4, 1);
}
void writer_writeDouble(writer_t*w, double f)
{
    w->write(w, &f, 8);
    return;

    unsigned long long v = *(unsigned long long*)&f;
    unsigned char b1 = v;
    unsigned char b2 = v>>8;
    unsigned char b3 = v>>16;
    unsigned char b4 = v>>24;
    unsigned char b5 = v>>32;
    unsigned char b6 = v>>40;
    unsigned char b7 = v>>48;
    unsigned char b8 = v>>56;
    w->write(w, &b1, 1);
    w->write(w, &b2, 1);
    w->write(w, &b3, 1);
    w->write(w, &b4, 1);
    w->write(w, &b5, 1);
    w->write(w, &b6, 1);
    w->write(w, &b7, 1);
    w->write(w, &b8, 1);
}
