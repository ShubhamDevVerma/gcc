/* Scanning of rtl for dataflow analysis.
   Copyright (C) 2007-2014 Free Software Foundation, Inc.
   Contributed by Kenneth Zadeck (zadeck@naturalbridge.com).

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */


#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "tm_p.h"
#include "flags.h"
#include "regs.h"
#include "except.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "timevar.h"
#include "df.h"


struct regstat_n_sets_and_refs_t *regstat_n_sets_and_refs;

/*----------------------------------------------------------------------------
   REG_N_SETS and REG_N_REFS.
   ----------------------------------------------------------------------------*/

/* If a pass need to change these values in some magical way or the
   pass needs to have accurate values for these and is not using
   incremental df scanning, then it should use REG_N_SETS and
   REG_N_USES.  If the pass is doing incremental scanning then it
   should be getting the info from DF_REG_DEF_COUNT and
   DF_REG_USE_COUNT.  */

void
regstat_init_n_sets_and_refs (void)
{
  unsigned int i;
  unsigned int max_regno = max_reg_num ();

  timevar_push (TV_REG_STATS);
  df_grow_reg_info ();
  gcc_assert (!regstat_n_sets_and_refs);

  regstat_n_sets_and_refs = XNEWVEC (struct regstat_n_sets_and_refs_t, max_regno);

  if (MAY_HAVE_DEBUG_INSNS)
    for (i = 0; i < max_regno; i++)
      {
	int use_count;
	df_ref use;

	use_count = DF_REG_USE_COUNT (i);
	for (use = DF_REG_USE_CHAIN (i); use; use = DF_REF_NEXT_REG (use))
	  if (DF_REF_INSN_INFO (use) && DEBUG_INSN_P (DF_REF_INSN (use)))
	    use_count--;


	SET_REG_N_SETS (i, DF_REG_DEF_COUNT (i));
	SET_REG_N_REFS (i, use_count + REG_N_SETS (i));
      }
  else
    for (i = 0; i < max_regno; i++)
      {
	SET_REG_N_SETS (i, DF_REG_DEF_COUNT (i));
	SET_REG_N_REFS (i, DF_REG_USE_COUNT (i) + REG_N_SETS (i));
      }
  timevar_pop (TV_REG_STATS);

}


/* Free the array that holds the REG_N_SETS and REG_N_REFS.  */

void
regstat_free_n_sets_and_refs (void)
{
  gcc_assert (regstat_n_sets_and_refs);
  free (regstat_n_sets_and_refs);
  regstat_n_sets_and_refs = NULL;
}


/*----------------------------------------------------------------------------
   REGISTER INFORMATION

   Process REG_N_DEATHS, REG_LIVE_LENGTH, REG_N_CALLS_CROSSED,
   REG_N_THROWING_CALLS_CROSSED and REG_BASIC_BLOCK.

   ----------------------------------------------------------------------------*/

static bitmap setjmp_crosses;
struct reg_info_t *reg_info_p;

/* The number allocated elements of reg_info_p.  */
size_t reg_info_p_size;

/* Compute register info: lifetime, bb, and number of defs and uses
   for basic block BB.  The three bitvectors are scratch regs used
   here.  */

static void
regstat_bb_compute_ri (unsigned int bb_index,
		       bitmap live, bitmap artificial_uses,
		       bitmap local_live, bitmap local_processed,
		       int *local_live_last_luid)
{
  basic_block bb = BASIC_BLOCK_FOR_FN (cfun, bb_index);
  rtx insn;
  df_ref def, use;
  int luid = 0;
  bitmap_iterator bi;
  unsigned int regno;

  bitmap_copy (live, df_get_live_out (bb));
  bitmap_clear (artificial_uses);

  /* Process the regs live at the end of the block.  Mark them as
     not local to any one basic block.  */
  EXECUTE_IF_SET_IN_BITMAP (live, 0, regno, bi)
    REG_BASIC_BLOCK (regno) = REG_BLOCK_GLOBAL;

  /* Process the artificial defs and uses at the bottom of the block
     to begin processing.  */
  FOR_EACH_ARTIFICIAL_DEF (def, bb_index)
    if ((DF_REF_FLAGS (def) & DF_REF_AT_TOP) == 0)
      bitmap_clear_bit (live, DF_REF_REGNO (def));

  FOR_EACH_ARTIFICIAL_USE (use, bb_index)
    if ((DF_REF_FLAGS (use) & DF_REF_AT_TOP) == 0)
      {
	regno = DF_REF_REGNO (use);
	bitmap_set_bit (live, regno);
	bitmap_set_bit (artificial_uses, regno);
      }

  FOR_BB_INSNS_REVERSE (bb, insn)
    {
      struct df_insn_info *insn_info = DF_INSN_INFO_GET (insn);
      bitmap_iterator bi;
      df_mw_hardreg *mw;
      rtx link;

      if (!NONDEBUG_INSN_P (insn))
	continue;

      luid++;

      link = REG_NOTES (insn);
      while (link)
	{
	  if (REG_NOTE_KIND (link) == REG_DEAD)
	    REG_N_DEATHS (REGNO (XEXP (link, 0)))++;
	  link = XEXP (link, 1);
	}

      /* Process the defs.  */
      if (CALL_P (insn))
	{
	  bool can_throw = can_throw_internal (insn);
	  bool set_jump = (find_reg_note (insn, REG_SETJMP, NULL) != NULL);
	  EXECUTE_IF_SET_IN_BITMAP (live, 0, regno, bi)
	    {
	      REG_N_CALLS_CROSSED (regno)++;
	      REG_FREQ_CALLS_CROSSED (regno) += REG_FREQ_FROM_BB (bb);
	      REG_FREQ_CALLS_CROSSED (regno) =
		MIN (REG_FREQ_CALLS_CROSSED (regno), REG_FREQ_MAX);
	      if (can_throw)
		REG_N_THROWING_CALLS_CROSSED (regno)++;

	      /* We have a problem with any pseudoreg that lives
		 across the setjmp.  ANSI says that if a user variable
		 does not change in value between the setjmp and the
		 longjmp, then the longjmp preserves it.  This
		 includes longjmp from a place where the pseudo
		 appears dead.  (In principle, the value still exists
		 if it is in scope.)  If the pseudo goes in a hard
		 reg, some other value may occupy that hard reg where
		 this pseudo is dead, thus clobbering the pseudo.
		 Conclusion: such a pseudo must not go in a hard
		 reg.  */
	      if (set_jump)
		bitmap_set_bit (setjmp_crosses, regno);
	    }
	}

      /* We only care about real sets for calls.  Clobbers cannot
	 be depended on.
	 Only do this if the value is totally dead.  */
      FOR_EACH_INSN_INFO_MW (mw, insn_info)
	if (DF_MWS_REG_DEF_P (mw))
	  {
	    bool all_dead = true;
	    unsigned int r;

	    for (r = mw->start_regno; r <= mw->end_regno; r++)
	      if (bitmap_bit_p (artificial_uses, r)
		  || bitmap_bit_p (live, r))
		{
		  all_dead = false;
		  break;
		}

	    if (all_dead)
	      {
		regno = mw->start_regno;
		REG_LIVE_LENGTH (regno)++;
	      }
	  }

      /* All of the defs except the return value are some sort of
	 clobber.  This code is for the return.  */
      FOR_EACH_INSN_INFO_DEF (def, insn_info)
	{
	  if ((!CALL_P (insn))
	      || (!(DF_REF_FLAGS (def) & (DF_REF_MUST_CLOBBER | DF_REF_MAY_CLOBBER))))
	    {
	      unsigned int dregno = DF_REF_REGNO (def);

	      if (bitmap_bit_p (live, dregno))
		{
		  /* If we have seen a use of DREGNO somewhere before (i.e.
		     later in this basic block), and DEF is not a subreg
		     store or conditional store, then kill the register
		     here and add the proper length to its REG_LIVE_LENGTH.

		     If we have not seen a use of DREGNO later in this basic
		     block, then we need to add the length from here to the
		     end of the block to the live length.  */
		  if (bitmap_bit_p (local_live, dregno))
		    {
		      /* Note that LOCAL_LIVE implies LOCAL_PROCESSED, so
			 we don't have to set LOCAL_PROCESSED in this clause.  */
		      if (!(DF_REF_FLAGS (def) & (DF_REF_PARTIAL | DF_REF_CONDITIONAL)))
			{
			  REG_LIVE_LENGTH (dregno) +=
			    (luid - local_live_last_luid[dregno]);
			  local_live_last_luid[dregno] = luid;
			  bitmap_clear_bit (local_live, dregno);
			}
		    }
		  else
		    {
		      bitmap_set_bit (local_processed, dregno);
		      REG_LIVE_LENGTH (dregno) += luid;
		      local_live_last_luid[dregno] = luid;
		    }

		  /* Kill this register if it is not a subreg store or
		     conditional store.
		     ??? This means that any partial store is live from
		     the last use in a basic block to the start of this
		     basic block.  This results in poor calculations of
		     REG_LIVE_LENGTH in large basic blocks.  */
		  if (!(DF_REF_FLAGS (def) & (DF_REF_PARTIAL | DF_REF_CONDITIONAL)))
		    bitmap_clear_bit (live, dregno);
		}
	      else if ((!(DF_REF_FLAGS (def) & DF_REF_MW_HARDREG))
		       && (!bitmap_bit_p (artificial_uses, dregno)))
		{
		  REG_LIVE_LENGTH (dregno)++;
		}

	      if (dregno >= FIRST_PSEUDO_REGISTER)
		{
		  REG_FREQ (dregno) += REG_FREQ_FROM_BB (bb);
		  REG_FREQ (dregno) =
		    MIN (REG_FREQ (dregno), REG_FREQ_MAX);

		  if (REG_BASIC_BLOCK (dregno) == REG_BLOCK_UNKNOWN)
		    REG_BASIC_BLOCK (dregno) = bb->index;
		  else if (REG_BASIC_BLOCK (dregno) != bb->index)
		    REG_BASIC_BLOCK (dregno) = REG_BLOCK_GLOBAL;
		}
	    }
	}

      FOR_EACH_INSN_INFO_USE (use, insn_info)
	{
	  unsigned int uregno = DF_REF_REGNO (use);

	  if (uregno >= FIRST_PSEUDO_REGISTER)
	    {
	      REG_FREQ (uregno) += REG_FREQ_FROM_BB (bb);
	      REG_FREQ (uregno) =
		MIN (REG_FREQ (uregno), REG_FREQ_MAX);

	      if (REG_BASIC_BLOCK (uregno) == REG_BLOCK_UNKNOWN)
		REG_BASIC_BLOCK (uregno) = bb->index;
	      else if (REG_BASIC_BLOCK (uregno) != bb->index)
		REG_BASIC_BLOCK (uregno) = REG_BLOCK_GLOBAL;
	    }

	  if (bitmap_set_bit (live, uregno))
	    {
	      /* This register is now live.  Begin to process it locally.

		 Note that we don't even get here if the variable was live
		 at the end of the block since just a ref inside the block
		 does not effect the calculations.  */
	      REG_LIVE_LENGTH (uregno) ++;
	      local_live_last_luid[uregno] = luid;
	      bitmap_set_bit (local_live, uregno);
	      bitmap_set_bit (local_processed, uregno);
	    }
	}
    }

  /* Add the liveness length to all registers that were used somewhere
     in this bock, but not between that use and the head of this block.  */
  EXECUTE_IF_SET_IN_BITMAP (local_live, 0, regno, bi)
    {
      REG_LIVE_LENGTH (regno) += (luid - local_live_last_luid[regno]);
    }

  /* Add the length of the block to all of the registers that were not
     referenced, but still live in this block.  */
  bitmap_and_compl_into (live, local_processed);
  EXECUTE_IF_SET_IN_BITMAP (live, 0, regno, bi)
    REG_LIVE_LENGTH (regno) += luid;

  bitmap_clear (local_processed);
  bitmap_clear (local_live);
}


/* Compute register info: lifetime, bb, and number of defs and uses.  */
void
regstat_compute_ri (void)
{
  basic_block bb;
  bitmap live = BITMAP_ALLOC (&df_bitmap_obstack);
  bitmap artificial_uses = BITMAP_ALLOC (&df_bitmap_obstack);
  bitmap local_live = BITMAP_ALLOC (&df_bitmap_obstack);
  bitmap local_processed = BITMAP_ALLOC (&df_bitmap_obstack);
  unsigned int regno;
  bitmap_iterator bi;
  int *local_live_last_luid;

  /* Initialize everything.  */

  gcc_assert (!reg_info_p);

  timevar_push (TV_REG_STATS);
  setjmp_crosses = BITMAP_ALLOC (&df_bitmap_obstack);
  max_regno = max_reg_num ();
  reg_info_p_size = max_regno;
  reg_info_p = XCNEWVEC (struct reg_info_t, max_regno);
  local_live_last_luid = XNEWVEC (int, max_regno);

  FOR_EACH_BB_FN (bb, cfun)
    {
      regstat_bb_compute_ri (bb->index, live, artificial_uses,
			     local_live, local_processed,
			     local_live_last_luid);
    }

  BITMAP_FREE (live);
  BITMAP_FREE (artificial_uses);
  BITMAP_FREE (local_live);
  BITMAP_FREE (local_processed);
  free (local_live_last_luid);

  /* See the setjmp comment in regstat_bb_compute_ri.  */
  EXECUTE_IF_SET_IN_BITMAP (setjmp_crosses, FIRST_PSEUDO_REGISTER, regno, bi)
    {
      REG_BASIC_BLOCK (regno) = REG_BLOCK_UNKNOWN;
      REG_LIVE_LENGTH (regno) = -1;
    }

  timevar_pop (TV_REG_STATS);
}


/* Free all storage associated with the problem.  */

void
regstat_free_ri (void)
{
  gcc_assert (reg_info_p);
  reg_info_p_size = 0;
  free (reg_info_p);
  reg_info_p = NULL;

  BITMAP_FREE (setjmp_crosses);
}


/* Return a bitmap containing the set of registers that cross a setjmp.
   The client should not change or delete this bitmap.  */

bitmap
regstat_get_setjmp_crosses (void)
{
  return setjmp_crosses;
}

/*----------------------------------------------------------------------------
   Process REG_N_CALLS_CROSSED.

   This is used by sched_deps.  A good implementation of sched-deps
   would really process the blocks directly rather than going through
   lists of insns.  If it did this, it could use the exact regs that
   cross an individual call rather than using this info that merges
   the info for all calls.

   ----------------------------------------------------------------------------*/



/* Compute calls crossed for BB. Live is a scratch bitvector.  */

static void
regstat_bb_compute_calls_crossed (unsigned int bb_index, bitmap live)
{
  basic_block bb = BASIC_BLOCK_FOR_FN (cfun, bb_index);
  rtx insn;
  df_ref def, use;

  bitmap_copy (live, df_get_live_out (bb));

  /* Process the artificial defs and uses at the bottom of the block
     to begin processing.  */
  FOR_EACH_ARTIFICIAL_DEF (def, bb_index)
    if ((DF_REF_FLAGS (def) & DF_REF_AT_TOP) == 0)
      bitmap_clear_bit (live, DF_REF_REGNO (def));

  FOR_EACH_ARTIFICIAL_USE (use, bb_index)
    if ((DF_REF_FLAGS (use) & DF_REF_AT_TOP) == 0)
      bitmap_set_bit (live, DF_REF_REGNO (use));

  FOR_BB_INSNS_REVERSE (bb, insn)
    {
      struct df_insn_info *insn_info = DF_INSN_INFO_GET (insn);
      unsigned int regno;

      if (!INSN_P (insn))
	continue;

      /* Process the defs.  */
      if (CALL_P (insn))
	{
	  bitmap_iterator bi;
	  EXECUTE_IF_SET_IN_BITMAP (live, 0, regno, bi)
	    {
	      REG_N_CALLS_CROSSED (regno)++;
	      REG_FREQ_CALLS_CROSSED (regno) += REG_FREQ_FROM_BB (bb);
	      REG_FREQ_CALLS_CROSSED (regno) =
		MIN (REG_FREQ_CALLS_CROSSED (regno), REG_FREQ_MAX);
	    }
	}

      /* All of the defs except the return value are some sort of
	 clobber.  This code is for the return.  */
      FOR_EACH_INSN_INFO_DEF (def, insn_info)
	{
	  if ((!CALL_P (insn))
	      || (!(DF_REF_FLAGS (def) & (DF_REF_MUST_CLOBBER | DF_REF_MAY_CLOBBER))))
	    {
	      /* Kill this register if it is not a subreg store or conditional store.  */
	      if (!(DF_REF_FLAGS (def) & (DF_REF_PARTIAL | DF_REF_CONDITIONAL)))
		bitmap_clear_bit (live, DF_REF_REGNO (def));
	    }
	}

      FOR_EACH_INSN_INFO_USE (use, insn_info)
	bitmap_set_bit (live, DF_REF_REGNO (use));
    }
}


/* Compute register info: lifetime, bb, and number of defs and uses.  */
void
regstat_compute_calls_crossed (void)
{
  basic_block bb;
  bitmap live = BITMAP_ALLOC (&df_bitmap_obstack);

  /* Initialize everything.  */
  gcc_assert (!reg_info_p);

  timevar_push (TV_REG_STATS);
  max_regno = max_reg_num ();
  reg_info_p_size = max_regno;
  reg_info_p = XCNEWVEC (struct reg_info_t, max_regno);

  FOR_EACH_BB_FN (bb, cfun)
    {
      regstat_bb_compute_calls_crossed (bb->index, live);
    }

  BITMAP_FREE (live);
  timevar_pop (TV_REG_STATS);
}


/* Free all storage associated with the problem.  */

void
regstat_free_calls_crossed (void)
{
  gcc_assert (reg_info_p);
  reg_info_p_size = 0;
  free (reg_info_p);
  reg_info_p = NULL;
}

/* Dump the register info to FILE.  */

void
dump_reg_info (FILE *file)
{
  unsigned int i, max = max_reg_num ();
  if (reload_completed)
    return;

  if (reg_info_p_size < max)
    max = reg_info_p_size;

  fprintf (file, "%d registers.\n", max);
  for (i = FIRST_PSEUDO_REGISTER; i < max; i++)
    {
      enum reg_class rclass, altclass;

      if (regstat_n_sets_and_refs)
	fprintf (file, "\nRegister %d used %d times across %d insns",
		 i, REG_N_REFS (i), REG_LIVE_LENGTH (i));
      else if (df)
	fprintf (file, "\nRegister %d used %d times across %d insns",
		 i, DF_REG_USE_COUNT (i) + DF_REG_DEF_COUNT (i), REG_LIVE_LENGTH (i));

      if (REG_BASIC_BLOCK (i) >= NUM_FIXED_BLOCKS)
	fprintf (file, " in block %d", REG_BASIC_BLOCK (i));
      if (regstat_n_sets_and_refs)
	fprintf (file, "; set %d time%s", REG_N_SETS (i),
		 (REG_N_SETS (i) == 1) ? "" : "s");
      else if (df)
	fprintf (file, "; set %d time%s", DF_REG_DEF_COUNT (i),
		 (DF_REG_DEF_COUNT (i) == 1) ? "" : "s");
      if (regno_reg_rtx[i] != NULL && REG_USERVAR_P (regno_reg_rtx[i]))
	fputs ("; user var", file);
      if (REG_N_DEATHS (i) != 1)
	fprintf (file, "; dies in %d places", REG_N_DEATHS (i));
      if (REG_N_CALLS_CROSSED (i) == 1)
	fputs ("; crosses 1 call", file);
      else if (REG_N_CALLS_CROSSED (i))
	fprintf (file, "; crosses %d calls", REG_N_CALLS_CROSSED (i));
      if (REG_FREQ_CALLS_CROSSED (i))
	fprintf (file, "; crosses call with %d frequency", REG_FREQ_CALLS_CROSSED (i));
      if (regno_reg_rtx[i] != NULL
	  && PSEUDO_REGNO_BYTES (i) != UNITS_PER_WORD)
	fprintf (file, "; %d bytes", PSEUDO_REGNO_BYTES (i));

      rclass = reg_preferred_class (i);
      altclass = reg_alternate_class (i);
      if (rclass != GENERAL_REGS || altclass != ALL_REGS)
	{
	  if (altclass == ALL_REGS || rclass == ALL_REGS)
	    fprintf (file, "; pref %s", reg_class_names[(int) rclass]);
	  else if (altclass == NO_REGS)
	    fprintf (file, "; %s or none", reg_class_names[(int) rclass]);
	  else
	    fprintf (file, "; pref %s, else %s",
		     reg_class_names[(int) rclass],
		     reg_class_names[(int) altclass]);
	}

      if (regno_reg_rtx[i] != NULL && REG_POINTER (regno_reg_rtx[i]))
	fputs ("; pointer", file);
      fputs (".\n", file);
    }
}

