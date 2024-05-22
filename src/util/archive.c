/*
 * Copyright 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "archive.h"

#include <assert.h>
#include <string.h>

/* A tar archive contain a sequence of files, each files is composed of a
 * sequence of fixed size records.  The first record of a file has header,
 * defined by the table below:
 *
 *     Field Name   Byte Offset     Length in Bytes Field Type
 *     name         0               100             NUL-terminated if NUL fits
 *     mode         100             8
 *     uid          108             8
 *     gid          116             8
 *     size         124             12
 *     mtime        136             12
 *     chksum       148             8
 *     typeflag     156             1               see below
 *     linkname     157             100             NUL-terminated if NUL fits
 *     magic        256             6               must be TMAGIC (NUL term.)
 *     version      263             2               must be TVERSION
 *     uname        265             32              NUL-terminated
 *     gname        297             32              NUL-terminated
 *     devmajor     329             8
 *     devminor     337             8
 *     prefix       345             155             NUL-terminated if NUL fits
 *
 * The subsequent records contain the file contents, with extra padding to
 * fill a full record.  After that the header for the next file starts.
 * There's no archive-wide index.  See the code below for how checksum is
 * calculated.
 *
 * Comprehensive references for the tar archive are available in
 * https://www.loc.gov/preservation/digital/formats/fdd/fdd000531.shtml
 *
 * Note: the archive_writer implementation uses only the features and fields
 * needed for storing debug files.  The archive_reader implementation covers
 * only what's provided by the writer.
 */

enum {
   RECORD_SIZE = 512,

   HEADER_NAME_OFFSET = 0,
   HEADER_NAME_LENGTH = 100,

   HEADER_MODE_OFFSET = 100,
   HEADER_MODE_LENGTH = 8,

   HEADER_SIZE_OFFSET = 124,
   HEADER_SIZE_LENGTH = 12,

   HEADER_CHECKSUM_OFFSET = 148,
   HEADER_CHECKSUM_LENGTH = 8,

   HEADER_MAGIC_OFFSET = 257,
   HEADER_MAGIC_LENGTH = 6,

   HEADER_VERSION_OFFSET = 263,
   HEADER_VERSION_LENGTH = 2,

   HEADER_PREFIX_OFFSET = 345,
   HEADER_PREFIX_LENGTH = 155,
};

static void
archive_update_size(char *header, unsigned size)
{
   snprintf(&header[HEADER_SIZE_OFFSET], HEADER_SIZE_LENGTH, "%011o", size);

   /* Checksum of the header assumes the checksum field itself is
    * filled with ASCII spaces (value 32).
    */
   memset(&header[HEADER_CHECKSUM_OFFSET], 32, HEADER_CHECKSUM_LENGTH);
   unsigned checksum = 0;
   for (int i = 0; i < RECORD_SIZE; i++)
      checksum += header[i];
   snprintf(&header[148], HEADER_CHECKSUM_LENGTH, "%07o", checksum);
}

static void
archive_start_header(char *header, const char *prefix, const char *filename)
{
   /* NOTE: If we ever need more, implement the more complex `path` extension. */
   assert(!prefix || strlen(prefix) < HEADER_PREFIX_LENGTH);
   assert(strlen(filename) < HEADER_NAME_LENGTH);

   strncpy(&header[HEADER_NAME_OFFSET], filename, HEADER_NAME_LENGTH);

   if (prefix)
      strncpy(&header[HEADER_PREFIX_OFFSET], prefix, HEADER_PREFIX_LENGTH);

   const char *filemode = "0644";
   strcpy(&header[HEADER_MODE_OFFSET], filemode);

   const char *ustar_magic = "ustar";
   strcpy(&header[HEADER_MAGIC_OFFSET], ustar_magic);

   const char *ustar_version = "00";
   strcpy(&header[HEADER_VERSION_OFFSET], ustar_version);
}

static archive_pos
archive_start_file(FILE *archive, const char *prefix, const char *filename)
{
   char header[RECORD_SIZE] = {0};

   archive_start_header(header, prefix, filename);
   archive_update_size(header, 0);

   archive_pos header_pos = ftell(archive);
   if (header_pos == -1)
      return -1;

   if (fwrite(header, RECORD_SIZE, 1, archive) != 1)
      return -1;

   if (fflush(archive) != 0)
      return -1;

   return header_pos;
}

static bool
archive_write_padding(FILE *archive, unsigned contents_size)
{
   static const char padding[RECORD_SIZE] = {0};
   const unsigned padding_size = RECORD_SIZE - (contents_size % RECORD_SIZE);
   return fwrite(padding, padding_size, 1, archive) == 1;
}

static void
archive_prewrite_end_of_archive(FILE *archive)
{
   /* Two empty records mark the proper end of the file, so always
    * keep them but reposition the cursor so next write overwrites
    * them.
    */
   archive_write_padding(archive, 0);
   archive_write_padding(archive, 0);
   fseek(archive, -RECORD_SIZE * 2, SEEK_END);
}

static bool
archive_finish_file(FILE *archive, archive_pos header_pos)
{
   assert(header_pos >= 0);

   const long end_pos = ftell(archive);
   if (end_pos == -1)
      return false;

   assert((header_pos + RECORD_SIZE) <= end_pos);
   const unsigned size = end_pos - header_pos - RECORD_SIZE;

   archive_write_padding(archive, size);

   /* Read back the header to update file size and checksum. */
   char header[RECORD_SIZE];
   if (fseek(archive, header_pos, SEEK_SET) == -1)
      return false;
   if (fread(header, RECORD_SIZE, 1, archive) != 1)
      return false;

   archive_update_size(header, size);

   if (fseek(archive, header_pos, SEEK_SET) == -1)
      return false;
   if (fwrite(header, RECORD_SIZE, 1, archive) != 1)
      return false;

   if (fseek(archive, 0, SEEK_END) != 0)
      return false;

   archive_prewrite_end_of_archive(archive);

   return fflush(archive) == 0;
}

static bool
archive_file_from_bytes(FILE *archive, const char *prefix, const char *filename,
                        const uint8_t *contents, unsigned contents_size)
{
   char header[RECORD_SIZE] = {0};

   archive_start_header(header, prefix, filename);
   archive_update_size(header, contents_size);

   if (fwrite(header, RECORD_SIZE, 1, archive) != 1)
      return false;

   if (fwrite(contents, contents_size, 1, archive) != 1)
      return false;

   archive_write_padding(archive, contents_size);
   archive_prewrite_end_of_archive(archive);

   fflush(archive);

   return ferror(archive) == 0;
}

void
archive_writer_init(archive_writer *aw, FILE *f)
{
   memset(aw, 0, sizeof(*aw));
   aw->file = f;
   aw->header_pos = -1;
   aw->error = false;
   aw->prefix = NULL;
}

void
archive_writer_start_file(archive_writer *aw, const char *filename)
{
   assert(aw->header_pos == -1);
   aw->header_pos = archive_start_file(aw->file, aw->prefix, filename);
   if (aw->header_pos == -1)
      aw->error = true;
}

void
archive_writer_finish_file(archive_writer *aw)
{
   if (!archive_finish_file(aw->file, aw->header_pos))
      aw->error = true;
   aw->header_pos = -1;
}

void
archive_writer_file_from_bytes(archive_writer *aw, const char *filename,
                               const uint8_t *contents, unsigned contents_size)
{
   assert(aw->header_pos == -1);
   if (!archive_file_from_bytes(aw->file, aw->prefix, filename, contents, contents_size))
      aw->error = true;
}

void
archive_reader_init_from_bytes(archive_reader *ar, uint8_t *contents, unsigned contents_size)
{
   memset(ar, 0, sizeof(*ar));
   ar->contents = contents;
   ar->contents_size = contents_size;
}

bool
archive_reader_next(archive_reader *ar, archive_reader_entry *entry)
{
   if (ar->error)
      return false;

   if (ar->pos >= ar->contents_size)
      return false;

   if (ar->pos + RECORD_SIZE > ar->contents_size) {
      ar->error = true;
      return false;
   }

   const uint8_t *header = &ar->contents[ar->pos];
   const uint8_t *name   = &header[HEADER_NAME_OFFSET];
   const uint8_t *prefix = &header[HEADER_PREFIX_OFFSET];

   ar->pos += RECORD_SIZE;

   /* The current writer enforces the NUL termination and padding, so for
    * now let's rely on it.
    */
   if (name[HEADER_NAME_LENGTH-1] != 0 || prefix[HEADER_PREFIX_LENGTH-1] != 0) {
      ar->error = true;
      return false;
   }

   unsigned size = 0;
   if (sscanf((const char *)&header[HEADER_SIZE_OFFSET], "%011o", &size) == EOF) {
      ar->error = true;
      return false;
   }

   const unsigned padded_size = size + (RECORD_SIZE - (size % RECORD_SIZE));
   if (ar->pos + padded_size > ar->contents_size) {
      ar->error = true;
      return false;
   }

   const uint8_t *contents = &ar->contents[ar->pos];
   ar->pos += padded_size;

   /* TODO: Verify checksum. */

   entry->prefix_start   = prefix;
   entry->prefix_end     = prefix + strlen((const char *)prefix);
   entry->name_start     = name;
   entry->name_end       = name + strlen((const char *)name);
   entry->contents_start = contents;
   entry->contents_end   = contents + size;

   return true;
}
