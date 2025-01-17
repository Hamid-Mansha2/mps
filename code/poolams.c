/* poolams.c: AUTOMATIC MARK & SWEEP POOL CLASS
 *
 * $Id$
 * Copyright (c) 2001-2020 Ravenbrook Limited.  See end of file for license.
 * Portions copyright (c) 2002 Global Graphics Software.
 *
 *
 * .design: <design/poolams>.
 *
 *
 * TRANSGRESSSIONS
 *
 * .no-check.local: We have decided to omit checks in local functions of
 * structure arguments that are simply passed down through the caller
 * (as opposed to being constructed by the caller).
 */

#include "poolams.h"
#include "dbgpool.h"
#include "mpm.h"
#include <stdarg.h>

SRCID(poolams, "$Id$");


#define AMSSig          ((Sig)0x519A3599) /* SIGnature AMS */
#define AMSSegSig       ((Sig)0x519A3559) /* SIGnature AMS SeG */

static Bool amsSegBufferFill(Addr *baseReturn, Addr *limitReturn,
                             Seg seg, Size size, RankSet rankSet);
static void amsSegBufferEmpty(Seg seg, Buffer buffer);
static void amsSegBlacken(Seg seg, TraceSet traceSet);
static Res amsSegWhiten(Seg seg, Trace trace);
static Res amsSegScan(Bool *totalReturn, Seg seg, ScanState ss);
static Res amsSegFix(Seg seg, ScanState ss, Ref *refIO);
static void amsSegReclaim(Seg seg, Trace trace);
static void amsSegWalk(Seg seg, Format format, FormattedObjectsVisitor f,
                       void *p, size_t s);


/* AMSDebugStruct -- structure for a debug subclass */

typedef struct AMSDebugStruct {
  AMSStruct amsStruct;         /* AMS structure */
  PoolDebugMixinStruct debug;  /* debug mixin */
} AMSDebugStruct;

typedef struct AMSDebugStruct *AMSDebug;


#define AMS2AMSDebug(ams)  PARENT(AMSDebugStruct, amsStruct, ams)
#define AMSDebug2AMS(amsd) (&((amsd)->amsStruct))



/* AMSSegCheck -- check an AMS segment */

Bool AMSSegCheck(AMSSeg amsseg)
{
  Seg seg = MustBeA(Seg, amsseg);
  Pool pool = SegPool(seg);
  CHECKS(AMSSeg, amsseg);
  CHECKD(GCSeg, &amsseg->gcSegStruct);
  CHECKU(AMS, amsseg->ams);
  CHECKL(AMSPool(amsseg->ams) == SegPool(seg));

  CHECKL(amsseg->grains == PoolSizeGrains(pool, SegSize(seg)));
  CHECKL(amsseg->grains > 0);
  CHECKL(amsseg->grains == amsseg->freeGrains + amsseg->bufferedGrains
         + amsseg->oldGrains + amsseg->newGrains);

  CHECKL(BoolCheck(amsseg->allocTableInUse));
  if (!amsseg->allocTableInUse)
    CHECKL(amsseg->firstFree <= amsseg->grains);
  CHECKD_NOSIG(BT, amsseg->allocTable);

  if (SegWhite(seg) != TraceSetEMPTY) {
    /* <design/poolams#.colour.single> */
    CHECKL(TraceSetIsSingle(SegWhite(seg)));
    CHECKL(amsseg->colourTablesInUse);
  }

  CHECKL(BoolCheck(amsseg->marksChanged));
  CHECKL(BoolCheck(amsseg->ambiguousFixes));
  CHECKL(BoolCheck(amsseg->colourTablesInUse));
  CHECKD_NOSIG(BT, amsseg->nongreyTable);
  CHECKD_NOSIG(BT, amsseg->nonwhiteTable);

  /* If tables are shared, they mustn't both be in use. */
  CHECKL(!(amsseg->ams->shareAllocTable
           && amsseg->allocTableInUse
           && amsseg->colourTablesInUse));

  return TRUE;
}


/* AMSSegFreeWalk -- walk the free space in a segment */

void AMSSegFreeWalk(AMSSeg amsseg, FreeBlockVisitor f, void *p)
{
  Pool pool;
  Seg seg;

  AVERT(AMSSeg, amsseg);
  pool = SegPool(AMSSeg2Seg(amsseg));
  seg = AMSSeg2Seg(amsseg);

  if (amsseg->freeGrains == 0)
    return;
  if (amsseg->allocTableInUse) {
    Index base, limit, next;

    next = 0;
    while (next < amsseg->grains) {
      Bool found = BTFindLongResRange(&base, &limit, amsseg->allocTable,
                                      next, amsseg->grains, 1);
      if (!found)
        break;
      (*f)(PoolAddrOfIndex(SegBase(seg), pool, base),
           PoolAddrOfIndex(SegBase(seg), pool, limit), pool, p);
      next = limit + 1;
    }
  } else if (amsseg->firstFree < amsseg->grains)
    (*f)(PoolAddrOfIndex(SegBase(seg), pool, amsseg->firstFree),
         SegLimit(seg), pool, p);
}


/* AMSSegFreeCheck -- check the free space in a segment */

static void amsFreeBlockCheckStep(Addr base, Addr limit, Pool pool, void *p)
{
  UNUSED(p);
  DebugPoolFreeCheck(pool, base, limit);
}

void AMSSegFreeCheck(AMSSeg amsseg)
{
  Pool pool;
  PoolDebugMixin debug;

  AVERT(AMSSeg, amsseg);

  if (amsseg->freeGrains == 0)
    return;

  /* If it's not a debug class, don't bother walking. */
  pool = SegPool(AMSSeg2Seg(amsseg));
  AVERT(Pool, pool);
  debug = Method(Pool, pool, debugMixin)(pool);
  if (debug == NULL)
    return;

  AMSSegFreeWalk(amsseg, amsFreeBlockCheckStep, NULL);
}


/* amsCreateTables -- create the tables for an AMS seg */

static Res amsCreateTables(AMS ams, BT *allocReturn,
                           BT *nongreyReturn, BT *nonwhiteReturn,
                           Arena arena, Count length)
{
  Res res;
  BT allocTable, nongreyTable, nonwhiteTable;

  AVER(allocReturn != NULL);
  AVER(nongreyReturn != NULL);
  AVER(nonwhiteReturn != NULL);
  AVERT(Arena, arena);
  AVER(length > 0);

  res = BTCreate(&allocTable, arena, length);
  if (res != ResOK)
    goto failAlloc;
  res = BTCreate(&nongreyTable, arena, length);
  if (res != ResOK)
    goto failGrey;
  if (ams->shareAllocTable)
    nonwhiteTable = allocTable;
  else {
    res = BTCreate(&nonwhiteTable, arena, length);
    if (res != ResOK)
      goto failWhite;
  }

#if defined(AVER_AND_CHECK_ALL)
  /* Invalidate the colour tables in checking varieties. The algorithm
   * is designed not to depend on the initial values of these tables,
   * so by invalidating them we get some checking of this.
   */
  BTResRange(nongreyTable, 0, length);
  BTSetRange(nonwhiteTable, 0, length);
#endif

  *allocReturn = allocTable;
  *nongreyReturn = nongreyTable;
  *nonwhiteReturn = nonwhiteTable;
  return ResOK;

failWhite:
  BTDestroy(nongreyTable, arena, length);
failGrey:
  BTDestroy(allocTable, arena, length);
failAlloc:
  return res;
}


/* amsDestroyTables -- destroy the tables for an AMS seg */

static void amsDestroyTables(AMS ams, BT allocTable,
                             BT nongreyTable, BT nonwhiteTable,
                             Arena arena, Count length)
{
  AVER(allocTable != NULL);
  AVER(nongreyTable != NULL);
  AVER(nonwhiteTable != NULL);
  AVERT(Arena, arena);
  AVER(length > 0);

  if (!ams->shareAllocTable)
    BTDestroy(nonwhiteTable, arena, length);
  BTDestroy(nongreyTable, arena, length);
  BTDestroy(allocTable, arena, length);
}


/* AMSSegInit -- Init method for AMS segments */

static Res AMSSegInit(Seg seg, Pool pool, Addr base, Size size, ArgList args)
{
  AMSSeg amsseg;
  Res res;
  Arena arena;
  AMS ams;

  /* Initialize the superclass fields first via next-method call */
  res = NextMethod(Seg, AMSSeg, init)(seg, pool, base, size, args);
  if (res != ResOK)
    goto failNextMethod;
  amsseg = CouldBeA(AMSSeg, seg);

  AVERT(Pool, pool);
  ams = PoolAMS(pool);
  AVERT(AMS, ams);
  arena = PoolArena(pool);
  /* no useful checks for base and size */

  amsseg->grains = PoolSizeGrains(pool, size);
  amsseg->freeGrains = amsseg->grains;
  amsseg->bufferedGrains = (Count)0;
  amsseg->newGrains = (Count)0;
  amsseg->oldGrains = (Count)0;
  amsseg->marksChanged = FALSE; /* <design/poolams#.marked.unused> */
  amsseg->ambiguousFixes = FALSE;

  res = amsCreateTables(ams, &amsseg->allocTable,
                        &amsseg->nongreyTable, &amsseg->nonwhiteTable,
                        arena, amsseg->grains);
  if (res != ResOK)
    goto failCreateTables;

  /* start off using firstFree, see <design/poolams#.no-bit> */
  amsseg->allocTableInUse = FALSE;
  amsseg->firstFree = 0;
  amsseg->colourTablesInUse = FALSE;
  amsseg->ams = ams;
  SetClassOfPoly(seg, CLASS(AMSSeg));
  amsseg->sig = AMSSegSig;
  AVERC(AMSSeg, amsseg);

  return ResOK;

failCreateTables:
  NextMethod(Inst, AMSSeg, finish)(MustBeA(Inst, seg));
failNextMethod:
  AVER(res != ResOK);
  return res;
}


/* AMSSegFinish -- Finish method for AMS segments */

static void AMSSegFinish(Inst inst)
{
  Seg seg = MustBeA(Seg, inst);
  AMSSeg amsseg = MustBeA(AMSSeg, seg);
  AMS ams = amsseg->ams;
  Arena arena = PoolArena(AMSPool(ams));

  AVERT(AMSSeg, amsseg);
  AVER(!SegHasBuffer(seg));

  /* keep the destructions in step with AMSSegInit failure cases */
  amsDestroyTables(ams, amsseg->allocTable, amsseg->nongreyTable,
                   amsseg->nonwhiteTable, arena, amsseg->grains);

  amsseg->sig = SigInvalid;

  /* finish the superclass fields last */
  NextMethod(Inst, AMSSeg, finish)(inst);
}


/* AMSSegMerge & AMSSegSplit -- AMSSeg split & merge methods
 *
 * .empty: segment merging and splitting is limited to simple cases
 * where the high segment is empty.
 * <design/poolams#.split-merge.constrain>.
 *
 * .grain-align: segment merging and splitting is limited to cases
 * where the join is aligned with the grain alignment
 * <design/poolams#.split-merge.constrain>.
 *
 * .alloc-early: Allocations are performed before calling the
 * next method to simplify the fail cases. See
 * <design/seg#.split-merge.fail>
 *
 * .table-names: The names of local variables holding the new
 * allocation and colour tables are chosen to have names which
 * are derivable from the field names for tables in AMSSegStruct.
 * (I.e. allocTable, nongreyTable, nonwhiteTable). This simplifies
 * processing of all such tables by a macro.
 */

static Res AMSSegMerge(Seg seg, Seg segHi,
                       Addr base, Addr mid, Addr limit)
{
  Count loGrains, hiGrains, allGrains;
  AMSSeg amsseg, amssegHi;
  Pool pool;
  Arena arena;
  AMS ams;
  BT allocTable, nongreyTable, nonwhiteTable;   /* .table-names */
  Res res;

  AVERT(Seg, seg);
  AVERT(Seg, segHi);
  amsseg = Seg2AMSSeg(seg);
  amssegHi = Seg2AMSSeg(segHi);
  AVERT(AMSSeg, amsseg);
  AVERT(AMSSeg, amssegHi);
  /* other parameters are checked by next-method */
  pool = SegPool(seg);
  arena = PoolArena(pool);
  ams = PoolAMS(pool);

  loGrains = amsseg->grains;
  hiGrains = amssegHi->grains;
  allGrains = loGrains + hiGrains;

  /* checks for .grain-align */
  AVER(allGrains == PoolSizeGrains(pool, AddrOffset(base, limit)));
  /* checks for .empty */
  AVER(amssegHi->freeGrains == hiGrains);
  AVER(!amssegHi->marksChanged);

  /* .alloc-early  */
  res = amsCreateTables(ams, &allocTable, &nongreyTable, &nonwhiteTable,
                        arena, allGrains);
  if (res != ResOK)
    goto failCreateTables;

  /* Merge the superclass fields via next-method call */
  res = NextMethod(Seg, AMSSeg, merge)(seg, segHi, base, mid, limit);
  if (res != ResOK)
    goto failSuper;

  /* Update fields of seg. Finish segHi. */

#define MERGE_TABLES(table, setHighRangeFn) \
  /* Implementation depends on .table-names */ \
  BEGIN \
    BTCopyRange(amsseg->table, (table), 0, loGrains); \
    setHighRangeFn((table), loGrains, allGrains); \
    BTDestroy(amsseg->table, arena, loGrains); \
    BTDestroy(amssegHi->table, arena, hiGrains); \
    amsseg->table = (table); \
  END

  MERGE_TABLES(allocTable, BTResRange);
  MERGE_TABLES(nongreyTable, BTSetRange);
  if (!ams->shareAllocTable)
    MERGE_TABLES(nonwhiteTable, BTSetRange);

  amsseg->grains = allGrains;
  amsseg->freeGrains = amsseg->freeGrains + amssegHi->freeGrains;
  amsseg->bufferedGrains = amsseg->bufferedGrains + amssegHi->bufferedGrains;
  amsseg->newGrains = amsseg->newGrains + amssegHi->newGrains;
  amsseg->oldGrains = amsseg->oldGrains + amssegHi->oldGrains;
  /* other fields in amsseg are unaffected */

  amssegHi->sig = SigInvalid;

  AVERT(AMSSeg, amsseg);
  PoolGenAccountForSegMerge(ams->pgen);
  return ResOK;

failSuper:
  amsDestroyTables(ams, allocTable, nongreyTable, nonwhiteTable,
                   arena, allGrains);
failCreateTables:
  AVERT(AMSSeg, amsseg);
  AVERT(AMSSeg, amssegHi);
  return res;
}


static Res AMSSegSplit(Seg seg, Seg segHi,
                       Addr base, Addr mid, Addr limit)
{
  Count loGrains, hiGrains, allGrains;
  AMSSeg amsseg, amssegHi;
  Pool pool;
  Arena arena;
  AMS ams;
  BT allocTableLo, nongreyTableLo, nonwhiteTableLo; /* .table-names */
  BT allocTableHi, nongreyTableHi, nonwhiteTableHi; /* .table-names */
  Res res;

  AVERT(Seg, seg);
  AVER(segHi != NULL);  /* can't check fully, it's not initialized */
  amsseg = Seg2AMSSeg(seg);
  amssegHi = Seg2AMSSeg(segHi);
  AVERT(AMSSeg, amsseg);
  /* other parameters are checked by next-method */
  pool = SegPool(seg);
  arena = PoolArena(pool);
  ams = PoolAMS(pool);

  loGrains = PoolSizeGrains(pool, AddrOffset(base, mid));
  hiGrains = PoolSizeGrains(pool, AddrOffset(mid, limit));
  allGrains = loGrains + hiGrains;

  /* checks for .grain-align */
  AVER(allGrains == amsseg->grains);
  /* checks for .empty */
  AVER(amsseg->freeGrains >= hiGrains);
  if (amsseg->allocTableInUse) {
    AVER(BTIsResRange(amsseg->allocTable, loGrains, allGrains));
  } else {
    AVER(amsseg->firstFree <= loGrains);
  }

  /* .alloc-early */
  res = amsCreateTables(ams, &allocTableLo, &nongreyTableLo, &nonwhiteTableLo,
                        arena, loGrains);
  if (res != ResOK)
    goto failCreateTablesLo;
  res = amsCreateTables(ams, &allocTableHi, &nongreyTableHi, &nonwhiteTableHi,
                        arena, hiGrains);
  if (res != ResOK)
    goto failCreateTablesHi;

  /* Split the superclass fields via next-method call */
  res = NextMethod(Seg, AMSSeg, split)(seg, segHi, base, mid, limit);
  if (res != ResOK)
    goto failSuper;

  /* Update seg. Full initialization for segHi. */

#define SPLIT_TABLES(table, setHighRangeFn) \
  /* Implementation depends on .table-names */ \
  BEGIN \
    BTCopyRange(amsseg->table, table ## Lo, 0, loGrains); \
    setHighRangeFn(table ## Hi, 0, hiGrains); \
    BTDestroy(amsseg->table, arena, allGrains); \
    amsseg->table = table ## Lo; \
    amssegHi->table = table ## Hi; \
  END

  SPLIT_TABLES(nonwhiteTable, BTSetRange);
  SPLIT_TABLES(nongreyTable, BTSetRange);
  SPLIT_TABLES(allocTable, BTResRange);

  amsseg->grains = loGrains;
  amssegHi->grains = hiGrains;
  AVER(amsseg->freeGrains >= hiGrains);
  amsseg->freeGrains -= hiGrains;
  amssegHi->freeGrains = hiGrains;
  amssegHi->bufferedGrains = (Count)0;
  amssegHi->newGrains = (Count)0;
  amssegHi->oldGrains = (Count)0;
  amssegHi->marksChanged = FALSE; /* <design/poolams#.marked.unused> */
  amssegHi->ambiguousFixes = FALSE;

  /* start off using firstFree, see <design/poolams#.no-bit> */
  amssegHi->allocTableInUse = FALSE;
  amssegHi->firstFree = 0;
  /* use colour tables if the segment is white */
  amssegHi->colourTablesInUse = (SegWhite(segHi) != TraceSetEMPTY);
  amssegHi->ams = ams;
  amssegHi->sig = AMSSegSig;
  AVERT(AMSSeg, amsseg);
  AVERT(AMSSeg, amssegHi);
  PoolGenAccountForSegSplit(ams->pgen);
  return ResOK;

failSuper:
  amsDestroyTables(ams, allocTableHi, nongreyTableHi, nonwhiteTableHi,
                   arena, hiGrains);
failCreateTablesHi:
  amsDestroyTables(ams, allocTableLo, nongreyTableLo, nonwhiteTableLo,
                   arena, loGrains);
failCreateTablesLo:
  AVERT(AMSSeg, amsseg);
  return res;
}


/* AMSSegDescribe -- describe an AMS segment */

#define WRITE_BUFFER_LIMIT(i, accessor, code) \
  BEGIN \
  if (hasBuffer && \
      (i) == PoolIndexOfAddr(SegBase(seg), SegPool(seg), accessor(buffer))) \
    { \
      Res _res = WriteF(stream, 0, code, NULL); \
      if (_res != ResOK) return _res; \
    } \
  END

static Res AMSSegDescribe(Inst inst, mps_lib_FILE *stream, Count depth)
{
  AMSSeg amsseg = CouldBeA(AMSSeg, inst);
  Seg seg = CouldBeA(Seg, amsseg);
  Res res;
  Buffer buffer;
  Bool hasBuffer;
  Index i;

  if (!TESTC(AMSSeg, amsseg))
    return ResPARAM;
  if (stream == NULL)
    return ResPARAM;

  /* Describe the superclass fields first via next-method call */
  res = NextMethod(Inst, AMSSeg, describe)(inst, stream, depth);
  if (res != ResOK)
    return res;

  hasBuffer = SegBuffer(&buffer, seg);

  res = WriteF(stream, depth + 2,
               "AMS $P\n", (WriteFP)amsseg->ams,
               "grains $W\n", (WriteFW)amsseg->grains,
               "freeGrains $W\n", (WriteFW)amsseg->freeGrains,
               "buffferedGrains $W\n", (WriteFW)amsseg->bufferedGrains,
               "newGrains $W\n", (WriteFW)amsseg->newGrains,
               "oldGrains $W\n", (WriteFW)amsseg->oldGrains,
               NULL);
  if (res != ResOK)
    return res;
  if (amsseg->allocTableInUse)
    res = WriteF(stream, depth,
                 "alloctable $P\n", (WriteFP)amsseg->allocTable,
                 NULL);
  else
    res = WriteF(stream, depth,
                 "firstFree $W\n", (WriteFW)amsseg->firstFree,
                 NULL);
  if (res != ResOK)
    return res;
  res = WriteF(stream, depth,
               "tables: nongrey $P, nonwhite $P\n",
               (WriteFP)amsseg->nongreyTable,
               (WriteFP)amsseg->nonwhiteTable,
               "map:",
               NULL);
  if (res != ResOK)
    return res;

  for (i=0; i < amsseg->grains; ++i) {
    char c = 0;

    if (i % 64 == 0) {
      res = WriteF(stream, 0, "\n", NULL);
      if (res != ResOK)
        return res;
      res = WriteF(stream, depth, "  ", NULL);
      if (res != ResOK)
        return res;
    }

    WRITE_BUFFER_LIMIT(i, BufferBase,    "[");
    WRITE_BUFFER_LIMIT(i, BufferGetInit, "|");
    WRITE_BUFFER_LIMIT(i, BufferAlloc,   ">");

    if (AMS_ALLOCED(seg, i)) {
      if (amsseg->colourTablesInUse) {
        if (AMS_IS_INVALID_COLOUR(seg, i))
          c = '!';
        else if (AMS_IS_WHITE(seg, i))
          c = '-';
        else if (AMS_IS_GREY(seg, i))
          c = '+';
        else /* must be black */
          c = '*';
      } else
        c = '.';
    } else
      c = ' ';
    res = WriteF(stream, 0, "$C", (WriteFC)c, NULL);
    if (res != ResOK)
      return res;

    WRITE_BUFFER_LIMIT(i+1, BufferScanLimit, "<");
    WRITE_BUFFER_LIMIT(i+1, BufferLimit,     "]");
  }

  return ResOK;
}


/* AMSSegClass -- Class definition for AMS segments */

DEFINE_CLASS(Seg, AMSSeg, klass)
{
  INHERIT_CLASS(klass, AMSSeg, MutatorSeg);
  klass->instClassStruct.describe = AMSSegDescribe;
  klass->instClassStruct.finish = AMSSegFinish;
  klass->size = sizeof(AMSSegStruct);
  klass->init = AMSSegInit;
  klass->bufferFill = amsSegBufferFill;
  klass->bufferEmpty = amsSegBufferEmpty;
  klass->merge = AMSSegMerge;
  klass->split = AMSSegSplit;
  klass->whiten = amsSegWhiten;
  klass->blacken = amsSegBlacken;
  klass->scan = amsSegScan;
  klass->fix = amsSegFix;
  klass->fixEmergency = amsSegFix;
  klass->reclaim = amsSegReclaim;
  klass->walk = amsSegWalk;
  AVERT(SegClass, klass);
}


/* AMSSegSizePolicy
 *
 * Picks a segment size.  This policy simply rounds the size
 * up to the arena grain size.
 */
static Res AMSSegSizePolicy(Size *sizeReturn,
                            Pool pool, Size size, RankSet rankSet)
{
  Arena arena;

  AVER(sizeReturn != NULL);
  AVERT(Pool, pool);
  AVER(size > 0);
  AVERT(RankSet, rankSet);

  arena = PoolArena(pool);

  size = SizeArenaGrains(size, arena);
  if (size == 0) {
    /* overflow */
    return ResMEMORY;
  }
  *sizeReturn = size;
  return ResOK;
}


/* AMSSegCreate -- create a single AMSSeg */

static Res AMSSegCreate(Seg *segReturn, Pool pool, Size size,
                        RankSet rankSet)
{
  Seg seg;
  AMS ams;
  Res res;
  Arena arena;
  Size prefSize;

  AVER(segReturn != NULL);
  AVERT(Pool, pool);
  AVER(size > 0);
  AVERT(RankSet, rankSet);

  ams = PoolAMS(pool);
  AVERT(AMS,ams);
  arena = PoolArena(pool);

  res = ams->segSize(&prefSize, pool, size, rankSet);
  if (res != ResOK)
    goto failSize;

  res = PoolGenAlloc(&seg, ams->pgen, (*ams->segClass)(), prefSize,
                     argsNone);
  if (res != ResOK) { /* try to allocate one that's just large enough */
    Size minSize = SizeArenaGrains(size, arena);
    if (minSize == prefSize)
      goto failSeg;
    res = PoolGenAlloc(&seg, ams->pgen, (*ams->segClass)(), prefSize,
                       argsNone);
    if (res != ResOK)
      goto failSeg;
  }

  /* see <design/seg#.field.rankset> */
  if (rankSet != RankSetEMPTY) {
    SegSetRankAndSummary(seg, rankSet, RefSetUNIV);
  } else {
    SegSetRankAndSummary(seg, rankSet, RefSetEMPTY);
  }
  DebugPoolFreeSplat(pool, SegBase(seg), SegLimit(seg));

  AVERT(AMSSeg, Seg2AMSSeg(seg));

  *segReturn = seg;
  return ResOK;

failSeg:
failSize:
  return res;
}


/* AMSSegsDestroy -- destroy all the segments */

static void AMSSegsDestroy(AMS ams)
{
  Pool pool = AMSPool(ams);
  Ring ring, node, next;     /* for iterating over the segments */

  ring = PoolSegRing(AMSPool(ams));
  RING_FOR(node, ring, next) {
    Seg seg = SegOfPoolRing(node);
    AMSSeg amsseg = Seg2AMSSeg(seg);
    AVER(!SegHasBuffer(seg));
    AVERT(AMSSeg, amsseg);
    AVER(amsseg->ams == ams);
    AVER(amsseg->bufferedGrains == 0);
    AMSSegFreeCheck(amsseg);
    PoolGenFree(ams->pgen, seg,
                PoolGrainsSize(pool, amsseg->freeGrains),
                PoolGrainsSize(pool, amsseg->oldGrains),
                PoolGrainsSize(pool, amsseg->newGrains),
                FALSE);
  }
}


/* AMSVarargs -- decode obsolete varargs */

static void AMSVarargs(ArgStruct args[MPS_ARGS_MAX], va_list varargs)
{
  args[0].key = MPS_KEY_FORMAT;
  args[0].val.format = va_arg(varargs, Format);
  args[1].key = MPS_KEY_CHAIN;
  args[1].val.chain = va_arg(varargs, Chain);
  args[2].key = MPS_KEY_AMS_SUPPORT_AMBIGUOUS;
  args[2].val.b = va_arg(varargs, Bool);
  args[3].key = MPS_KEY_ARGS_END;
  AVERT(ArgList, args);
}

static void AMSDebugVarargs(ArgStruct args[MPS_ARGS_MAX], va_list varargs)
{
  args[0].key = MPS_KEY_POOL_DEBUG_OPTIONS;
  args[0].val.pool_debug_options = va_arg(varargs, mps_pool_debug_option_s *);
  AMSVarargs(args + 1, varargs);
}


/* AMSInit -- the pool class initialization method
 *
 *  Takes one additional argument: the format of the objects
 *  allocated in the pool.  <design/poolams#.init>.
 */

ARG_DEFINE_KEY(AMS_SUPPORT_AMBIGUOUS, Bool);

static Res AMSInit(Pool pool, Arena arena, PoolClass klass, ArgList args)
{
  Res res;
  Chain chain;
  Bool supportAmbiguous = AMS_SUPPORT_AMBIGUOUS_DEFAULT;
  unsigned gen = AMS_GEN_DEFAULT;
  ArgStruct arg;
  AMS ams;

  AVER(pool != NULL);
  AVERT(Arena, arena);
  AVERT(ArgList, args);
  UNUSED(klass); /* used for debug pools only */

  if (ArgPick(&arg, args, MPS_KEY_CHAIN))
    chain = arg.val.chain;
  else {
    chain = ArenaGlobals(arena)->defaultChain;
    gen = 1; /* avoid the nursery of the default chain by default */
  }
  if (ArgPick(&arg, args, MPS_KEY_GEN))
    gen = arg.val.u;
  if (ArgPick(&arg, args, MPS_KEY_AMS_SUPPORT_AMBIGUOUS))
    supportAmbiguous = arg.val.b;

  AVERT(Chain, chain);
  AVER(gen <= ChainGens(chain));
  AVER(chain->arena == arena);

  res = NextMethod(Pool, AMSPool, init)(pool, arena, klass, args);
  if (res != ResOK)
    goto failNextInit;
  ams = CouldBeA(AMSPool, pool);

  /* Ensure a format was supplied in the argument list. */
  AVER(pool->format != NULL);
  pool->alignment = pool->format->alignment;
  pool->alignShift = SizeLog2(pool->alignment);
  /* .ambiguous.noshare: If the pool is required to support ambiguous */
  /* references, the alloc and white tables cannot be shared. */
  ams->shareAllocTable = !supportAmbiguous;
  ams->pgen = NULL;

  /* The next four might be overridden by a subclass. */
  ams->segSize = AMSSegSizePolicy;
  ams->segsDestroy = AMSSegsDestroy;
  ams->segClass = AMSSegClassGet;

  SetClassOfPoly(pool, CLASS(AMSPool));
  ams->sig = AMSSig;
  AVERC(AMS, ams);

  res = PoolGenInit(&ams->pgenStruct, ChainGen(chain, gen), pool);
  if (res != ResOK)
    goto failGenInit;
  ams->pgen = &ams->pgenStruct;

  EVENT2(PoolInitAMS, pool, pool->format);

  return ResOK;

failGenInit:
  NextMethod(Inst, AMSPool, finish)(MustBeA(Inst, pool));
failNextInit:
  AVER(res != ResOK);
  return res;
}


/* AMSFinish -- the pool class finishing method
 *
 * Destroys all the segs in the pool.  Can't invalidate the AMS until
 * we've destroyed all the segments, as it may be checked.
 */
void AMSFinish(Inst inst)
{
  Pool pool = MustBeA(AbstractPool, inst);
  AMS ams = MustBeA(AMSPool, pool);

  AVERT(AMS, ams);

  ams->segsDestroy(ams);
  /* can't invalidate the AMS until we've destroyed all the segs */
  ams->sig = SigInvalid;
  PoolGenFinish(ams->pgen);
  ams->pgen = NULL;

  NextMethod(Inst, AMSPool, finish)(inst);
}


/* amsSegBufferFill -- try filling buffer from segment */

static Bool amsSegBufferFill(Addr *baseReturn, Addr *limitReturn,
                             Seg seg, Size size, RankSet rankSet)
{
  Index baseIndex, limitIndex;
  AMSSeg amsseg = MustBeA(AMSSeg, seg);
  Pool pool = SegPool(seg);
  Count requestedGrains, segGrains, allocatedGrains;
  Addr segBase, base, limit;

  AVER(baseReturn != NULL);
  AVER(limitReturn != NULL);
  AVER(SizeIsAligned(size, PoolAlignment(pool)));
  AVER(size > 0);
  AVERT(RankSet, rankSet);

  requestedGrains = PoolSizeGrains(pool, size);
  if (amsseg->freeGrains < requestedGrains)
    /* Not enough space to satisfy the request. */
    return FALSE;

  if (SegHasBuffer(seg))
    /* Don't bother trying to allocate from a buffered segment */
    return FALSE;

  if (RefSetUnion(SegWhite(seg), SegGrey(seg)) != TraceSetEMPTY)
    /* Can't use a white or grey segment, see <design/poolams#.fill.colour> */
    return FALSE;

  if (rankSet != SegRankSet(seg))
    /* Can't satisfy required rank set. */
    return FALSE;

  segGrains = PoolSizeGrains(pool, SegSize(seg));
  if (amsseg->freeGrains == segGrains) {
    /* Whole segment is free: no need for a search. */
    baseIndex = 0;
    limitIndex = segGrains;
    goto found;
  }

  /* We don't place buffers on white segments, so no need to adjust colour. */
  AVER(!amsseg->colourTablesInUse);

  if (amsseg->allocTableInUse) {
    if (!BTFindLongResRange(&baseIndex, &limitIndex, amsseg->allocTable,
                            0, segGrains, requestedGrains))
      return FALSE;
  } else {
    if (amsseg->firstFree > segGrains - requestedGrains)
      return FALSE;
    baseIndex = amsseg->firstFree;
    limitIndex = segGrains;
  }

found:
  AVER(baseIndex < limitIndex);
  if (amsseg->allocTableInUse) {
    BTSetRange(amsseg->allocTable, baseIndex, limitIndex);
  } else {
    amsseg->firstFree = limitIndex;
  }
  allocatedGrains = limitIndex - baseIndex;
  AVER(requestedGrains <= allocatedGrains);
  AVER(amsseg->freeGrains >= allocatedGrains);
  amsseg->freeGrains -= allocatedGrains;
  amsseg->bufferedGrains += allocatedGrains;

  segBase = SegBase(seg);
  base = PoolAddrOfIndex(segBase, pool, baseIndex);
  limit = PoolAddrOfIndex(segBase, pool, limitIndex);
  PoolGenAccountForFill(PoolSegPoolGen(pool, seg), AddrOffset(base, limit));
  DebugPoolFreeCheck(pool, base, limit);

  *baseReturn = base;
  *limitReturn = limit;
  return TRUE;
}


/* AMSBufferFill -- the pool class buffer fill method
 *
 * Iterates over the segments looking for space.  See
 * <design/poolams#.fill>.
 */
static Res AMSBufferFill(Addr *baseReturn, Addr *limitReturn,
                         Pool pool, Buffer buffer, Size size)
{
  Res res;
  Ring node, nextNode;
  RankSet rankSet;
  Seg seg;
  Bool b;

  AVER(baseReturn != NULL);
  AVER(limitReturn != NULL);
  AVERC(Buffer, buffer);
  AVER(BufferIsReset(buffer));
  AVER(size > 0);
  AVER(SizeIsAligned(size, PoolAlignment(pool)));

  /* Check that we're not in the grey mutator phase */
  /* <design/poolams#.fill.colour>. */
  AVER(PoolArena(pool)->busyTraces == PoolArena(pool)->flippedTraces);

  /* <design/poolams#.fill.slow> */
  rankSet = BufferRankSet(buffer);
  RING_FOR(node, &pool->segRing, nextNode) {
    seg = SegOfPoolRing(node);
    if (SegBufferFill(baseReturn, limitReturn, seg, size, rankSet))
      return ResOK;
  }

  /* No segment had enough space, so make a new one. */
  res = AMSSegCreate(&seg, pool, size, BufferRankSet(buffer));
  if (res != ResOK)
    return res;
  b = SegBufferFill(baseReturn, limitReturn, seg, size, rankSet);
  AVER(b);
  return ResOK;
}


/* amsSegBufferEmpty -- empty buffer to segment
 *
 * Frees the unused part of the buffer.  The colour of the area doesn't
 * need to be changed.  <design/poolams#.empty>.
 */
static void amsSegBufferEmpty(Seg seg, Buffer buffer)
{
  AMSSeg amsseg = MustBeA(AMSSeg, seg);
  Pool pool = SegPool(seg);
  Addr segBase, bufferBase, init, limit;
  Index initIndex, limitIndex;
  Count usedGrains, unusedGrains;

  AVERT(Seg, seg);
  AVERT(Buffer, buffer);
  segBase = SegBase(seg);
  bufferBase = BufferBase(buffer);
  init = BufferGetInit(buffer);
  limit = BufferLimit(buffer);
  AVER(segBase <= bufferBase);
  AVER(bufferBase <= init);
  AVER(init <= limit);
  AVER(limit <= SegLimit(seg));

  initIndex = PoolIndexOfAddr(segBase, pool, init);
  limitIndex = PoolIndexOfAddr(segBase, pool, limit);

  if (initIndex < limitIndex) {
    AMS ams = MustBeA(AMSPool, pool);

    /* Tripped allocations might have scribbled on it, need to splat again. */
    DebugPoolFreeSplat(pool, init, limit);

    if (amsseg->allocTableInUse) {
      /* check that it's allocated */
      AVER(BTIsSetRange(amsseg->allocTable, initIndex, limitIndex));
      BTResRange(amsseg->allocTable, initIndex, limitIndex);
    } else {
      /* check that it's allocated */
      AVER(limitIndex <= amsseg->firstFree);
      if (limitIndex == amsseg->firstFree) /* is it at the end? */ {
        amsseg->firstFree = initIndex;
      } else if (ams->shareAllocTable && amsseg->colourTablesInUse) {
        /* The nonwhiteTable is shared with allocTable and in use, so we
         * mustn't start using allocTable. In this case we know: 1. the
         * segment has been condemned (because colour tables are turned on
         * in amsSegWhiten); 2. the segment has not yet been reclaimed
         * (because colour tables are turned off in amsSegReclaim); 3. the
         * unused portion of the buffer is black (see amsSegWhiten). So we
         * need to whiten the unused portion of the buffer. The allocTable
         * will be turned back on (if necessary) in amsSegReclaim, when we
         * know that the nonwhite grains are exactly the allocated grains.
         */
      } else {
        /* start using allocTable */
        amsseg->allocTableInUse = TRUE;
        BTSetRange(amsseg->allocTable, 0, amsseg->firstFree);
        if (amsseg->firstFree < amsseg->grains)
          BTResRange(amsseg->allocTable, amsseg->firstFree, amsseg->grains);
        BTResRange(amsseg->allocTable, initIndex, limitIndex);
      }
    }

    if (amsseg->colourTablesInUse)
      AMS_RANGE_WHITEN(seg, initIndex, limitIndex);
  }

  unusedGrains = limitIndex - initIndex;
  AVER(unusedGrains <= amsseg->bufferedGrains);
  usedGrains = amsseg->bufferedGrains - unusedGrains;
  amsseg->freeGrains += unusedGrains;
  amsseg->bufferedGrains = 0;
  amsseg->newGrains += usedGrains;

  PoolGenAccountForEmpty(PoolSegPoolGen(pool, seg),
                         PoolGrainsSize(pool, usedGrains),
                         PoolGrainsSize(pool, unusedGrains), FALSE);
}


/* amsSegPoolGen -- get pool generation for an AMS segment */

static PoolGen amsSegPoolGen(Pool pool, Seg seg)
{
  AMS ams = MustBeA(AMSPool, pool);
  AVERT(Seg, seg);
  return ams->pgen;
}


/* amsSegRangeWhiten -- Condemn a part of an AMS segment
 * Allow calling it with base = limit, to simplify the callers.
 */
static void amsSegRangeWhiten(Seg seg, Index base, Index limit)
{
  if (base != limit) {
    AMSSeg amsseg = Seg2AMSSeg(seg);

    AVER(base < limit);
    AVER(limit <= amsseg->grains);

    AMS_RANGE_WHITEN(seg, base, limit);
  }
}


/* amsSegWhiten -- the pool class segment condemning method */

static Res amsSegWhiten(Seg seg, Trace trace)
{
  Buffer buffer;                /* the seg's buffer, if it has one */
  Count agedGrains, uncondemnedGrains;
  AMSSeg amsseg = MustBeA(AMSSeg, seg);
  Pool pool = SegPool(seg);
  PoolGen pgen = PoolSegPoolGen(pool, seg);

  AVERT(Trace, trace);

  /* <design/poolams#.colour.single> */
  AVER(SegWhite(seg) == TraceSetEMPTY);
  AVER(!amsseg->colourTablesInUse);

  amsseg->colourTablesInUse = TRUE;

  /* Init allocTable, if necessary. */
  if (!amsseg->allocTableInUse) {
    if (0 < amsseg->firstFree)
      BTSetRange(amsseg->allocTable, 0, amsseg->firstFree);
    if (amsseg->firstFree < amsseg->grains)
      BTResRange(amsseg->allocTable, amsseg->firstFree, amsseg->grains);
  }

  /* Start using allocTable as the white table, if so configured. */
  if (amsseg->ams->shareAllocTable) {
    if (amsseg->allocTableInUse) {
      /* During the collection, it can't use allocTable for AMS_ALLOCED, so */
      /* make it use firstFree. */
      amsseg->allocTableInUse = FALSE;
      /* Could find a better value for firstFree, but probably not worth it. */
      amsseg->firstFree = amsseg->grains;
    }
  } else { /* Otherwise, use it as alloc table. */
    amsseg->allocTableInUse = TRUE;
  }

  if (SegBuffer(&buffer, seg)) { /* <design/poolams#.condemn.buffer> */
    Index scanLimitIndex, limitIndex;
    scanLimitIndex = PoolIndexOfAddr(SegBase(seg), pool, BufferScanLimit(buffer));
    limitIndex = PoolIndexOfAddr(SegBase(seg), pool, BufferLimit(buffer));

    amsSegRangeWhiten(seg, 0, scanLimitIndex);
    if (scanLimitIndex < limitIndex)
      AMS_RANGE_BLACKEN(seg, scanLimitIndex, limitIndex);
    amsSegRangeWhiten(seg, limitIndex, amsseg->grains);
    /* We didn't condemn the buffer, subtract it from the count. */
    uncondemnedGrains = limitIndex - scanLimitIndex;
  } else { /* condemn whole seg */
    amsSegRangeWhiten(seg, 0, amsseg->grains);
    uncondemnedGrains = (Count)0;
  }

  /* The unused part of the buffer remains buffered: the rest becomes old. */
  AVER(amsseg->bufferedGrains >= uncondemnedGrains);
  agedGrains = amsseg->bufferedGrains - uncondemnedGrains;
  PoolGenAccountForAge(pgen, PoolGrainsSize(pool, agedGrains),
                       PoolGrainsSize(pool, amsseg->newGrains), FALSE);
  amsseg->oldGrains += agedGrains + amsseg->newGrains;
  amsseg->bufferedGrains = uncondemnedGrains;
  amsseg->newGrains = 0;
  amsseg->marksChanged = FALSE; /* <design/poolams#.marked.condemn> */
  amsseg->ambiguousFixes = FALSE;

  if (amsseg->oldGrains > 0) {
    GenDescCondemned(pgen->gen, trace,
                     PoolGrainsSize(pool, amsseg->oldGrains));
    SegSetWhite(seg, TraceSetAdd(SegWhite(seg), trace));
  } else {
    amsseg->colourTablesInUse = FALSE;
  }

  return ResOK;
}


/* AMSObjectFunction is the type of the method that an */
/* amsIterate applies to each object in a segment. */
typedef Res (*AMSObjectFunction)(
  /* the segment */              Seg seg,
  /* the object grain index */   Index i,
  /* the address of the object */Addr p,
  /*  "   "   after the object */Addr next,
  /* the iteration closure */    void *closure);

#define AMSObjectFunctionCheck(f) \
  ((f) != NULL) /* that's the best we can do */


/* semSegIterate -- applies a function to each object in a segment
 *
 * semSegIterate(seg, f, closure) applies f to all the objects in the
 * segment.  It skips the buffer, if any (from BufferScanLimit to
 * BufferLimit).  */

static Res semSegIterate(Seg seg, AMSObjectFunction f, void *closure)
{
  Res res;
  Pool pool;
  AMSSeg amsseg;
  Format format;
  Align alignment;
  Index i;
  Addr p, next, limit;
  Buffer buffer;
  Bool hasBuffer;

  AVERT(Seg, seg);
  AVERT(AMSObjectFunction, f);
  /* Can't check closure */

  amsseg = Seg2AMSSeg(seg);
  AVERT(AMSSeg, amsseg);
  pool = SegPool(seg);
  AVERT(Pool, pool);
  format = pool->format;
  AVERT(Format, format);
  alignment = PoolAlignment(pool);

  /* If we're using the alloc table as a white table, we can't use it to */
  /* determine where there are objects. */
  AVER(!amsseg->ams->shareAllocTable || !amsseg->colourTablesInUse);

  p = SegBase(seg);
  limit = SegLimit(seg);
  hasBuffer = SegBuffer(&buffer, seg);

  while (p < limit) { /* loop over the objects in the segment */
    if (hasBuffer && p == BufferScanLimit(buffer) && p != BufferLimit(buffer)) {
      /* skip buffer */
      next = BufferLimit(buffer);
      AVER(AddrIsAligned(next, alignment));
    } else {
      AVER(!hasBuffer
           || (p < BufferScanLimit(buffer))
           || (p >= BufferLimit(buffer)));  /* not in the buffer */

      i = PoolIndexOfAddr(SegBase(seg), pool, p);
      if (!AMS_ALLOCED(seg, i)) { /* no object here */
        if (amsseg->allocTableInUse) {
          Index dummy, nextIndex;
          Bool more;

          /* Find out how large the free block is. */
          more = BTFindLongResRange(&dummy, &nextIndex, amsseg->allocTable,
                                    i, amsseg->grains, 1);
          AVER(more);
          AVER(dummy == i);
          next = PoolAddrOfIndex(SegBase(seg), pool, nextIndex);
        } else {
          /* If there's no allocTable, this is the free block at the end. */
          next = limit;
        }
      } else { /* there is an object here */
        if (format->skip != NULL) {
            next = (*format->skip)(AddrAdd(p, format->headerSize));
            next = AddrSub(next, format->headerSize);
        } else {
          next = AddrAdd(p, alignment);
        }
        AVER(AddrIsAligned(next, alignment));
        res = (*f)(seg, i, p, next, closure);
        if (res != ResOK)
          return res;
      }
    }
    AVER(next > p); /* make sure we make progress */
    p = next;
  }
  AVER(p == limit);
  return ResOK;
}


/* amsScanObject -- scan a single object
 *
 * This is the object function passed to semSegIterate by amsSegScan.  */

struct amsScanClosureStruct {
  ScanState ss;
  Bool scanAllObjects;
};

typedef struct amsScanClosureStruct *amsScanClosure;

static Res amsScanObject(Seg seg, Index i, Addr p, Addr next, void *clos)
{
  amsScanClosure closure;
  AMSSeg amsseg;
  Format format;
  Res res;

  amsseg = Seg2AMSSeg(seg);
  /* seg & amsseg have already been checked, in semSegIterate. */
  AVER(i < amsseg->grains);
  AVER(p != 0);
  AVER(p < next);
  AVER(clos != NULL);
  closure = (amsScanClosure)clos;
  AVERT(ScanState, closure->ss);
  AVERT(Bool, closure->scanAllObjects);

  format = AMSPool(amsseg->ams)->format;
  AVERT(Format, format);

  /* @@@@ This isn't quite right for multiple traces. */
  if (closure->scanAllObjects || AMS_IS_GREY(seg, i)) {
    res = TraceScanFormat(closure->ss,
                          AddrAdd(p, format->headerSize),
                          AddrAdd(next, format->headerSize));
    if (res != ResOK)
      return res;
    if (!closure->scanAllObjects) {
      Index j = PoolIndexOfAddr(SegBase(seg), SegPool(seg), next);
      AVER(!AMS_IS_INVALID_COLOUR(seg, i));
      AMS_GREY_BLACKEN(seg, i);
      if (i+1 < j)
        AMS_RANGE_WHITE_BLACKEN(seg, i+1, j);
    }
  }

  return ResOK;
}


/* amsSegScan -- the segment scanning method
 *
 * <design/poolams#.scan>
 */
static Res amsSegScan(Bool *totalReturn, Seg seg, ScanState ss)
{
  Res res;
  AMSSeg amsseg = MustBeA(AMSSeg, seg);
  Pool pool = SegPool(seg);
  AMS ams = MustBeA(AMSPool, pool);
  Arena arena = PoolArena(pool);
  struct amsScanClosureStruct closureStruct;
  Format format;
  Align alignment;

  AVER(totalReturn != NULL);
  AVERT(ScanState, ss);

  /* Check that we're not in the grey mutator phase */
  /* <design/poolams#.not-req.grey>. */
  AVER(TraceSetSub(ss->traces, arena->flippedTraces));

  closureStruct.scanAllObjects =
    (TraceSetDiff(ss->traces, SegWhite(seg)) != TraceSetEMPTY);
  closureStruct.ss = ss;
  /* @@@@ This isn't quite right for multiple traces. */
  if (closureStruct.scanAllObjects) {
    /* The whole seg (except the buffer) is grey for some trace. */
    res = semSegIterate(seg, amsScanObject, &closureStruct);
    if (res != ResOK) {
      *totalReturn = FALSE;
      return res;
    }
    *totalReturn = TRUE;
  } else {
    AVER(amsseg->marksChanged); /* something must have changed */
    AVER(amsseg->colourTablesInUse);
    format = pool->format;
    AVERT(Format, format);
    alignment = PoolAlignment(AMSPool(ams));
    do { /* <design/poolams#.scan.iter> */
      amsseg->marksChanged = FALSE; /* <design/poolams#.marked.scan> */
      /* <design/poolams#.ambiguous.middle> */
      if (amsseg->ambiguousFixes) {
        res = semSegIterate(seg, amsScanObject, &closureStruct);
        if (res != ResOK) {
          /* <design/poolams#.marked.scan.fail> */
          amsseg->marksChanged = TRUE;
          *totalReturn = FALSE;
          return res;
        }
      } else {
        Index i, j = 0;
        Addr p, next;

        while(j < amsseg->grains
              && AMSFindGrey(&i, &j, seg, j, amsseg->grains)) {
          Addr clientP, clientNext;
          AVER(!AMS_IS_INVALID_COLOUR(seg, i));
          p = PoolAddrOfIndex(SegBase(seg), pool, i);
          clientP = AddrAdd(p, format->headerSize);
          if (format->skip != NULL) {
            clientNext = (*format->skip)(clientP);
            next = AddrSub(clientNext, format->headerSize);
          } else {
            clientNext = AddrAdd(clientP, alignment);
            next = AddrAdd(p, alignment);
          }
          j = PoolIndexOfAddr(SegBase(seg), pool, next);
          res = TraceScanFormat(ss, clientP, clientNext);
          if (res != ResOK) {
            /* <design/poolams#.marked.scan.fail> */
            amsseg->marksChanged = TRUE;
            *totalReturn = FALSE;
            return res;
          }
          /* Check that there haven't been any ambiguous fixes during the */
          /* scan, because AMSFindGrey won't work otherwise. */
          AVER_CRITICAL(!amsseg->ambiguousFixes);
          AMS_GREY_BLACKEN(seg, i);
          if (i+1 < j)
            AMS_RANGE_WHITE_BLACKEN(seg, i+1, j);
        }
      }
    } while(amsseg->marksChanged);
    *totalReturn = FALSE;
  }

  return ResOK;
}


/* amsSegFix -- the segment fixing method */

static Res amsSegFix(Seg seg, ScanState ss, Ref *refIO)
{
  AMSSeg amsseg = MustBeA_CRITICAL(AMSSeg, seg);
  Pool pool = SegPool(seg);
  Index i;                      /* the index of the fixed grain */
  Addr base;
  Ref clientRef;
  Format format;

  AVERT_CRITICAL(ScanState, ss);
  AVER_CRITICAL(refIO != NULL);

  format = pool->format;
  AVERT_CRITICAL(Format, format);

  amsseg = Seg2AMSSeg(seg);
  AVERT_CRITICAL(AMSSeg, amsseg);
  /* It's a white seg, so it must have colour tables. */
  AVER_CRITICAL(amsseg->colourTablesInUse);

  /* @@@@ We should check that we're not in the grey mutator phase */
  /* <design/poolams#.not-req.grey>, but there's no way of */
  /* doing that here (this can be called from RootScan, during flip). */

  clientRef = *refIO;
  AVER_CRITICAL(SegBase(seg) <= clientRef);
  AVER_CRITICAL(clientRef < SegLimit(seg)); /* see .ref-limit */
  base = AddrSub((Addr)clientRef, format->headerSize);

  /* Not a real reference if out of bounds. This can happen if an
     ambiguous reference is closer to the base of the segment than the
     header size. */
  if (base < SegBase(seg)) {
    AVER(ss->rank == RankAMBIG);
    return ResOK;
  }

  /* Not a real reference if unaligned. */
  if (!AddrIsAligned(base, PoolAlignment(pool))) {
    AVER(ss->rank == RankAMBIG);
    return ResOK;
  }

  i = PoolIndexOfAddr(SegBase(seg), pool, base);
  AVER_CRITICAL(i < amsseg->grains);
  AVER_CRITICAL(!AMS_IS_INVALID_COLOUR(seg, i));

  /* Not a real reference if unallocated. */
  if (!AMS_ALLOCED(seg, i)) {
    AVER(ss->rank == RankAMBIG);
    return ResOK;
  }

  switch (ss->rank) {
  case RankAMBIG:
    if (PoolAMS(pool)->shareAllocTable)
      /* In this state, the pool doesn't support ambiguous references (see */
      /* .ambiguous.noshare), so this is not a reference. */
      break;
    amsseg->ambiguousFixes = TRUE;
    /* falls through */
  case RankEXACT:
  case RankFINAL:
  case RankWEAK:
    if (AMS_IS_WHITE(seg, i)) {
      ss->wasMarked = FALSE; /* <design/fix#.was-marked.not> */
      if (ss->rank == RankWEAK) { /* then splat the reference */
        *refIO = (Ref)0;
      } else {
        STATISTIC(++ss->preservedInPlaceCount); /* Size updated on reclaim */
        if (SegRankSet(seg) == RankSetEMPTY && ss->rank != RankAMBIG) {
          /* <design/poolams#.fix.to-black> */
          Addr clientNext, next;

          ShieldExpose(PoolArena(pool), seg);
          clientNext = (*pool->format->skip)(clientRef);
          ShieldCover(PoolArena(pool), seg);
          next = AddrSub(clientNext, format->headerSize);
          /* Part of the object might be grey, because of ambiguous */
          /* fixes, but that's OK, because scan will ignore that. */
          AMS_RANGE_WHITE_BLACKEN(seg, i, PoolIndexOfAddr(SegBase(seg), pool, next));
        } else { /* turn it grey */
          AMS_WHITE_GREYEN(seg, i);
          SegSetGrey(seg, TraceSetUnion(SegGrey(seg), ss->traces));
          /* mark it for scanning - <design/poolams#.marked.fix> */
          amsseg->marksChanged = TRUE;
        }
      }
    }
    break;
  default:
    NOTREACHED;
  }

  return ResOK;
}


/* amsSegBlacken -- the segment blackening method
 *
 * Turn all grey objects black.  */

static Res amsSegBlackenObject(Seg seg, Index i, Addr p, Addr next, void *clos)
{
  UNUSED(p);
  AVER(clos == UNUSED_POINTER);
  /* Do what amsScanObject does, minus the scanning. */
  if (AMS_IS_GREY(seg, i)) {
    Index j = PoolIndexOfAddr(SegBase(seg), SegPool(seg), next);
    AVER(!AMS_IS_INVALID_COLOUR(seg, i));
    AMS_GREY_BLACKEN(seg, i);
    if (i+1 < j)
      AMS_RANGE_BLACKEN(seg, i+1, j);
  }
  return ResOK;
}

static void amsSegBlacken(Seg seg, TraceSet traceSet)
{
  Res res;

  AVERT(TraceSet, traceSet);
  AVERT(Seg, seg);

  /* If it's white for any of these traces, turn grey to black without scanning. */
  if (TraceSetInter(traceSet, SegWhite(seg)) != TraceSetEMPTY) {
    AMSSeg amsseg = Seg2AMSSeg(seg);
    AVERT(AMSSeg, amsseg);
    AVER(amsseg->marksChanged); /* there must be something grey */
    amsseg->marksChanged = FALSE;
    res = semSegIterate(seg, amsSegBlackenObject, UNUSED_POINTER);
    AVER(res == ResOK);
  }
}


/* amsSegReclaim -- the segment reclamation method */

static void amsSegReclaim(Seg seg, Trace trace)
{
  AMSSeg amsseg = MustBeA(AMSSeg, seg);
  Pool pool = SegPool(seg);
  PoolGen pgen = PoolSegPoolGen(pool, seg);
  Count nowFree, grains, reclaimedGrains;
  Size preservedInPlaceSize;
  PoolDebugMixin debug;

  AVERT(Trace, trace);

  /* It's a white seg, so it must have colour tables. */
  AVER(amsseg->colourTablesInUse);
  AVER(!amsseg->marksChanged); /* there must be nothing grey */
  grains = amsseg->grains;

  /* Loop over all white blocks and splat them, if it's a debug class. */
  debug = Method(Pool, pool, debugMixin)(pool);
  if (debug != NULL) {
    Index i, j = 0;

    while(j < grains && AMS_FIND_WHITE_RANGE(&i, &j, seg, j, grains)) {
      AVER(!AMS_IS_INVALID_COLOUR(seg, i));
      DebugPoolFreeSplat(pool, PoolAddrOfIndex(SegBase(seg), pool, i),
                         PoolAddrOfIndex(SegBase(seg), pool, j));
      ++j; /* we know next grain is not white */
    }
  }

  nowFree = BTCountResRange(amsseg->nonwhiteTable, 0, grains);

  /* If the free space is all after firstFree, keep on using firstFree. */
  /* It could have a more complicated condition, but not worth the trouble. */
  if (!amsseg->allocTableInUse && amsseg->firstFree + nowFree == grains) {
    AVER(amsseg->firstFree == grains
         || BTIsResRange(amsseg->nonwhiteTable,
                         amsseg->firstFree, grains));
  } else {
    if (amsseg->ams->shareAllocTable) {
      /* Stop using allocTable as the white table. */
      amsseg->allocTableInUse = TRUE;
    } else {
      AVER(amsseg->allocTableInUse);
      BTCopyRange(amsseg->nonwhiteTable, amsseg->allocTable, 0, grains);
    }
  }

  reclaimedGrains = nowFree - amsseg->freeGrains;
  AVER(amsseg->oldGrains >= reclaimedGrains);
  amsseg->oldGrains -= reclaimedGrains;
  amsseg->freeGrains += reclaimedGrains;
  PoolGenAccountForReclaim(pgen, PoolGrainsSize(pool, reclaimedGrains), FALSE);
  STATISTIC(trace->reclaimSize += PoolGrainsSize(pool, reclaimedGrains));
  /* preservedInPlaceCount is updated on fix */
  preservedInPlaceSize = PoolGrainsSize(pool, amsseg->oldGrains);
  GenDescSurvived(pgen->gen, trace, 0, preservedInPlaceSize);

  /* Ensure consistency of segment even if are just about to free it */
  amsseg->colourTablesInUse = FALSE;
  SegSetWhite(seg, TraceSetDel(SegWhite(seg), trace));

  if (amsseg->freeGrains == grains && !SegHasBuffer(seg)) {
    /* No survivors */
    AVER(amsseg->bufferedGrains == 0);
    PoolGenFree(pgen, seg,
                PoolGrainsSize(pool, amsseg->freeGrains),
                PoolGrainsSize(pool, amsseg->oldGrains),
                PoolGrainsSize(pool, amsseg->newGrains),
                FALSE);
  }
}


/* amsSegWalk -- walk formatted objects in AMC segment */

static void amsSegWalk(Seg seg, Format format, FormattedObjectsVisitor f,
                       void *p, size_t s)
{
  AMSSeg amsseg = MustBeA(AMSSeg, seg);
  Pool pool = SegPool(seg);
  Addr object, base, limit;

  AVERT(Format, format);
  AVER(FUNCHECK(f));
  /* p and s are arbitrary closures and can't be checked */

  base = SegBase(seg);
  object = base;
  limit = SegLimit(seg);

  while (object < limit) {
    /* object is a slight misnomer because it might point to a free grain */
    Addr next;
    Index i;
    Buffer buffer;

    if (SegBuffer(&buffer, seg)) {
      if (object == BufferScanLimit(buffer)
          && BufferScanLimit(buffer) != BufferLimit(buffer)) {
        /* skip over buffered area */
        object = BufferLimit(buffer);
        continue;
      }
      /* since we skip over the buffered area we are always */
      /* either before the buffer, or after it, never in it */
      AVER(object < BufferGetInit(buffer) || BufferLimit(buffer) <= object);
    }
    i = PoolIndexOfAddr(SegBase(seg), pool, object);
    if (!AMS_ALLOCED(seg, i)) {
      /* This grain is free */
      object = AddrAdd(object, PoolAlignment(pool));
      continue;
    }
    object = AddrAdd(object, format->headerSize);
    next = format->skip(object);
    next = AddrSub(next, format->headerSize);
    AVER(AddrIsAligned(next, PoolAlignment(pool)));
    if (!amsseg->colourTablesInUse || !AMS_IS_WHITE(seg, i))
      (*f)(object, pool->format, pool, p, s);
    object = next;
  }
}


/* AMSFreeWalk -- free block walking method of the pool class */

static void AMSFreeWalk(Pool pool, FreeBlockVisitor f, void *p)
{
  AMS ams;
  Ring node, ring, nextNode;    /* for iterating over the segments */

  AVERT(Pool, pool);
  ams = PoolAMS(pool);
  AVERT(AMS, ams);

  ring = PoolSegRing(AMSPool(ams));
  RING_FOR(node, ring, nextNode) {
    AMSSegFreeWalk(Seg2AMSSeg(SegOfPoolRing(node)), f, p);
  }
}


/* AMSTotalSize -- total memory allocated from the arena */

static Size AMSTotalSize(Pool pool)
{
  AMS ams;

  AVERT(Pool, pool);
  ams = PoolAMS(pool);
  AVERT(AMS, ams);

  return ams->pgen->totalSize;
}


/* AMSFreeSize -- free memory (unused by client program) */

static Size AMSFreeSize(Pool pool)
{
  AMS ams;

  AVERT(Pool, pool);
  ams = PoolAMS(pool);
  AVERT(AMS, ams);

  return ams->pgen->freeSize;
}


/* AMSDescribe -- the pool class description method
 *
 * Iterates over the segments, describing all of them.
 */

static Res AMSDescribe(Inst inst, mps_lib_FILE *stream, Count depth)
{
  Pool pool = CouldBeA(AbstractPool, inst);
  AMS ams = CouldBeA(AMSPool, pool);
  Ring ring, node, nextNode;
  Res res;

  if (!TESTC(AMSPool, ams))
    return ResPARAM;
  if (stream == NULL)
    return ResPARAM;

  res = NextMethod(Inst, AMSPool, describe)(inst, stream, depth);
  if (res != ResOK)
    return res;

  res = WriteF(stream, depth + 2,
               "segments: * black  + grey  - white  . alloc  ! bad\n"
               "buffers: [ base  < scan limit  | init  > alloc  ] limit\n",
               NULL);
  if (res != ResOK)
    return res;

  ring = PoolSegRing(AMSPool(ams));
  RING_FOR(node, ring, nextNode) {
    res = SegDescribe(SegOfPoolRing(node), stream, depth + 2);
    if (res != ResOK)
      return res;
  }

  return ResOK;
}


/* AMSPoolClass -- the class definition */

/* <code/poolams.h> contains the type definition.  Hence the use */
/* of DEFINE_CLASS rather than DEFINE_POOL_CLASS */

DEFINE_CLASS(Pool, AMSPool, klass)
{
  INHERIT_CLASS(klass, AMSPool, AbstractCollectPool);
  klass->instClassStruct.describe = AMSDescribe;
  klass->instClassStruct.finish = AMSFinish;
  klass->size = sizeof(AMSStruct);
  klass->varargs = AMSVarargs;
  klass->init = AMSInit;
  klass->bufferClass = RankBufClassGet;
  klass->bufferFill = AMSBufferFill;
  klass->segPoolGen = amsSegPoolGen;
  klass->freewalk = AMSFreeWalk;
  klass->totalSize = AMSTotalSize;
  klass->freeSize = AMSFreeSize;
  AVERT(PoolClass, klass);
}


/* AMSDebugMixin - find debug mixin in class AMSDebug */

static PoolDebugMixin AMSDebugMixin(Pool pool)
{
  AMS ams;

  AVERT(Pool, pool);
  ams = PoolAMS(pool);
  AVERT(AMS, ams);
  /* Can't check AMSDebug, because this is called during init */
  return &(AMS2AMSDebug(ams)->debug);
}


/* AMSDebugPoolClass -- the class definition for the debug version */

DEFINE_CLASS(Pool, AMSDebugPool, klass)
{
  INHERIT_CLASS(klass, AMSDebugPool, AMSPool);
  PoolClassMixInDebug(klass);
  klass->size = sizeof(AMSDebugStruct);
  klass->varargs = AMSDebugVarargs;
  klass->debugMixin = AMSDebugMixin;
  AVERT(PoolClass, klass);
}


/* mps_class_ams -- return the AMS pool class descriptor */

mps_pool_class_t mps_class_ams(void)
{
  return (mps_pool_class_t)CLASS(AMSPool);
}


/* mps_class_ams_debug -- return the AMS (debug) pool class descriptor */

mps_pool_class_t mps_class_ams_debug(void)
{
  return (mps_pool_class_t)CLASS(AMSDebugPool);
}


/* AMSCheck -- the check method for an AMS */

Bool AMSCheck(AMS ams)
{
  CHECKS(AMS, ams);
  CHECKC(AMSPool, ams);
  CHECKD(Pool, AMSPool(ams));
  CHECKL(IsA(AMSPool, ams));
  CHECKL(PoolAlignment(AMSPool(ams)) == AMSPool(ams)->format->alignment);
  if (ams->pgen != NULL) {
    CHECKL(ams->pgen == &ams->pgenStruct);
    CHECKD(PoolGen, ams->pgen);
  }
  CHECKL(FUNCHECK(ams->segSize));
  CHECKL(FUNCHECK(ams->segsDestroy));
  CHECKL(FUNCHECK(ams->segClass));

  return TRUE;
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
