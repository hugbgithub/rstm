/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

/**
 *  PipelineTurbo Implementation
 *
 *    This algorithm is inspired by FastPath [LCPC 2009], and by Oancea et
 *    al. SPAA 2009.  We induce a total order on transactions at start time,
 *    via a global counter, and then we require them to commit in this order.
 *    For concurrency control, we use an orec table, but atomics are not
 *    needed, since the counter also serves as a commit token.
 *
 *    In addition, the lead thread uses in-place writes, via a special
 *    version of the read and write functions.  However, the lead thread
 *    can't self-abort.
 */

#include "algs.hpp"
#include "../Diagnostics.hpp"

namespace stm
{
  TM_FASTCALL void* PipelineTurboReadRO(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* PipelineTurboReadRW(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* PipelineTurboReadTurbo(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void PipelineTurboWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void PipelineTurboWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void PipelineTurboWriteTurbo(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void PipelineTurboCommitRO(TX_LONE_PARAMETER);
  TM_FASTCALL void PipelineTurboCommitRW(TX_LONE_PARAMETER);
  TM_FASTCALL void PipelineTurboCommitTurbo(TX_LONE_PARAMETER);
  NOINLINE void PipelineTurboValidate(TxThread*, uintptr_t finish_cache);

  /**
   *  PipelineTurbo begin:
   *
   *    PipelineTurbo is very fair: on abort, we keep our old order.  Thus only if we
   *    are starting a new transaction do we get an order.  We always check if we
   *    are oldest, in which case we can move straight to turbo mode.
   *
   *    ts_cache is important: when this tx starts, it knows its commit time.
   *    However, earlier txns have not yet committed.  The difference between
   *    ts_cache and order tells how many transactions need to commit.  Whenever
   *    one does, this tx will need to validate.
   */
  void PipelineTurboBegin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();

      // only get a new start time if we didn't just abort
      if (tx->order == -1)
          tx->order = 1 + faiptr(&timestamp.val);

      tx->ts_cache = last_complete.val;
      if (tx->ts_cache == ((uintptr_t)tx->order - 1))
          GoTurbo(tx, PipelineTurboReadTurbo, PipelineTurboWriteTurbo, PipelineTurboCommitTurbo);
  }

  /**
   *  PipelineTurbo commit (read-only):
   *
   *    For the sake of ordering, read-only transactions must wait until they
   *    are the oldest, then they validate.  This introduces a lot of
   *    overhead, but it gives SGLA (in the [Menon SPAA 2008] sense)
   *    semantics.
   */
  void
  PipelineTurboCommitRO(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // wait our turn, then validate
      while (last_complete.val != ((uintptr_t)tx->order - 1)) {
          // in this wait loop, we need to check if an adaptivity action is
          // underway :(
          if (tmbegin != PipelineTurboBegin)
              tmabort();
      }
      foreach (OrecList, i, tx->r_orecs) {
          // read this orec
          uintptr_t ivt = (*i)->v.all;
          // if it has a timestamp of ts_cache or greater, abort
          if (ivt > tx->ts_cache)
              tmabort();
      }
      // mark self as complete
      last_complete.val = tx->order;

      // set status to committed...
      tx->order = -1;

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      OnROCommit(tx);
  }

  /**
   *  PipelineTurbo commit (writing context):
   *
   *    Given the total order, RW commit is just like RO commit, except that we
   *    need to acquire locks and do writeback, too.  One nice thing is that
   *    acquisition is with naked stores, and it is on a path that always
   *    commits.
   */
  void
  PipelineTurboCommitRW(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // wait our turn, validate, writeback
      while (last_complete.val != ((uintptr_t)tx->order - 1)) {
          if (tmbegin != PipelineTurboBegin)
              tmabort();
      }
      foreach (OrecList, i, tx->r_orecs) {
          // read this orec
          uintptr_t ivt = (*i)->v.all;
          // if it has a timestamp of ts_cache or greater, abort
          if (ivt > tx->ts_cache)
              tmabort();
      }
      // mark every location in the write set, and perform write-back
      // NB: we cannot abort anymore
      foreach (WriteSet, i, tx->writes) {
          // get orec
          orec_t* o = get_orec(i->addr);
          // mark orec
          o->v.all = tx->order;
          CFENCE; // WBW
          // write-back
          *i->addr = i->val;
      }
      last_complete.val = tx->order;

      // set status to committed...
      tx->order = -1;

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnRWCommit(tx);
      ResetToRO(tx, PipelineTurboReadRO, PipelineTurboWriteRO, PipelineTurboCommitRO);
  }

  /**
   *  PipelineTurbo commit (turbo mode):
   *
   *    The current transaction is oldest, used in-place writes, and eagerly
   *    acquired all locks.  There is nothing to do but mark self as done.
   *
   *    NB: we do not distinguish between RO and RW... we should, and could
   *        via tx->writes
   */
  void
  PipelineTurboCommitTurbo(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      CFENCE;
      last_complete.val = tx->order;

      // set status to committed...
      tx->order = -1;

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnRWCommit(tx);
      ResetToRO(tx, PipelineTurboReadRO, PipelineTurboWriteRO, PipelineTurboCommitRO);
  }

  /**
   *  PipelineTurbo read (read-only transaction)
   *
   *    Since the commit time is determined before final validation (because the
   *    commit time is determined at begin time!), we can skip pre-validation.
   *    Otherwise, this is a standard orec read function.
   */
  void*
  PipelineTurboReadRO(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      void* tmp = *addr;
      CFENCE; // RBR between dereference and orec check

      // get the orec addr, read the orec's version#
      orec_t* o = get_orec(addr);
      uintptr_t ivt = o->v.all;
      // abort if this changed since the last time I saw someone finish
      if (ivt > tx->ts_cache)
          tmabort();
      // log orec
      tx->r_orecs.insert(o);
      // validate if necessary
      if (last_complete.val > tx->ts_cache)
          PipelineTurboValidate(tx, last_complete.val);
      return tmp;
  }

  /**
   *  PipelineTurbo read (writing transaction)
   */
  void*
  PipelineTurboReadRW(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      void* tmp = *addr;
      CFENCE; // RBR between dereference and orec check

      // get the orec addr, read the orec's version#
      orec_t* o = get_orec(addr);
      uintptr_t ivt = o->v.all;
      // abort if this changed since the last time I saw someone finish
      if (ivt > tx->ts_cache)
          tmabort();
      // log orec
      tx->r_orecs.insert(o);
      // validate if necessary
      if (last_complete.val > tx->ts_cache)
          PipelineTurboValidate(tx, last_complete.val);

      REDO_RAW_CLEANUP(tmp, found, log, mask)
      return tmp;
  }

  /**
   *  PipelineTurbo read (turbo mode)
   */
  void*
  PipelineTurboReadTurbo(TX_FIRST_PARAMETER_ANON STM_READ_SIG(addr,))
  {
      return *addr;
  }

  /**
   *  PipelineTurbo write (read-only context)
   */
  void
  PipelineTurboWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, PipelineTurboReadRW, PipelineTurboWriteRW, PipelineTurboCommitRW);
  }

  /**
   *  PipelineTurbo write (writing context)
   */
  void
  PipelineTurboWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  PipelineTurbo write (turbo mode)
   *
   *    The oldest transaction needs to mark the orec before writing in-place.
   */
  void
  PipelineTurboWriteTurbo(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      orec_t* o = get_orec(addr);
      o->v.all = tx->order;
      CFENCE;
      STM_DO_MASKED_WRITE(addr, val, mask);
  }

  /**
   *  PipelineTurbo unwinder:
   *
   *    For now, unwinding always happens before locks are held, and can't
   *    happen in turbo mode.
   *
   *    NB: Self-abort is not supported in PipelineTurbo.  Adding undo logging to
   *        turbo mode would resolve the issue.
   */
  void
  PipelineTurboRollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);
      // we cannot be in fast mode
      if (CheckTurboMode(tx, PipelineTurboReadTurbo))
          UNRECOVERABLE("Attempting to abort a turbo-mode transaction!");

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      tx->r_orecs.reset();
      tx->writes.reset();
      // NB: at one time, this implementation could not reset pointers on
      //     abort.  This situation may remain, but it is not certain that it
      //     has not been resolved.
      PostRollback(tx);
  }

  /**
   *  PipelineTurbo in-flight irrevocability:
   */
  bool PipelineTurboIrrevoc(TxThread*)
  {
      UNRECOVERABLE("PipelineTurbo Irrevocability not yet supported");
      return false;
  }

  /**
   *  PipelineTurbo validation
   *
   *    Make sure all orec version#s are valid.  Then see about switching to
   *    turbo mode.  Note that to do the switch, the current write set must be
   *    written to memory.
   */
  void
  PipelineTurboValidate(TxThread* tx, uintptr_t finish_cache)
  {
      foreach (OrecList, i, tx->r_orecs) {
          // read this orec
          uintptr_t ivt = (*i)->v.all;
          // if it has a timestamp of ts_cache or greater, abort
          if (ivt > tx->ts_cache)
              tmabort();
      }
      // now update the finish_cache to remember that at this time, we were
      // still valid
      tx->ts_cache = finish_cache;
      // and if we are now the oldest thread, transition to fast mode
      if (tx->ts_cache == ((uintptr_t)tx->order - 1)) {
          if (tx->writes.size() != 0) {
              // mark every location in the write set, and perform write-back
              foreach (WriteSet, i, tx->writes) {
                  // get orec
                  orec_t* o = get_orec(i->addr);
                  // mark orec
                  o->v.all = tx->order;
                  CFENCE; // WBW
                  // write-back
                  *i->addr = i->val;
              }
              GoTurbo(tx, PipelineTurboReadTurbo, PipelineTurboWriteTurbo, PipelineTurboCommitTurbo);
          }
      }
  }

  /**
   *  Switch to PipelineTurbo:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   *
   *    Also, last_complete must equal timestamp
   *
   *    Also, all threads' order values must be -1
   */
  void
  PipelineTurboOnSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
      last_complete.val = timestamp.val;
      for (uint32_t i = 0; i < threadcount.val; ++i)
          threads[i]->order = -1;
  }
}

DECLARE_SIMPLE_METHODS_FROM_TURBO(PipelineTurbo)
REGISTER_FGADAPT_ALG(PipelineTurbo, "PipelineTurbo", true)

#ifdef STM_ONESHOT_ALG_PipelineTurbo
DECLARE_AS_ONESHOT_TURBO(PipelineTurbo)
#endif
