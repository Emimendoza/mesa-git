/*
 * Copyright © 2024 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <gtest/gtest.h>
#include "util/archive.h"

TEST(archive, writer_reader_small_file)
{
   FILE *f = tmpfile();
   const char *test = "TEST TEST TEST";

   {
      archive_writer aw;
      archive_writer_init(&aw, f);

      archive_writer_start_file(&aw, "test");

      fwrite(test, strlen(test), 1, f);

      archive_writer_finish_file(&aw);
   }

   long size = ftell(f);
   ASSERT_TRUE(size > 0);
   ASSERT_TRUE(size % 512 == 0);
   uint8_t *contents = new uint8_t[size];

   fseek(f, 0, SEEK_SET);
   fread(contents, size, 1, f);
   fclose(f);

   {
      archive_reader ar;
      archive_reader_init_from_bytes(&ar, contents, size);

      archive_reader_entry entry;

      bool first_read = archive_reader_next(&ar, &entry);
      ASSERT_TRUE(first_read);
      ASSERT_FALSE(entry.error);

      ASSERT_EQ(entry.name_end - entry.name_start, 4);
      ASSERT_TRUE(memcmp(entry.name_start, "test", 4) == 0);

      ASSERT_EQ(entry.contents_end - entry.contents_start, strlen(test));
      ASSERT_TRUE(memcmp(entry.contents_start, test, strlen(test)) == 0);

      bool second_read = archive_reader_next(&ar, &entry);
      ASSERT_FALSE(second_read);
      ASSERT_FALSE(entry.error);
   }

   delete[] contents;
}
