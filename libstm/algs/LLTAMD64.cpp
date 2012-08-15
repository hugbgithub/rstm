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
 * Like LLT, but we use the tick counter instead of a timestamp
 */

/**
 *  LLTAMD64 Implementation
 *
 *    This STM very closely resembles the GV1 variant of TL2.  That is, it
 *    uses orecs and lazy acquire.  Its clock requires everyone to increment
 *    it to commit writes, but this allows for read-set validation to be
 *    skipped at commit time.  Most importantly, there is no in-flight
 *    validation: if a timestamp is greater than when the transaction sampled
 *    the clock at begin time, the transaction aborts.
 */

#include "algs.hpp"

namespace stm
{
  TM_FASTCALL void* LLTAMD64ReadRO(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* LLTAMD64ReadRW(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void LLTAMD64WriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void LLTAMD64WriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void LLTAMD64CommitRO(TX_LONE_PARAMETER);
  TM_FASTCALL void LLTAMD64CommitRW(TX_LONE_PARAMETER);
  NOINLINE void LLTAMD64Validate(TxThread*);

  /**
   *  LLTAMD64 begin:
   */
  void LLTAMD64Begin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();
      // get a start time
      tx->start_time = tickp();
  }

  /**
   *  LLTAMD64 commit (read-only):
   */
  void
  LLTAMD64CommitRO(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // read-only, so just reset lists
      tx->r_orecs.reset();
      OnROCommit(tx);
  }

  /**
   *  LLTAMD64 commit (writing context):
   *
   *    Get all locks, validate, do writeback.  Use the counter to avoid some
   *    validations.
   */
  void
  LLTAMD64CommitRW(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // acquire locks
      foreach (WriteSet, i, tx->writes) {
          // get orec, read its version#
          orec_t* o = get_orec(i->addr);
          uintptr_t ivt = o->v.all;

          // lock all orecs, unless already locked
          if (ivt <= tx->start_time) {
              // abort if cannot acquire
              if (!bcasptr(&o->v.all, ivt, tx->my_lock.all))
                  tmabort();
              // save old version to o->p, remember that we hold the lock
              o->p = ivt;
              tx->locks.insert(o);
          }
          // else if we don't hold the lock abort
          else if (ivt != tx->my_lock.all) {
              tmabort();
          }
      }

      // increment the global timestamp since we have writes
      uintptr_t end_time = tickp();

      // validate
      LLTAMD64Validate(tx);

      // run the redo log
      tx->writes.writeback();

      // release locks
      CFENCE;
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = end_time;

      // clean-up
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();
      OnRWCommit(tx);
      ResetToRO(tx, LLTAMD64ReadRO, LLTAMD64WriteRO, LLTAMD64CommitRO);
  }

  /**
   *  LLTAMD64 read (read-only transaction)
   *
   *    We use "check twice" timestamps in LLTAMD64
   */
  void*
  LLTAMD64ReadRO(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      // get the orec addr
      orec_t* o = get_orec(addr);

      // read orec, then val, then orec
      uintptr_t ivt = o->v.all;
      CFENCE;
      void* tmp = *addr;
      CFENCE;
      uintptr_t ivt2 = o->v.all;
      // if orec never changed, and isn't too new, the read is valid
      if ((ivt <= tx->start_time) && (ivt == ivt2)) {
          // log orec, return the value
          tx->r_orecs.insert(o);
          return tmp;
      }
      // unreachable
      tmabort();
      return NULL;
  }

  /**
   *  LLTAMD64 read (writing transaction)
   */
  void*
  LLTAMD64ReadRW(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // get the orec addr
      orec_t* o = get_orec(addr);

      // read orec, then val, then orec
      uintptr_t ivt = o->v.all;
      CFENCE;
      void* tmp = *addr;
      CFENCE;
      uintptr_t ivt2 = o->v.all;

      // fixup is here to minimize the postvalidation orec read latency
      REDO_RAW_CLEANUP(tmp, found, log, mask);
      // if orec never changed, and isn't too new, the read is valid
      if ((ivt <= tx->start_time) && (ivt == ivt2)) {
          // log orec, return the value
          tx->r_orecs.insert(o);
          return tmp;
      }
      tmabort();
      // unreachable
      return NULL;
  }

  /**
   *  LLTAMD64 write (read-only context)
   */
  void
  LLTAMD64WriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // add to redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, LLTAMD64ReadRW, LLTAMD64WriteRW, LLTAMD64CommitRW);
  }

  /**
   *  LLTAMD64 write (writing context)
   */
  void
  LLTAMD64WriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // add to redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  LLTAMD64 unwinder:
   */
  void
  LLTAMD64Rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // release the locks and restore version numbers
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = (*i)->p;

      // undo memory operations, reset lists
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();
      PostRollback(tx);
      ResetToRO(tx, LLTAMD64ReadRO, LLTAMD64WriteRO, LLTAMD64CommitRO);
  }

  /**
   *  LLTAMD64 in-flight irrevocability:
   */
  bool
  LLTAMD64Irrevoc(TxThread*)
  {
      return false;
  }

  /**
   *  LLTAMD64 validation
   */
  void LLTAMD64Validate(TxThread* tx)
  {
      // validate
      foreach (OrecList, i, tx->r_orecs) {
          uintptr_t ivt = (*i)->v.all;
          // if unlocked and newer than start time, abort
          if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
              tmabort();
      }
  }

  /**
   *  Switch to LLTAMD64:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   */
  void
  LLTAMD64OnSwitchTo()
  {
      // timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
  }
}

DECLARE_SIMPLE_METHODS_FROM_NORMAL(LLTAMD64)
REGISTER_FGADAPT_ALG(LLTAMD64, "LLTAMD64", false)

#ifdef STM_ONESHOT_ALG_LLTAMD64
DECLARE_AS_ONESHOT_NORMAL(LLTAMD64)
#endif
