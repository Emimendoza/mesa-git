/*
 * Copyright 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#ifndef UTIL_ARCHIVE_H
#define UTIL_ARCHIVE_H

/* Subset of the tar archive format.  The writer produces a fully valid tar file,
 * and the reader is capable to read files procuded by that writer.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef long archive_pos;

typedef struct archive_writer {
   FILE *file;
   archive_pos header_pos;
   bool error;
   const char *prefix;
} archive_writer;

void archive_writer_init(archive_writer *aw, FILE *f);
void archive_writer_start_file(archive_writer *aw, const char *filename);
void archive_writer_finish_file(archive_writer *aw);
void archive_writer_file_from_bytes(archive_writer *aw, const char *filename,
                                    const uint8_t *contents, unsigned contents_size);

typedef struct archive_reader {
   const uint8_t *contents;
   unsigned contents_size;

   bool error;

   archive_pos pos;
} archive_reader;

void archive_reader_init_from_bytes(archive_reader *ar, uint8_t *contents, unsigned contents_size);

typedef struct archive_reader_entry {
   const uint8_t *prefix_start;
   const uint8_t *prefix_end;

   const uint8_t *name_start;
   const uint8_t *name_end;

   const uint8_t *contents_start;
   const uint8_t *contents_end;

   bool error;
} archive_reader_entry;

bool archive_reader_next(archive_reader *ar, archive_reader_entry *entry);

#ifdef __cplusplus
}
#endif

#endif /* UTIL_ARCHIVE_H */
