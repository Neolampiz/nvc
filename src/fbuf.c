//
//  Copyright (C) 2014-2022  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "fbuf.h"
#include "fastlz.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define SPILL_SIZE 65536
#define BLOCK_SIZE (SPILL_SIZE - (SPILL_SIZE / 16))

#define UNPACK_BE32(b)                                  \
   ((uint32_t)((b)[0] << 24) | (uint32_t)((b)[1] << 16) \
    | (uint32_t)((b)[2] << 8) | (uint32_t)(b)[3])

#define PACK_BE32(u)                            \
   ((u) >> 24) & 0xff, ((u) >> 16) & 0xff,      \
      ((u) >> 8) & 0xff, (u) & 0xff

#if DEBUG
#define ASSERT_AVAIL(f, n) do {                                 \
      if (unlikely((f)->rptr + (n) > (f)->origsz))              \
         fatal_trace("read past end of decompressed file %s",   \
                     f->fname);                                 \
   } while (0);
#else
#define ASSERT_AVAIL(f, n)
#endif

typedef struct {
   unsigned long s1;
   unsigned long s2;
} adler32_t;

typedef struct {
   fbuf_cs_t algo;
   uint32_t  expect;
   union {
      adler32_t adler32;
   } u;
} cs_state_t;

struct _fbuf {
   fbuf_mode_t  mode;
   char        *fname;
   FILE        *file;
   uint8_t     *wbuf;
   size_t       wpend;
   size_t       wtotal;
   uint8_t     *rbuf;
   size_t       rptr;
   size_t       origsz;
   fbuf_t      *next;
   fbuf_t      *prev;
   cs_state_t   checksum;
   fbuf_zip_t   zip;
};

static fbuf_t *open_list = NULL;

static void adler32_update(adler32_t *state, uint8_t *input, size_t length)
{
   // Public domain implementation from
   //   https://github.com/weidai11/cryptopp/blob/master/adler32.cpp

   const unsigned long BASE = 65521;

   unsigned long s1 = state->s1;
   unsigned long s2 = state->s2;

   if (length % 8 != 0) {
      do {
         s1 += *input++;
         s2 += s1;
         length--;
      } while (length % 8 != 0);

      if (s1 >= BASE)
         s1 -= BASE;
      s2 %= BASE;
   }

   while (length > 0) {
      s1 += input[0]; s2 += s1;
      s1 += input[1]; s2 += s1;
      s1 += input[2]; s2 += s1;
      s1 += input[3]; s2 += s1;
      s1 += input[4]; s2 += s1;
      s1 += input[5]; s2 += s1;
      s1 += input[6]; s2 += s1;
      s1 += input[7]; s2 += s1;

      length -= 8;
      input += 8;

      if (s1 >= BASE)
         s1 -= BASE;
      if (length % 0x8000 == 0)
         s2 %= BASE;
   }

   assert(s1 < BASE);
   assert(s2 < BASE);

   state->s1 = s1;
   state->s2 = s2;
}

static void checksum_init(cs_state_t *state, fbuf_cs_t algo)
{
   state->expect = 0;

   switch ((state->algo = algo)) {
   case FBUF_CS_NONE:
      break;
   case FBUF_CS_ADLER32:
      state->u.adler32.s1 = 1;
      state->u.adler32.s2 = 0;
      break;
   }
}

static void checksum_update(cs_state_t *state, uint8_t *input, size_t length)
{
   switch (state->algo) {
   case FBUF_CS_NONE:
      break;
   case FBUF_CS_ADLER32:
      adler32_update(&(state->u.adler32), input, length);
      break;
   }
}

static uint32_t checksum_finish(cs_state_t *state)
{
   switch (state->algo) {
   case FBUF_CS_ADLER32:
      return (state->u.adler32.s1 << 16) | state->u.adler32.s2;
   default:
      return 0;
   }
}

void fbuf_cleanup(void)
{
   for (fbuf_t *it = open_list; it != NULL; it = it->next) {
      fclose(it->file);
      if (it->mode == FBUF_OUT)
         remove(it->fname);
   }
}

static void fbuf_write_raw(fbuf_t *f, const uint8_t *bytes, size_t count)
{
   if (fwrite(bytes, count, 1, f->file) != 1)
      fatal_errno("%s: fwrite", f->fname);
}

static void fbuf_read_raw(fbuf_t *f, uint8_t *bytes, size_t count)
{
   if (fread(bytes, count, 1, f->file) != 1)
      fatal_errno("%s: fread", f->fname);
}

static void fbuf_write_header(fbuf_t *f)
{
   const uint8_t header[16] = {
      'F', 'B', 'U', 'F',     // Magic number "FBUF"
      f->zip,                 // Compression format
      f->checksum.algo,       // Checksum algorithm
      0, 0,                   // Unused
      0, 0, 0, 0,             // Decompressed length
      0, 0, 0, 0,             // Checksum
   };
   fbuf_write_raw(f, header, sizeof(header));
}

static void fbuf_update_header(fbuf_t *f, uint32_t checksum)
{
   struct stat buf;
   if (fstat(fileno(f->file), &buf) != 0)
      fatal_errno("fstat");

   if (S_ISFIFO(buf.st_mode)) {
      // Streaming mode: length and checksum is appended instead
   }
   else if (fseek(f->file, 8, SEEK_SET) != 0)
      fatal_errno("%s: fseek", f->fname);

   const uint8_t bytes[8] = { PACK_BE32(f->wtotal), PACK_BE32(checksum) };
   fbuf_write_raw(f, bytes, 8);
}

static void fbuf_decompress(fbuf_t *f)
{
   uint8_t header[16];
   fbuf_read_raw(f, header, sizeof(header));

   if (memcmp(header, "FBUF", 4))
      fatal("%s: file created with an older version of NVC", f->fname);

   if (header[4] != 'F')
      fatal("%s has was created with unexpected compression algorithm %c",
            f->fname, header[4]);

   if (header[5] != f->checksum.algo)
      fatal("%s has was created with unexpected checksum algorithm %c",
            f->fname, header[5]);

   struct stat buf;
   if (fstat(fileno(f->file), &buf) != 0)
      fatal_errno("fstat");

   size_t bufsz;
   uint8_t *rmap = NULL;
   if (S_ISFIFO(buf.st_mode)) {
      rmap = xmalloc((bufsz = 16384));
      memcpy(rmap, header, sizeof(header));

      size_t wptr = sizeof(header);
      for (;;) {
         const int nr = fread(rmap + wptr, 1, bufsz - wptr, f->file);
         if (nr < 0)
            fatal_errno("%s", f->fname);
         else if (nr == 0)
            break;
         else if (wptr + nr == bufsz)
            rmap = xrealloc(rmap, (bufsz *= 2));

         wptr += nr;
      }

      memcpy(header + 8, rmap + wptr - 8, 8);   // Update header
   }
   else
      rmap = map_file(fileno(f->file), (bufsz = buf.st_size));

   const uint32_t len = UNPACK_BE32(header + 8);
   const uint32_t checksum = UNPACK_BE32(header + 12);

   f->origsz = len;
   f->checksum.expect = checksum;
   f->rbuf = xmalloc(f->origsz);

   for (uint8_t *dst = f->rbuf, *src = rmap + 16; dst < f->rbuf + f->origsz;) {
      const uint32_t blksz = UNPACK_BE32(src);
      if (blksz > SPILL_SIZE)
         fatal("file %s has invalid compression format", f->fname);

      src += sizeof(uint32_t);

      if (src + blksz > (uint8_t *)rmap + bufsz)
         fatal_trace("read past end of compressed file %s", f->fname);

      const int ret = fastlz_decompress(src, blksz, dst, SPILL_SIZE);
      if (ret == 0)
         fatal("file %s has invalid compression format", f->fname);

      checksum_update(&(f->checksum), dst, ret);

      dst += ret;
      src += blksz;
   }

   if (S_ISFIFO(buf.st_mode))
      free(rmap);
   else
      unmap_file(rmap, buf.st_size);
}

static fbuf_t *fbuf_new(FILE *file, char *fname, fbuf_mode_t mode,
                        fbuf_cs_t csum, fbuf_zip_t zip)
{
   fbuf_t *f = xcalloc(sizeof(struct _fbuf));
   f->file  = file;
   f->fname = fname;
   f->mode  = mode;
   f->next  = open_list;
   f->zip   = zip;

   checksum_init(&(f->checksum), csum);

   if (mode == FBUF_OUT) {
      f->wbuf = xmalloc(SPILL_SIZE);
      fbuf_write_header(f);
   }
   else
      fbuf_decompress(f);

   if (open_list != NULL)
      open_list->prev = f;

   return (open_list = f);
}

fbuf_t *fbuf_open(const char *file, fbuf_mode_t mode, fbuf_cs_t csum)
{
   FILE *h = fopen(file, mode == FBUF_OUT ? "wb" : "rb");
   if (h == NULL)
      return NULL;

   return fbuf_new(h, xstrdup(file), mode, csum, FBUF_ZIP_FASTLZ);
}

fbuf_t *fbuf_fdopen(int fd, fbuf_mode_t mode, fbuf_cs_t csum)
{
   FILE *h = fdopen(fd, mode == FBUF_OUT ? "wb" : "rb");
   if (h == NULL)
      return NULL;

   return fbuf_new(h, xasprintf("<fd:%d>", fd), mode, csum, FBUF_ZIP_FASTLZ);
}

const char *fbuf_file_name(fbuf_t *f)
{
   return f->fname;
}

static void fbuf_maybe_flush(fbuf_t *f, size_t more)
{
   assert(more <= BLOCK_SIZE);
   if (f->wpend + more > BLOCK_SIZE) {
      if (f->wpend < 16) {
         // Write dummy bytes at end to meet fastlz block size requirement
         memset(f->wbuf + f->wpend, '\0', 16 - f->wpend);
         f->wpend = 16;
      }

      checksum_update(&(f->checksum), f->wbuf, f->wpend);

      uint8_t out[SPILL_SIZE];
      const int ret = fastlz_compress_level(2, f->wbuf, f->wpend, out);

      assert((ret > 0) && (ret < SPILL_SIZE));

      const uint8_t blksz[4] = { PACK_BE32(ret) };
      fbuf_write_raw(f, blksz, 4);

      fbuf_write_raw(f, out, ret);

      f->wtotal += f->wpend;
      f->wpend = 0;
   }
}

void fbuf_close(fbuf_t *f, uint32_t *checksum)
{
   if (f->wbuf != NULL)
      fbuf_maybe_flush(f, BLOCK_SIZE);

   const uint32_t cs = checksum_finish(&(f->checksum));

   if (f->mode == FBUF_IN && cs != f->checksum.expect)
      fatal("%s: incorrect checksum %08x, expected %08x",
            f->fname, cs, f->checksum.expect);

   if (checksum != NULL)
      *checksum = cs;

   if (f->rbuf != NULL)
      free(f->rbuf);

   if (f->wbuf != NULL) {
      fbuf_update_header(f, cs);
      free(f->wbuf);
   }

   fclose(f->file);

   if (f->prev == NULL) {
      assert(f == open_list);
      if (f->next != NULL)
         f->next->prev = NULL;
      open_list = f->next;
   }
   else {
      f->prev->next = f->next;
      if (f->next != NULL)
         f->next->prev = f->prev;
   }

   if (checksum != NULL)
      *checksum = checksum_finish(&(f->checksum));

   free(f->fname);
   free(f);
}

void fbuf_put_uint(fbuf_t *f, uint64_t val)
{
   uint8_t enc[10];
   int nbytes = 0;

   do {
      enc[nbytes] = val & 0x7f;
      val >>= 7;
      if (val) enc[nbytes] |= 0x80;
      nbytes++;
   } while (val);

   fbuf_maybe_flush(f, nbytes);
   for (int i = 0; i < nbytes; i++)
      *(f->wbuf + f->wpend++) = enc[i];
}

void fbuf_put_int(fbuf_t *f, int64_t val)
{
   uint64_t zz = (val << 1) ^ (val >> 63);   // Zig-zag encoding
   fbuf_put_uint(f, zz);
}

void write_u32(uint32_t u, fbuf_t *f)
{
   fbuf_maybe_flush(f, 4);
   *(f->wbuf + f->wpend++) = (u >>  0) & UINT32_C(0xff);
   *(f->wbuf + f->wpend++) = (u >>  8) & UINT32_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 16) & UINT32_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 24) & UINT32_C(0xff);
}

void write_u64(uint64_t u, fbuf_t *f)
{
   fbuf_maybe_flush(f, 8);
   *(f->wbuf + f->wpend++) = (u >>  0) & UINT64_C(0xff);
   *(f->wbuf + f->wpend++) = (u >>  8) & UINT64_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 16) & UINT64_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 24) & UINT64_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 32) & UINT64_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 40) & UINT64_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 48) & UINT64_C(0xff);
   *(f->wbuf + f->wpend++) = (u >> 56) & UINT64_C(0xff);
}

void write_u16(uint16_t s, fbuf_t *f)
{
   fbuf_maybe_flush(f, 2);
   *(f->wbuf + f->wpend++) = (s >> 0) & UINT16_C(0xff);
   *(f->wbuf + f->wpend++) = (s >> 8) & UINT16_C(0xff);
}

void write_u8(uint8_t u, fbuf_t *f)
{
   fbuf_maybe_flush(f, 1);
   *(f->wbuf + f->wpend++) = u;
}

void write_raw(const void *buf, size_t len, fbuf_t *f)
{
   fbuf_maybe_flush(f, len);
   memcpy(f->wbuf + f->wpend, buf, len);
   f->wpend += len;
}

void write_double(double d, fbuf_t *f)
{
   union { double d; uint64_t i; } u;
   u.d = d;
   write_u64(u.i, f);
}

uint64_t fbuf_get_uint(fbuf_t *f)
{
   uint8_t dec[10];
   int nbytes = 0;

   uint8_t byte;
   do {
      ASSERT_AVAIL(f, 1);
      byte = *(f->rbuf + f->rptr++);
      dec[nbytes++] = byte & 0x7f;
   } while (byte & 0x80);

   uint64_t val = 0;
   for (int i = nbytes - 1; i >= 0; i--) {
      val <<= 7;
      val |= dec[i];
   }

   return val;
}

int64_t fbuf_get_int(fbuf_t *f)
{
   uint64_t zz = fbuf_get_uint(f);
   return (zz >> 1) ^ -(zz & 1);
}

uint32_t read_u32(fbuf_t *f)
{
   ASSERT_AVAIL(f, 4);

   uint32_t val = 0;
   val |= (uint32_t)*(f->rbuf + f->rptr++) << 0;
   val |= (uint32_t)*(f->rbuf + f->rptr++) << 8;
   val |= (uint32_t)*(f->rbuf + f->rptr++) << 16;
   val |= (uint32_t)*(f->rbuf + f->rptr++) << 24;
   return val;
}

uint16_t read_u16(fbuf_t *f)
{
   ASSERT_AVAIL(f, 2);

   uint16_t val = 0;
   val |= (uint16_t)*(f->rbuf + f->rptr++) << 0;
   val |= (uint16_t)*(f->rbuf + f->rptr++) << 8;
   return val;
}

uint8_t read_u8(fbuf_t *f)
{
   ASSERT_AVAIL(f, 1);
   return *(f->rbuf + f->rptr++);
}

uint64_t read_u64(fbuf_t *f)
{
   ASSERT_AVAIL(f, 8);

   uint64_t val = 0;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 0;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 8;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 16;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 24;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 32;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 40;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 48;
   val |= (uint64_t)*(f->rbuf + f->rptr++) << 56;
   return val;
}

void read_raw(void *buf, size_t len, fbuf_t *f)
{
   ASSERT_AVAIL(f, len);
   memcpy(buf, f->rbuf + f->rptr, len);
   f->rptr += len;
}

double read_double(fbuf_t *f)
{
   union { uint64_t i; double d; } u;
   u.i = read_u64(f);
   return u.d;
}
