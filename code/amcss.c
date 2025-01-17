/* amcss.c: POOL CLASS AMC STRESS TEST
 *
 * $Id$
 * Copyright (c) 2001-2020 Ravenbrook Limited.  See end of file for license.
 * Portions copyright (C) 2002 Global Graphics Software.
 */

#include "fmtdy.h"
#include "fmtdytst.h"
#include "testlib.h"
#include "mpm.h"
#include "mpslib.h"
#include "mpscamc.h"
#include "mpsavm.h"
#include "mpstd.h"
#include "mps.h"
#include "mpslib.h"

#include <stdio.h> /* fflush, printf, putchar */


/* These values have been tuned in the hope of getting one dynamic collection. */
#define testArenaSIZE     ((size_t)1000*1024)
#define gen1SIZE          ((size_t)20)
#define gen2SIZE          ((size_t)85)
#define avLEN             3
#define exactRootsCOUNT   180
#define ambigRootsCOUNT   50
#define genCOUNT          2
#define collectionsCOUNT  37
#define rampSIZE          9
#define initTestFREQ      6000

/* testChain -- generation parameters for the test */

static mps_gen_param_s testChain[genCOUNT] = {
  { gen1SIZE, 0.85 }, { gen2SIZE, 0.45 } };


/* objNULL needs to be odd so that it's ignored in exactRoots. */
#define objNULL           ((mps_addr_t)MPS_WORD_CONST(0xDECEA5ED))


static mps_arena_t arena;
static mps_ap_t ap;
static mps_addr_t exactRoots[exactRootsCOUNT];
static mps_addr_t ambigRoots[ambigRootsCOUNT];
static size_t scale;            /* Overall scale factor. */
static unsigned long nCollsStart;
static unsigned long nCollsDone;


/* report -- report statistics from any messages */

static void report(void)
{
  mps_message_type_t type;

  while(mps_message_queue_type(&type, arena)) {
    mps_message_t message;

    cdie(mps_message_get(&message, arena, type), "message get");

    if (type == mps_message_type_gc_start()) {
      nCollsStart += 1;
      printf("\n{\n  Collection %lu started.  Because:\n", nCollsStart);
      printf("    %s\n", mps_message_gc_start_why(arena, message));
      printf("    clock: %"PRIuLONGEST"\n", (ulongest_t)mps_message_clock(arena, message));

    } else if (type == mps_message_type_gc()) {
      size_t live, condemned, not_condemned;

      nCollsDone += 1;
      live = mps_message_gc_live_size(arena, message);
      condemned = mps_message_gc_condemned_size(arena, message);
      not_condemned = mps_message_gc_not_condemned_size(arena, message);

      printf("\n  Collection %lu finished:\n", nCollsDone);
      printf("    live %"PRIuLONGEST"\n", (ulongest_t)live);
      printf("    condemned %"PRIuLONGEST"\n", (ulongest_t)condemned);
      printf("    not_condemned %"PRIuLONGEST"\n", (ulongest_t)not_condemned);
      printf("    clock: %"PRIuLONGEST"\n", (ulongest_t)mps_message_clock(arena, message));
      printf("}\n");
    } else {
      cdie(0, "unknown message type");
      break;
    }

    mps_message_discard(arena, message);
  }
}


/* make -- create one new object */

static mps_addr_t make(size_t rootsCount)
{
  static unsigned long calls = 0;
  size_t length = rnd() % (scale * avLEN);
  size_t size = (length+2) * sizeof(mps_word_t);
  mps_addr_t p;
  mps_res_t res;
  ++ calls;

  do {
    MPS_RESERVE_BLOCK(res, p, ap, size);
    if (res) {
      ArenaDescribe(arena, mps_lib_get_stderr(), 4);
      die(res, "MPS_RESERVE_BLOCK");
    }
    res = dylan_init(p, size, exactRoots, rootsCount);
    if (res)
      die(res, "dylan_init");
  } while(!mps_commit(ap, p, size));

  return p;
}


/* test_stepper -- stepping function for walk */

static void test_stepper(mps_addr_t object, mps_fmt_t fmt, mps_pool_t pool,
                         void *p, size_t s)
{
  testlib_unused(object); testlib_unused(fmt); testlib_unused(pool);
  testlib_unused(s);
  (*(unsigned long *)p)++;
}


/* area_scan -- area scanning function for mps_pool_walk */

static mps_res_t area_scan(mps_ss_t ss, void *base, void *limit, void *closure)
{
  unsigned long *count = closure;
  mps_res_t res;
  while (base < limit) {
    mps_addr_t prev = base;
    ++ *count;
    res = dylan_scan1(ss, &base);
    if (res != MPS_RES_OK) return res;
    Insist(prev < base);
  }
  Insist(base == limit);
  return MPS_RES_OK;
}


/* test -- the body of the test */

static void test(mps_pool_class_t pool_class, size_t roots_count)
{
  mps_fmt_t format;
  mps_chain_t chain;
  mps_root_t exactRoot, ambigRoot;
  unsigned long objs; size_t i;
  mps_word_t collections, rampSwitch;
  mps_alloc_pattern_t ramp = mps_alloc_pattern_ramp();
  int ramping;
  mps_ap_t busy_ap;
  mps_addr_t busy_init;
  mps_pool_t pool;
  int described = 0;

  die(dylan_fmt(&format, arena), "fmt_create");
  die(mps_chain_create(&chain, arena, genCOUNT, testChain), "chain_create");

  die(mps_pool_create(&pool, arena, pool_class, format, chain),
      "pool_create(amc)");

  die(mps_ap_create(&ap, pool, mps_rank_exact()), "BufferCreate");
  die(mps_ap_create(&busy_ap, pool, mps_rank_exact()), "BufferCreate 2");

  for(i = 0; i < exactRootsCOUNT; ++i)
    exactRoots[i] = objNULL;
  for(i = 0; i < ambigRootsCOUNT; ++i)
    ambigRoots[i] = rnd_addr();

  die(mps_root_create_table_masked(&exactRoot, arena,
                                   mps_rank_exact(), (mps_rm_t)0,
                                   &exactRoots[0], exactRootsCOUNT,
                                   (mps_word_t)1),
      "root_create_table(exact)");
  die(mps_root_create_table(&ambigRoot, arena,
                            mps_rank_ambig(), (mps_rm_t)0,
                            &ambigRoots[0], ambigRootsCOUNT),
      "root_create_table(ambig)");

  /* create an ap, and leave it busy */
  die(mps_reserve(&busy_init, busy_ap, 64), "mps_reserve busy");

  nCollsStart = 0;
  nCollsDone = 0;
  collections = 0;
  rampSwitch = rampSIZE;
  die(mps_ap_alloc_pattern_begin(ap, ramp), "pattern begin (ap)");
  die(mps_ap_alloc_pattern_begin(busy_ap, ramp), "pattern begin (busy_ap)");
  ramping = 1;
  objs = 0;
  while (collections < collectionsCOUNT) {
    size_t r;

    report();
    if (collections != nCollsStart) {
      if (!described) {
        die(ArenaDescribe(arena, mps_lib_get_stdout(), 0), "ArenaDescribe");
        described = TRUE;
      }
      collections = nCollsStart;

      printf("%lu objects (nCollsStart=%"PRIuLONGEST")\n", objs,
             (ulongest_t)collections);

      /* test mps_arena_has_addr */
      {
        size_t hitRatio;
        unsigned hitsWanted = 4;  /* aim for 4 hits (on average) */
        /* [Note: The for-loop condition used to be "i < 4 * hitRatio",
         *  with "4" an unexplained naked constant.  I have now labelled
         *  it "hitsWanted", as I think that is the intent.  RHSK]
         */

        /* how many random addrs must we try, to hit the arena once? */
        hitRatio = (0xfffffffful / mps_arena_committed(arena));
        for (i = 0; i < hitsWanted * hitRatio ; i++) {
          /* An exact root maybe in the arena, so add a random 32-bit
           * offset to it.  We may get no hits if it is objNULL.
           */
          mps_addr_t p = (char *)exactRoots[rnd() % exactRootsCOUNT]
                         + rnd()-0x80000000ul;
          if (mps_arena_has_addr(arena, p)) {
            printf("%p is in the arena\n", p);
          }
        }
      }

      for (i = 0; i < exactRootsCOUNT; ++i)
        cdie(exactRoots[i] == objNULL
             || (dylan_check(exactRoots[i])
                 && mps_arena_has_addr(arena, exactRoots[i])),
             "all roots check");
      cdie(!mps_arena_has_addr(arena, NULL),
           "NULL in arena");

      if (collections == collectionsCOUNT / 2) {
        unsigned long count1 = 0, count2 = 0;
        mps_arena_park(arena);
        mps_arena_formatted_objects_walk(arena, test_stepper, &count1, 0);
        die(mps_pool_walk(pool, area_scan, &count2), "mps_pool_walk");
        mps_arena_release(arena);
        printf("stepped on %lu objects.\n", count1);
        printf("walked %lu objects.\n", count2);
        Insist(count1 == count2);
      }
      if (collections == rampSwitch) {
        int begin_ramp = !ramping
          || /* Every other time, switch back immediately. */ (collections & 1);

        rampSwitch += rampSIZE;
        if (ramping) {
          die(mps_ap_alloc_pattern_end(ap, ramp), "pattern end (ap)");
          die(mps_ap_alloc_pattern_end(busy_ap, ramp), "pattern end (busy_ap)");
          ramping = 0;
          /* kill half of the roots */
          for(i = 0; i < exactRootsCOUNT; i += 2) {
            if (exactRoots[i] != objNULL) {
              cdie(dylan_check(exactRoots[i]), "ramp kill check");
              exactRoots[i] = objNULL;
            }
          }
        }
        if (begin_ramp) {
          die(mps_ap_alloc_pattern_begin(ap, ramp),
              "pattern rebegin (ap)");
          die(mps_ap_alloc_pattern_begin(busy_ap, ramp),
              "pattern rebegin (busy_ap)");
          ramping = 1;
        }
      }
    }

    r = (size_t)rnd();
    if (r & 1) {
      i = (r >> 1) % exactRootsCOUNT;
      if (exactRoots[i] != objNULL)
        cdie(dylan_check(exactRoots[i]), "dying root check");
      exactRoots[i] = make(roots_count);
      if (exactRoots[(exactRootsCOUNT-1) - i] != objNULL)
        dylan_write(exactRoots[(exactRootsCOUNT-1) - i],
                    exactRoots, exactRootsCOUNT);
    } else {
      i = (r >> 1) % ambigRootsCOUNT;
      ambigRoots[(ambigRootsCOUNT-1) - i] = make(roots_count);
      /* Create random interior pointers */
      ambigRoots[i] = (mps_addr_t)((char *)(ambigRoots[i/2]) + 1);
    }

    if (r % initTestFREQ == 0)
      *(int*)busy_init = -1; /* check that the buffer is still there */

    if (objs % 1024 == 0) {
      report();
      putchar('.');
      (void)fflush(stdout);
    }

    ++objs;
  }

  (void)mps_commit(busy_ap, busy_init, 64);
  mps_arena_park(arena);
  mps_ap_destroy(busy_ap);
  mps_ap_destroy(ap);
  mps_root_destroy(exactRoot);
  mps_root_destroy(ambigRoot);
  mps_pool_destroy(pool);
  mps_chain_destroy(chain);
  mps_fmt_destroy(format);
  mps_arena_release(arena);
}

int main(int argc, char *argv[])
{
  size_t i, grainSize;
  mps_thr_t thread;

  testlib_init(argc, argv);

  scale = (size_t)1 << (rnd() % 6);
  for (i = 0; i < genCOUNT; ++i) testChain[i].mps_capacity *= scale;
  grainSize = rnd_grain(scale * testArenaSIZE);
  printf("Picked scale=%lu grainSize=%lu\n", (unsigned long)scale, (unsigned long)grainSize);

  MPS_ARGS_BEGIN(args) {
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_SIZE, scale * testArenaSIZE);
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_GRAIN_SIZE, grainSize);
    die(mps_arena_create_k(&arena, mps_arena_class_vm(), args), "arena_create");
  } MPS_ARGS_END(args);
  mps_message_type_enable(arena, mps_message_type_gc());
  mps_message_type_enable(arena, mps_message_type_gc_start());
  die(mps_thread_reg(&thread, arena), "thread_reg");
  test(mps_class_amc(), exactRootsCOUNT);
  test(mps_class_amcz(), 0);
  mps_thread_dereg(thread);
  report();
  mps_arena_destroy(arena);

  printf("%s: Conclusion: Failed to find any defects.\n", argv[0]);
  return 0;
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2001-2020 Ravenbrook Limited <https://www.ravenbrook.com/>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
