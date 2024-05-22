/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_file.h"
#include "util/archive.h"
#include "util/ralloc.h"

typedef struct step {
   const char    *name;
   const uint8_t *start;
   const uint8_t *end;
} step;

typedef struct shader {
   const char *name;
   const char *repr_name;

   const char *match_name;

   int         steps_count;
   step       *steps;
} shader;

typedef struct mesa_archive {
   uint8_t *contents;
   unsigned contents_size;

   int     shaders_count;
   shader *shaders;

   const char *info;
} mesa_archive;

const char DEFAULT_DIFF_COMMAND[] = "git diff --no-index --color-words %s %s | tail -n +4";

static void
diff(const uint8_t *a_start, const uint8_t *a_end,
     const uint8_t *b_start, const uint8_t *b_end)
{
   FILE *a = tmpfile();
   FILE *b = tmpfile();

   fwrite(a_start, a_end - a_start, 1, a);
   fwrite(b_start, b_end - b_start, 1, b);

   fflush(a);
   fflush(b);

   int fd_a = fileno(a);
   int fd_b = fileno(b);

   static const char *diff_cmd = NULL;
   if (!diff_cmd) {
      diff_cmd = getenv("MDA_DIFF_COMMAND");
      if (!diff_cmd)
         diff_cmd = DEFAULT_DIFF_COMMAND;
   }

   /* The git-diff, even in non-repository mode, will not follow symlinks, so
    * explicitly cat the contents.
    */
   char path_a[128] = {0};
   char path_b[128] = {0};
   snprintf(path_a, sizeof(path_a)-2, "<(cat /proc/self/fd/%d)", fd_a);
   snprintf(path_b, sizeof(path_b)-2, "<(cat /proc/self/fd/%d)", fd_b);

   char cmd[1024] = {0};
   snprintf(cmd, sizeof(cmd)-2, diff_cmd, path_a, path_b);

   /* Make sure everything printed so far is flushed before the diff
    * subprocess print things.
    */
   fflush(stdout);

   system(cmd);

   fclose(a);
   fclose(b);
}

static char *
ralloc_str_from_range(void *mem_ctx, const uint8_t *start, const uint8_t *end)
{
   assert(start);
   assert(end);
   assert(start <= end);
   return ralloc_strndup(mem_ctx, (const char *)start, end - start);
}

static mesa_archive *
parse_mesa_archive(void *mem_ctx, const char *filename)
{
   size_t size = 0;
   uint8_t *contents = (uint8_t *)os_read_file(filename, &size);
   if (!contents) {
      fprintf(stderr, "mda: error reading file %s: %s\n", filename, strerror(errno));
      return NULL;
   }

   mesa_archive *ma = rzalloc(mem_ctx, mesa_archive);
   ma->contents = ralloc_memdup(ma, (const char *)contents, size);
   ma->contents_size = size;
   free(contents);

   archive_reader ar = {0};
   archive_reader_init_from_bytes(&ar, ma->contents, size);

   shader *cur_shader = NULL;

   archive_reader_entry entry = {0};

   {
      if (!archive_reader_next(&ar, &entry) || entry.error) {
         fprintf(stderr, "mda: wrong archive, missing mesa.txt\n");
         return NULL;
      }

      const char *mesa_txt = ralloc_str_from_range(ma, entry.name_start, entry.name_end);
      if (strcmp(mesa_txt, "mesa.txt")) {
         fprintf(stderr, "mda: wrong archive, missing mesa.txt\n");
         return NULL;
      }

      ma->info = ralloc_str_from_range(ma, entry.contents_start, entry.contents_end);
   }

   while (archive_reader_next(&ar, &entry)) {
      char *name      = ralloc_str_from_range(ma, entry.prefix_start, entry.prefix_end);
      char *repr_name = ralloc_str_from_range(ma, entry.name_start,   entry.name_end);

      char *slash = strchr(repr_name, '/');
      char *step_name;
      if (slash) {
         assert(slash < repr_name + strlen(repr_name) - 1);
         *slash = 0;
         step_name = slash + 1;
      } else {
         step_name = "";
      }

      assert(strlen(name) > 4);
      assert(!strncmp(name, "mda/", 4));
      name = &name[4];

      if (!cur_shader || strcmp(name, cur_shader->name) || strcmp(repr_name, cur_shader->repr_name)) {
         ma->shaders = rerzalloc(ma, ma->shaders, shader, ma->shaders_count, ma->shaders_count + 1);
         cur_shader = &ma->shaders[ma->shaders_count++];
         cur_shader->name = name;
         cur_shader->repr_name = repr_name;
         cur_shader->match_name = ralloc_asprintf(ma, "%s/%s", name, repr_name);
      }

      cur_shader->steps = rerzalloc(ma, cur_shader->steps, step,
                                    cur_shader->steps_count, cur_shader->steps_count + 1);
      int s = cur_shader->steps_count++;

      cur_shader->steps[s].name  = step_name;
      cur_shader->steps[s].start = entry.contents_start;
      cur_shader->steps[s].end   = entry.contents_end;
   }

   return ma;
}

static void
print_repeated(char c, int count)
{
   for (; count > 0; count--)
      putchar(c);
}

static shader *
find_shader(mesa_archive *ma, const char *pattern[], int pattern_count)
{
   shader *match = NULL;

   if (ma->shaders_count == 0) {
      fprintf(stderr, "mda: no shaders in this archive\n");
      return NULL;
   }

   if (!pattern) {
      if (ma->shaders_count == 1) {
         return ma->shaders;
      } else {
         fprintf(stderr, "mda: multiple shaders in this archive file, "
                         "pass patterns to disambiguate\n");
         return NULL;
      }
   }

   for (shader *sh = ma->shaders; sh < ma->shaders + ma->shaders_count; sh++) {
      bool matched = true;
      for (const char **pat = pattern; pat < pattern + pattern_count; pat++) {
         matched &= strcasestr(sh->match_name, *pat) != NULL;
      }

      if (matched) {
         if (match) {
            fprintf(stderr, "error: multiple matches for pattern:");
            for (const char **pat = pattern; pat < pattern + pattern_count; pat++) {
               fprintf(stderr, " %s", *pat);
            }
            fprintf(stderr, "\n    %s\n    %s\npick a different pattern.\n",
                    match->match_name, sh->match_name);
            return NULL;
         } else {
            match = sh;
         }
      }
   }

   if (!match) {
      fprintf(stderr, "mda: couldn't find shader for pattern:");
      for (const char **pat = pattern; pat < pattern + pattern_count; pat++) {
         fprintf(stderr, " %s", *pat);
      }
      fprintf(stderr, "\n");
      return NULL;
   }

   return match;
}

static int
cmd_list(void *mem_ctx, int argc, char *argv[])
{
   assert(argc > 2);

   const char *filename = argv[2];
   mesa_archive *ma = parse_mesa_archive(mem_ctx, filename);
   if (!ma)
      return 1;

   printf("%s\n", ma->info);

   const char *cur_name = "";

   for (shader *sh = ma->shaders; sh < ma->shaders + ma->shaders_count; sh++) {
      if (strcmp(cur_name, sh->name)) {
         printf("\n%s\n", sh->name);
         cur_name = sh->name;
      }
      printf("    %s", sh->repr_name);
      if (sh->steps_count > 1)
         printf(" (%d steps)", sh->steps_count);
      printf("\n");
   }

   return 0;
}

static int
cmd_liststep(void *mem_ctx, int argc, char *argv[])
{
   assert(argc > 2);

   const char *filename = argv[2];
   mesa_archive *ma = parse_mesa_archive(mem_ctx, filename);
   if (!ma)
      return 1;

   const char **pattern = argc < 4 ? NULL : (const char **)&argv[3];
   int pattern_count    = argc < 4 ?    0 : argc - 3;

   shader *sh = find_shader(ma, pattern, pattern_count);
   if (!sh)
      return 1;

   printf("### %s\n", sh->match_name);

   for (int i = 0; i < sh->steps_count; i++) {
      const step *step = &sh->steps[i];
      printf("%s (%d)\n", step->name, i);
   }

   printf("\n");

   return 0;
}

static int
cmd_diffstep(void *mem_ctx, int argc, char *argv[])
{
   assert(argc > 2);

   const char *filename = argv[2];
   mesa_archive *ma = parse_mesa_archive(mem_ctx, filename);
   if (!ma)
      return 1;

   if (argc < 5) {
      fprintf(stderr, "mda: need to pass two step numbers to compare\n");
      return 1;
   }

   const int a = atoi(argv[3]);
   const int b = atoi(argv[4]);

   const char **pattern = argc < 6 ? NULL : (const char **)&argv[5];
   int pattern_count    = argc < 6 ?    0 : argc - 5;

   shader *sh = find_shader(ma, pattern, pattern_count);
   if (!sh)
      return 1;

   if (a >= sh->steps_count || b >= sh->steps_count) {
      fprintf(stderr, "mda: invalid step numbers %d and %d\n", a, b);
      return 1;
   }

   const step *a_step = &sh->steps[a];
   const step *b_step = &sh->steps[b];

   printf("# %s (%d) -> %s (%d)\n", a_step->name, a, b_step->name, b);
   printf("# ");
   print_repeated('=', strlen(a_step->name) + 2 + (int)ceilf(log10(a)) + 1 +
                       4 +
                       strlen(b_step->name) + 2 + (int)ceilf(log10(b)) + 1);
   printf("\n");

   diff(a_step->start, a_step->end,
        b_step->start, b_step->end);
   printf("\n");

   return 0;
}

static int
cmd_log(void *mem_ctx, int argc, char *argv[])
{
   assert(argc > 2);

   const char *filename = argv[2];
   mesa_archive *ma = parse_mesa_archive(mem_ctx, filename);
   if (!ma)
      return 1;

   const char **pattern = argc < 4 ? NULL : (const char **)&argv[3];
   int pattern_count    = argc < 4 ?    0 : argc - 3;

   shader *sh = find_shader(ma, pattern, pattern_count);
   if (!sh)
      return 1;

   for (int i = 1; i < sh->steps_count; i++) {
      const step *new_step = &sh->steps[i];
      const step *old_step = &sh->steps[i-1];

      printf("# %s (%d) -> %s (%d)\n", old_step->name, i-1, new_step->name, i);
      printf("# ");
      print_repeated('=', strlen(old_step->name) + 2 + (int)ceilf(log10(i-1)) + 1 +
                          4 +
                          strlen(new_step->name) + 2 + (int)ceilf(log10(i)) + 1);
      printf("\n");

      diff(old_step->start, old_step->end,
           new_step->start, new_step->end);
      printf("\n");
   }

   printf("\n");
   return 0;
}

static int
cmd_show(void *mem_ctx, int argc, char *argv[])
{
   assert(argc > 2);

   const char *filename = argv[2];
   mesa_archive *ma = parse_mesa_archive(mem_ctx, filename);
   if (!ma)
      return 1;

   const char **pattern = argc < 4 ? NULL : (const char **)&argv[3];
   int pattern_count    = argc < 4 ?    0 : argc - 3;

   shader *sh = find_shader(ma, pattern, pattern_count);
   if (!sh || !sh->steps_count)
      return 1;

   printf("### %s\n", sh->match_name);

   const step *step = &sh->steps[sh->steps_count - 1];

   printf("# %s (%d)\n", step->name, sh->steps_count - 1);
   printf("# ");
   print_repeated('=', strlen(step->name) + 2 + (int)ceilf(log10(sh->steps_count)) + 1);
   printf("\n");

   fwrite(step->start, step->end - step->start, 1, stdout);
   printf("\n");

   return 0;
}

static int
cmd_showall(void *mem_ctx, int argc, char *argv[])
{
   assert(argc > 2);

   const char *filename = argv[2];
   mesa_archive *ma = parse_mesa_archive(mem_ctx, filename);
   if (!ma)
      return 1;

   const char **pattern = argc < 4 ? NULL : (const char **)&argv[3];
   int pattern_count    = argc < 4 ?    0 : argc - 3;

   shader *sh = find_shader(ma, pattern, pattern_count);
   if (!sh)
      return 1;

   printf("### %s\n", sh->match_name);

   for (int i = 0; i < sh->steps_count; i++) {
      const step *step = &sh->steps[i];

      printf("# %s (%d)\n", step->name, i);
      printf("# ");
      print_repeated('=', strlen(step->name) + 2 + (int)ceilf(log10(i)) + 1);
      printf("\n");

      fwrite(step->start, step->end - step->start, 1, stdout);
      printf("\n");
   }

   printf("\n");

   return 0;
}

static int
cmd_help()
{
   printf("mda CMD FILENAME [ARGS...]\n"
          "\n"
          "Reads mda.* files generated by Mesa drivers, these\n"
          "files contain debugging information about a pipeline or\n"
          "a single shader stage.\n"
          "\n"
          "Without command, all the 'objects' are listed, an object can\n"
          "be a particular internal shader representation or other metadata.\n"
          "\n"
          "Objects are identified by a substring case insensitive of their\n"
          "name.  Multiple space separated substrings can be used to\n"
          "disambiguate objects -- the selected object must match all.\n"
          "\n"
          "COMMANDS\n"
          "\n"
          "    list                            list all the objects\n"
          "    liststep FILENAME [PATTERN...]  list all the states of an object\n"
          "    show     FILENAME [PATTERN...]  prints the last state of an object\n"
          "    showall  FILENAME [PATTERN...]  prints all the states of an object\n"
          "    log      FILENAME [PATTERN...]  prints a sequence of diffs of an object states\n"
          "    diffstep FILENAME STEP STEP [PATTERN...]  compare two states of an object\n"
          "\n"
          "The diff program used by mda can be configured by setting the MDA_DIFF_COMMAND\n"
          "environment variable.  By default it uses git-diff that works even without a\n"
          "git repository:\n"
          "\n"
          "    MDA_DIFF_COMMAND=\"%s\"\n"
          "\n", DEFAULT_DIFF_COMMAND);
   return 0;
}

static bool
is_help(const char *arg)
{
   return !strcmp(arg, "--help") ||
          !strcmp(arg, "-help") ||
          !strcmp(arg, "-h");
}

struct command {
   const char *name;
   int (*func)(void *mem_ctx, int argc, char *argv[]);
};

int
main(int argc, char *argv[])
{
   const char *cmd_name = argc < 2 ? NULL : argv[1];
   if (!cmd_name || !strcmp(cmd_name, "help") || is_help(cmd_name)) {
      cmd_help();
      return 0;
   }

   static const struct command cmds[] = {
      { "diffstep", cmd_diffstep },
      { "list",     cmd_list },
      { "liststep", cmd_liststep },
      { "log",      cmd_log },
      { "show",     cmd_show },
      { "showall",  cmd_showall },
   };

   const struct command *cmd = NULL;
   for (const struct command *c = cmds; c < cmds + ARRAY_SIZE(cmds); c++) {
      if (!strcmp(c->name, cmd_name)) {
         cmd = c;
         break;
      }
   }

   if (!cmd) {
      fprintf(stderr, "mda: unknown command '%s'\n", cmd_name);
      cmd_help();
      return 1;
   }

   void *mem_ctx = ralloc_context(NULL);
   int r = cmd->func(mem_ctx, argc, argv);
   ralloc_free(mem_ctx);

   return r;
}
