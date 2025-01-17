/* walk.c: OBJECT WALKER
 *
 * $Id$
 * Copyright (c) 2001-2020 Ravenbrook Limited.  See end of file for license.
 */

#include "mpm.h"
#include "mps.h"

SRCID(walk, "$Id$");


/* Heap Walking
 */


#define FormattedObjectsStepClosureSig ((Sig)0x519F05C1) /* SIGnature Formatted Objects Step CLosure */

typedef struct FormattedObjectsStepClosureStruct *FormattedObjectsStepClosure;

typedef struct FormattedObjectsStepClosureStruct {
  Sig sig;
  mps_formatted_objects_stepper_t f;
  void *p;
  size_t s;
} FormattedObjectsStepClosureStruct;


ATTRIBUTE_UNUSED
static Bool FormattedObjectsStepClosureCheck(FormattedObjectsStepClosure c)
{
  CHECKS(FormattedObjectsStepClosure, c);
  CHECKL(FUNCHECK(c->f));
  /* p and s fields are arbitrary closures which cannot be checked */
  return TRUE;
}


static void ArenaFormattedObjectsStep(Addr object, Format format, Pool pool,
                                      void *p, size_t s)
{
  FormattedObjectsStepClosure c;
  /* Can't check object */
  AVERT(Format, format);
  AVERT(Pool, pool);
  c = p;
  AVERT(FormattedObjectsStepClosure, c);
  AVER(s == UNUSED_SIZE);

  (*c->f)((mps_addr_t)object, (mps_fmt_t)format, (mps_pool_t)pool,
          c->p, c->s);
}


/* ArenaFormattedObjectsWalk -- iterate over all objects
 *
 * So called because it walks all formatted objects in an arena.  */

static void ArenaFormattedObjectsWalk(Arena arena, FormattedObjectsVisitor f,
                                      void *p, size_t s)
{
  Seg seg;
  FormattedObjectsStepClosure c;
  Format format;

  AVERT(Arena, arena);
  AVER(FUNCHECK(f));
  AVER(f == ArenaFormattedObjectsStep);
  /* Know that p is a FormattedObjectsStepClosure  */
  c = p;
  AVERT(FormattedObjectsStepClosure, c);
  /* Know that s is UNUSED_SIZE */
  AVER(s == UNUSED_SIZE);

  if (SegFirst(&seg, arena)) {
    do {
      if (PoolFormat(&format, SegPool(seg))) {
        ShieldExpose(arena, seg);
        SegWalk(seg, format, f, p, s);
        ShieldCover(arena, seg);
      }
    } while(SegNext(&seg, arena, seg));
  }
}


/* mps_arena_formatted_objects_walk -- iterate over all objects
 *
 * Client interface to ArenaFormattedObjectsWalk.  */

void mps_arena_formatted_objects_walk(mps_arena_t mps_arena,
                                      mps_formatted_objects_stepper_t f,
                                      void *p, size_t s)
{
  Arena arena = (Arena)mps_arena;
  FormattedObjectsStepClosureStruct c;

  ArenaEnter(arena);
  AVERT(Arena, arena);
  AVER(FUNCHECK(f));
  /* p and s are arbitrary closures, hence can't be checked */
  c.sig = FormattedObjectsStepClosureSig;
  c.f = f;
  c.p = p;
  c.s = s;
  ArenaFormattedObjectsWalk(arena, ArenaFormattedObjectsStep, &c, UNUSED_SIZE);
  ArenaLeave(arena);
}



/* Root Walking
 *
 * This involves more code than it should. The roots are walked by
 * scanning them. But there's no direct support for invoking the scanner
 * without there being a trace, and there's no direct support for
 * creating a trace without also condemning part of the heap. (@@@@ This
 * looks like a useful candidate for inclusion in the future). For now,
 * the root walker contains its own code for creating a minimal trace
 * and scan state.
 *
 * ASSUMPTIONS
 *
 * .assume.parked: The root walker must be invoked with a parked
 * arena. It's only strictly necessary for there to be no current trace,
 * but the client has no way to ensure this apart from parking the
 * arena.
 *
 * .assume.rootaddr: The client closure is called with a parameter which
 * is the address of a reference to an object referenced from a
 * root. The client may desire this address to be the address of the
 * actual reference in the root (so that the debugger can be used to
 * determine details about the root). This is not always possible, since
 * the root might actually be a register, or the format scan method
 * might not pass this address directly to the fix method. If the format
 * code does pass on the address, the client can be sure to be passed
 * the address of any root other than a register or stack.  */


/* rootsStepClosure -- closure environment for root walker
 *
 * Defined as a subclass of ScanState.  */

#define rootsStepClosureSig ((Sig)0x51965C10) /* SIGnature Roots Step CLOsure */

typedef struct rootsStepClosureStruct *rootsStepClosure;
typedef struct rootsStepClosureStruct {
  ScanStateStruct ssStruct;          /* generic scan state object */
  mps_roots_stepper_t f;             /* client closure function */
  void *p;                           /* client closure data */
  size_t s;                          /* client closure data */
  Root root;                         /* current root, or NULL */
  Sig sig;                           /* <code/misc.h#sig> */
} rootsStepClosureStruct;

#define rootsStepClosure2ScanState(rsc) (&(rsc)->ssStruct)
#define ScanState2rootsStepClosure(ss) \
  PARENT(rootsStepClosureStruct, ssStruct, ss)


/* rootsStepClosureCheck -- check a rootsStepClosure */

ATTRIBUTE_UNUSED
static Bool rootsStepClosureCheck(rootsStepClosure rsc)
{
  CHECKS(rootsStepClosure, rsc);
  CHECKD(ScanState, &rsc->ssStruct);
  CHECKL(FUNCHECK(rsc->f));
  /* p and s fields are arbitrary closures which cannot be checked */
  if (rsc->root != NULL) {
    CHECKD_NOSIG(Root, rsc->root); /* <design/check/#.hidden-type> */
  }
  return TRUE;
}


/* rootsStepClosureInit -- Initialize a rootsStepClosure
 *
 * Initialize the parent ScanState too.  */

static void rootsStepClosureInit(rootsStepClosure rsc,
                                 Globals arena, Trace trace,
                                 SegFixMethod rootFix,
                                 mps_roots_stepper_t f, void *p, size_t s)
{
  ScanState ss;

  /* First initialize the ScanState superclass */
  ss = &rsc->ssStruct;
  ScanStateInit(ss, TraceSetSingle(trace), GlobalsArena(arena), RankMIN,
                trace->white);

  /* Initialize the fix method in the ScanState */
  ss->fix = rootFix;

  /* Initialize subclass specific data */
  rsc->f = f;
  rsc->p = p;
  rsc->s = s;
  rsc->root = NULL;

  rsc->sig = rootsStepClosureSig;

  AVERT(rootsStepClosure, rsc);
}


/* rootsStepClosureFinish -- Finish a rootsStepClosure
 *
 * Finish the parent ScanState too.  */

static void rootsStepClosureFinish(rootsStepClosure rsc)
{
  ScanState ss;

  ss = rootsStepClosure2ScanState(rsc);
  rsc->sig = SigInvalid;
  ScanStateFinish(ss);
}


/* RootsWalkFix -- the fix method used during root walking
 *
 * This doesn't cause further scanning of transitive references, it just
 * calls the client closure.  */

static Res RootsWalkFix(Seg seg, ScanState ss, Ref *refIO)
{
  rootsStepClosure rsc;
  Ref ref;

  AVERT(Seg, seg);
  AVERT(ScanState, ss);
  AVER(refIO != NULL);
  rsc = ScanState2rootsStepClosure(ss);
  AVERT(rootsStepClosure, rsc);

  ref = *refIO;

  /* Call the client closure - .assume.rootaddr */
  rsc->f((mps_addr_t*)refIO, (mps_root_t)rsc->root, rsc->p, rsc->s);

  AVER(ref == *refIO);  /* can walk object graph - but not modify it */

  return ResOK;
}


/* rootWalk -- the step function for ArenaRootsWalk */

static Res rootWalk(Root root, void *p)
{
  ScanState ss = (ScanState)p;

  AVERT(ScanState, ss);

  if (RootRank(root) == ss->rank) {
    /* set the root for the benefit of the fix method */
    ScanState2rootsStepClosure(ss)->root = root;
    /* Scan it */
    ScanStateSetSummary(ss, RefSetEMPTY);
    return RootScan(ss, root);
  } else
    return ResOK;
}


/* rootWalkGrey -- make the root grey for the trace passed as p */

static Res rootWalkGrey(Root root, void *p)
{
  Trace trace = p;

  AVERT(Root, root);
  AVERT(Trace, trace);

  RootGrey(root, trace);
  return ResOK;
}


/* ArenaRootsWalk -- walks all the root in the arena */

static Res ArenaRootsWalk(Globals arenaGlobals, mps_roots_stepper_t f,
                          void *p, size_t s)
{
  Arena arena;
  rootsStepClosureStruct rscStruct;
  rootsStepClosure rsc = &rscStruct;
  Trace trace;
  ScanState ss;
  Rank rank;
  Res res;
  Seg seg;

  AVERT(Globals, arenaGlobals);
  AVER(FUNCHECK(f));
  /* p and s are arbitrary client-provided closure data. */
  arena = GlobalsArena(arenaGlobals);

  /* Scan all the roots with a minimal trace.  Invoke the scanner with a */
  /* rootsStepClosure, which is a subclass of ScanState and contains the */
  /* client-provided closure.  Supply a special fix method in order to */
  /* call the client closure.  This fix method must perform no tracing */
  /* operations of its own. */

  res = TraceCreate(&trace, arena, TraceStartWhyWALK);
  /* Have to fail if no trace available.  Unlikely due to .assume.parked. */
  if (res != ResOK)
    return res;

  /* .roots-walk.first-stage: In order to fool MPS_FIX12 into calling
     _mps_fix2 for a reference in a root, the reference must pass the
     first-stage test (against the summary of the trace's white
     set), so make the summary universal. */
  trace->white = ZoneSetUNIV;

  /* .roots-walk.second-stage: In order to fool _mps_fix2 into calling
     our fix function (RootsWalkFix), the reference must be to a
     segment that is white for the trace, so make all segments white
     for the trace. */
  if (SegFirst(&seg, arena)) {
    do {
      SegSetWhite(seg, TraceSetAdd(SegWhite(seg), trace));
    } while (SegNext(&seg, arena, seg));
  }

  /* Make the roots grey so that they are scanned */
  res = RootsIterate(arenaGlobals, rootWalkGrey, trace);
  /* Make this trace look like any other trace. */
  arena->flippedTraces = TraceSetAdd(arena->flippedTraces, trace);

  rootsStepClosureInit(rsc, arenaGlobals, trace, RootsWalkFix, f, p, s);
  ss = rootsStepClosure2ScanState(rsc);

  for(rank = RankMIN; rank < RankLIMIT; ++rank) {
    ss->rank = rank;
    AVERT(ScanState, ss);
    res = RootsIterate(arenaGlobals, rootWalk, (void *)ss);
    if (res != ResOK)
      break;
  }

  /* Turn segments black again. */
  if (SegFirst(&seg, arena)) {
    do {
      SegSetWhite(seg, TraceSetDel(SegWhite(seg), trace));
    } while (SegNext(&seg, arena, seg));
  }

  rootsStepClosureFinish(rsc);
  /* Make this trace look like any other finished trace. */
  trace->state = TraceFINISHED;
  TraceDestroyFinished(trace);
  AVER(!ArenaEmergency(arena)); /* There was no allocation. */

  return res;
}


/* mps_arena_roots_walk -- Client interface for walking */

void mps_arena_roots_walk(mps_arena_t mps_arena, mps_roots_stepper_t f,
                          void *p, size_t s)
{
  Arena arena = (Arena)mps_arena;
  Res res;

  ArenaEnter(arena);
  STACK_CONTEXT_BEGIN(arena) {
    AVER(FUNCHECK(f));
    /* p and s are arbitrary closures, hence can't be checked */

    AVER(ArenaGlobals(arena)->clamped);          /* .assume.parked */
    AVER(arena->busyTraces == TraceSetEMPTY);    /* .assume.parked */

    res = ArenaRootsWalk(ArenaGlobals(arena), f, p, s);
    AVER(res == ResOK);
  } STACK_CONTEXT_END(arena);
  ArenaLeave(arena);
}


/* walkNoFix -- third-stage fix function for poolWalk.
 *
 * The second-stage fix is not called via poolWalk; so this is not
 * called either. The NOTREACHED checks that this is the case.
 */
static Res walkNoFix(Seg seg, ScanState ss, Addr *refIO)
{
  AVERT(Seg, seg);
  AVERT(ScanState, ss);
  AVER(refIO != NULL);

  NOTREACHED;

  return ResUNIMPL;
}


/* poolWalkScan -- format scanner for poolWalk */

static mps_res_t poolWalkScan(mps_ss_t mps_ss, void *base, void *limit)
{
  ScanState ss = PARENT(ScanStateStruct, ss_s, mps_ss);

  AVERT(ScanState, ss);
  AVER(base != NULL);
  AVER(limit != NULL);
  AVER(base < limit);

  return ss->areaScan(mps_ss, base, limit, ss->areaScanClosure);
}


/* poolWalk -- walk formatted areas in a pool
 *
 * See <design/walk>.
 */

static Res poolWalk(Arena arena, Pool pool, mps_area_scan_t area_scan, void *closure)
{
  Trace trace;
  TraceSet ts;
  ScanStateStruct ss;
  Ring node, nextNode;
  Res res = ResOK;

  AVERT(Arena, arena);
  AVERT(Pool, pool);
  AVER(FUNCHECK(area_scan));
  /* closure is arbitrary and can't be checked */

  AVER(ArenaGlobals(arena)->clamped);          /* .assume.parked */
  AVER(arena->busyTraces == TraceSetEMPTY);    /* .assume.parked */

  /* Synthesize a flipped trace with an empty white set. The empty
   * white set means that the MPS_FIX1 test will always fail and
   * _mps_fix2 will never be called. */
  res = TraceCreate(&trace, arena, TraceStartWhyWALK);
  /* Fail if no trace available. Unlikely due to .assume.parked. */
  if (res != ResOK)
    return res;
  trace->white = ZoneSetEMPTY;
  trace->state = TraceFLIPPED;
  arena->flippedTraces = TraceSetAdd(arena->flippedTraces, trace);
  ts = TraceSetSingle(trace);

  ScanStateInit(&ss, ts, arena, RankEXACT, trace->white);
  ss.formatScan = poolWalkScan;
  ss.areaScan = area_scan;
  ss.areaScanClosure = closure;
  ss.fix = walkNoFix;

  RING_FOR(node, &pool->segRing, nextNode) {
    Bool wasTotal;
    Seg seg = SegOfPoolRing(node);
    Bool needSummary = SegRankSet(seg) != RankSetEMPTY;

    if (needSummary)
      ScanStateSetSummary(&ss, RefSetEMPTY);

    /* Expose the segment to make sure we can scan it. */
    ShieldExpose(arena, seg);
    res = SegScan(&wasTotal, seg, &ss);
    ShieldCover(arena, seg);

    if (needSummary)
      ScanStateUpdateSummary(&ss, seg, res == ResOK && wasTotal);

    if (res != ResOK)
      break;
  }

  ScanStateFinish(&ss);
  trace->state = TraceFINISHED;
  TraceDestroyFinished(trace);
  AVER(!ArenaEmergency(arena)); /* There was no allocation. */

  return res;
}


mps_res_t mps_pool_walk(mps_pool_t pool, mps_area_scan_t area_scan, void *closure)
{
  Arena arena;
  Res res;

  AVER(TESTT(Pool, pool));
  arena = PoolArena(pool);
  ArenaEnter(arena);
  AVER(FUNCHECK(area_scan));
  /* closure is arbitrary and can't be checked */

  res = poolWalk(arena, pool, area_scan, closure);
  ArenaLeave(arena);
  return res;
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
