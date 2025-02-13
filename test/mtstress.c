//
//  Copyright (C) 2022  Nick Gasson
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
#include "hash.h"
#include "ident.h"
#include "opt.h"
#include "rt/mspace.h"
#include "thread.h"

#include <check.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

////////////////////////////////////////////////////////////////////////////////
// Concurrent calls to ident_new

#define NWORDS 10000
static char *words[NWORDS];
static ident_t idents[NWORDS];
static volatile int start = 0;

static void *test_ident_thread(void *arg)
{
   const int nproc = nvc_nprocs();

   while (load_acquire(&start) == 0)
      spin_wait();

   for (int i = 0; i < NWORDS / 2 / nproc; i++) {
      const int pos = rand() % NWORDS;
      ident_t id = ident_new(words[pos]), exist;

   again:
      exist = load_acquire(&(idents[pos]));
      if (exist != NULL)
         ck_assert_ptr_eq(exist, id);
      else if (!atomic_cas(&(idents[pos]), NULL, id))
         goto again;
   }

   return NULL;
}

START_TEST(test_ident_new)
{
   FILE *f = fopen("/usr/share/dict/words", "r");
   ck_assert_ptr_nonnull(f);

   char *line LOCAL = NULL;
   size_t bufsz = 0;
   for (int pos = 0; pos < NWORDS; ) {
      const int nchars = getline(&line, &bufsz, f);
      ck_assert_int_ge(nchars, 2);

      if (islower(line[0]) && nchars > 2 && line[nchars - 3] != '\'') {
         line[nchars - 1] = '\0';
         words[pos++] = line;
         line = NULL;
         bufsz = 0;
      }
   }

   fclose(f);

   const int nproc = nvc_nprocs();
   nvc_thread_t *handles[nproc];
   for (int i = 0; i < nproc; i++)
      handles[i] = thread_create(test_ident_thread, NULL, "test thread %d", i);

   store_release(&start, 1);

   for (int i = 0; i < nproc; i++)
      thread_join(handles[i]);
}
END_TEST

////////////////////////////////////////////////////////////////////////////////
// Concurrent hash table updates

#define CHASH_NVALUES 1024
#define CHASH_ITERS   10000

static void *chash_keys[CHASH_NVALUES];
static void *chash_values[CHASH_NVALUES];

#define VOIDP(x) ((void *)(uintptr_t)x)

static void *test_chash_thread(void *arg)
{
   chash_t *h = arg;

   while (load_acquire(&start) == 0)
      spin_wait();

   for (int i = 0; i < CHASH_ITERS; i++) {
      int nth = rand() % CHASH_NVALUES;

      if (rand() % 3 == 0)
         chash_put(h, chash_keys[nth], chash_values[nth]);
      else {
         void *value = chash_get(h, chash_keys[nth]);
         if (value != NULL)
            ck_assert_ptr_eq(value, chash_values[nth]);
      }
   }

   return NULL;
}

START_TEST(test_chash_rand)
{
   for (int i = 0; i < CHASH_NVALUES; i++) {
      do {
         chash_keys[i] = VOIDP(((i << 16) | (rand() & 0xffff)));
      } while (chash_keys[i] == NULL);
      chash_values[i] = VOIDP(rand());
   }

   chash_t *h = chash_new(CHASH_NVALUES / 4);

   const int nproc = nvc_nprocs();
   nvc_thread_t *handles[nproc];
   for (int i = 0; i < nproc; i++)
      handles[i] = thread_create(test_chash_thread, h, "test thread %d", i);

   store_release(&start, 1);

   for (int i = 0; i < nproc; i++)
      thread_join(handles[i]);

   chash_free(h);
}
END_TEST

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
   srand((unsigned)time(NULL));

   term_init();
   thread_init();
   register_signal_handlers();
   set_default_options();
   mspace_stack_limit(MSPACE_CURRENT_FRAME);

   setenv("NVC_LIBPATH", "./lib", 1);

   Suite *s = suite_create("mtstress");

   TCase *tc_ident = tcase_create("ident");
   tcase_add_test(tc_ident, test_ident_new);
   suite_add_tcase(s, tc_ident);

   TCase *tc_chash = tcase_create("chash");
   tcase_add_test(tc_chash, test_chash_rand);
   suite_add_tcase(s, tc_chash);

   SRunner *sr = srunner_create(s);
   srunner_run_all(sr, CK_NORMAL);

   return srunner_ntests_failed(sr) == 0;
}
