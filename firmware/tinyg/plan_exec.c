/*
 * plan_exec.c - execution function for acceleration managed lines
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2013 Alden S. Hart, Jr.
 * Copyright (c) 2012 - 2013 Rob Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "tinyg.h"
#include "config.h"
#include "planner.h"
#include "kinematics.h"
#include "stepper.h"
#include "encoder.h"
#include "report.h"
#include "util.h"
//#include "xio.h"			// uncomment for debugging

#ifdef __cplusplus
extern "C"{
#endif

// execute routines (NB: These are all called from the LO interrupt)
static stat_t _exec_aline_head(void);
static stat_t _exec_aline_body(void);
static stat_t _exec_aline_tail(void);
static stat_t _exec_aline_segment(uint8_t correction_flag);
static void _init_forward_diffs(float t0, float t2);


/*************************************************************************
 * mp_exec_move() - execute runtime functions to prep move for steppers
 *
 *	Dequeues the buffer queue and executes the move continuations.
 *	Manages run buffers and other details
 */

stat_t mp_exec_move()
{
	mpBuf_t *bf;

	if ((bf = mp_get_run_buffer()) == NULL) return (STAT_NOOP);	// NULL means nothing's running

	// Manage cycle and motion state transitions
	// Cycle auto-start for lines only
	if (bf->move_type == MOVE_TYPE_ALINE) {
//		if (cm.cycle_state == CYCLE_OFF) {	// testing if this can be removed
//			cm_cycle_start();				// this should already be taken care of by G0,G1,G2,G3
//		}
		if (cm.motion_state == MOTION_STOP) cm_set_motion_state(MOTION_RUN);
	}
	if (bf->bf_func != NULL) { return (bf->bf_func(bf));} 	// run the move callback in the planner buffer
//	return(cm_hard_alarm(STAT_INTERNAL_ERROR));				// never supposed to get here
	return(STAT_INTERNAL_ERROR);				// never supposed to get here
}

/*************************************************************************/
/**** ALINE EXECUTION ROUTINES *******************************************/
/*************************************************************************
 * ---> Everything here fires from interrupts and must be interrupt safe
 *
 *  _exec_aline()		  - acceleration line main routine
 *	_exec_aline_head()	  - helper for acceleration section
 *	_exec_aline_body()	  - helper for cruise section
 *	_exec_aline_tail()	  - helper for deceleration section
 *	_exec_aline_segment() - helper for running a segment
 *
 *	Returns:
 *	 STAT_OK		move is done
 *	 STAT_EAGAIN	move is not finished - has more segments to run
 *	 STAT_NOOP		cause no operation from the steppers - do not load the move
 *	 STAT_xxxxx		fatal error. Ends the move and frees the bf buffer
 *	
 *	This routine is called from the (LO) interrupt level. The interrupt 
 *	sequencing relies on the behaviors of the routines being exactly correct.
 *	Each call to _exec_aline() must execute and prep *one and only one* 
 *	segment. If the segment is the not the last segment in the bf buffer the 
 *	_aline() must return STAT_EAGAIN. If it's the last segment it must return 
 *	STAT_OK. If it encounters a fatal error that would terminate the move it 
 *	should return a valid error code. Failure to obey this will introduce 
 *	subtle and very difficult to diagnose bugs (trust me on this).
 *
 *	Note 1 Returning STAT_OK ends the move and frees the bf buffer. 
 *		   Returning STAT_OK at this point does NOT advance position meaning any
 *		   position error will be compensated by the next move.
 *
 *	Note 2 Solves a potential race condition where the current move ends but the 
 * 		   new move has not started because the previous move is still being run 
 *		   by the steppers. Planning can overwrite the new move.
 */
/* OPERATION:
 *	Aline generates jerk-controlled S-curves as per Ed Red's course notes:
 *	  http://www.et.byu.edu/~ered/ME537/Notes/Ch5.pdf
 *	  http://www.scribd.com/doc/63521608/Ed-Red-Ch5-537-Jerk-Equations
 *
 *	A full trapezoid is divided into 5 periods Periods 1 and 2 are the 
 *	first and second halves of the acceleration ramp (the concave and convex 
 *	parts of the S curve in the "head"). Periods 3 and 4 are the first 
 *	and second parts of the deceleration ramp (the tail). There is also 
 *	a period for the constant-velocity plateau of the trapezoid (the body).
 *	There are various degraded trapezoids possible, including 2 section 
 *	combinations (head and tail; head and body; body and tail), and single 
 *	sections - any one of the three.
 *
 *	The equations that govern the acceleration and deceleration ramps are:
 *
 *	  Period 1	  V = Vi + Jm*(T^2)/2
 *	  Period 2	  V = Vh + As*T - Jm*(T^2)/2
 *	  Period 3	  V = Vi - Jm*(T^2)/2
 *	  Period 4	  V = Vh + As*T + Jm*(T^2)/2
 *
 * 	These routines play some games with the acceleration and move timing 
 *	to make sure this actually all works out. move_time is the actual time of the 
 *	move, accel_time is the time valaue needed to compute the velocity - which 
 *	takes the initial velocity into account (move_time does not need to).
 */
/* --- State transitions - hierarchical state machine ---
 *
 *	bf->move_state transitions:
 *	 from _NEW to _RUN on first call (sub_state set to _OFF)
 *	 from _RUN to _OFF on final call
 * 	 or just remains _OFF
 *
 *	mr.move_state transitions on first call from _OFF to one of _HEAD, _BODY, _TAIL
 *	Within each section state may be 
 *	 _NEW - trigger initialization
 *	 _RUN1 - run the first part
 *	 _RUN2 - run the second part 
 *
 *	Note: For a direct math implementation see build 357.xx or earlier
 *		  Builds 358 onward have only forward difference code
 */
stat_t mp_exec_aline(mpBuf_t *bf)
{
	if (bf->move_state == MOVE_OFF) { return (STAT_NOOP);} 

	// start a new move by setting up local context (singleton)
	if (mr.move_state == MOVE_OFF) {
		if (cm.hold_state == FEEDHOLD_HOLD) { return (STAT_NOOP);}// stops here if holding

		// initialization to process the new incoming bf buffer (Gcode block)
		memcpy(&mr.gm, &(bf->gm), sizeof(GCodeState_t));// copy in the gcode model state
		bf->replannable = false;
														// too short lines have already been removed
		if (fp_ZERO(bf->length)) {						// ...looks for an actual zero here
			mr.move_state = MOVE_OFF;					// reset mr buffer
			mr.section_state = SECTION_OFF;
			bf->nx->replannable = false;				// prevent overplanning (Note 2)
			st_prep_null();								// call this to keep the loader happy
			mp_free_run_buffer();
			return (STAT_NOOP);
		}
		bf->move_state = MOVE_RUN;
		mr.move_state = MOVE_RUN;
		mr.section = SECTION_HEAD;
		mr.section_state = SECTION_NEW;
		mr.jerk = bf->jerk;
		mr.jerk_div2 = bf->jerk/2;						// needed by __JERK_EXEC

		mr.head_length = bf->head_length;
		mr.body_length = bf->body_length;
		mr.tail_length = bf->tail_length;
		
		mr.entry_velocity = bf->entry_velocity;
		mr.cruise_velocity = bf->cruise_velocity;
		mr.exit_velocity = bf->exit_velocity;

		copy_axis_vector(mr.unit, bf->unit);
		copy_axis_vector(mr.target, bf->gm.target);		// save the final target of the move

		// generate the section targets for endpoint position correction
		for (uint8_t i=0; i<AXES; i++) {
			mr.section_target[SECTION_HEAD][i] = mr.position[i] + mr.unit[i] * mr.head_length;
			mr.section_target[SECTION_BODY][i] = mr.position[i] + mr.unit[i] * (mr.head_length + mr.body_length);
			mr.section_target[SECTION_TAIL][i] = mr.position[i] + mr.unit[i] * (mr.head_length + mr.body_length + mr.tail_length);
		}
	}
	// NB: from this point on the contents of the bf buffer do not affect execution

	//**** main dispatcher to process segments ***
	stat_t status = STAT_OK;
	if (mr.section == SECTION_HEAD) { status = _exec_aline_head();} else 
	if (mr.section == SECTION_BODY) { status = _exec_aline_body();} else
	if (mr.section == SECTION_TAIL) { status = _exec_aline_tail();} else 
	if (mr.move_state == MOVE_SKIP) { status = STAT_OK;}
	else { return (STAT_INTERNAL_ERROR);}

	// Feedhold processing. Refer to canonical_machine.h for state machine
	// Catch the feedhold request and start the planning the hold
	if (cm.hold_state == FEEDHOLD_SYNC) { cm.hold_state = FEEDHOLD_PLAN;}

	// Look for the end of the decel to go into HOLD state
	if ((cm.hold_state == FEEDHOLD_DECEL) && (status == STAT_OK)) {
		cm.hold_state = FEEDHOLD_HOLD;
		cm_set_motion_state(MOTION_HOLD);

//		mp_free_run_buffer();				// free bf and send a status report
		sr_request_status_report(SR_IMMEDIATE_REQUEST);
	}

	// There are 3 things that can happen here depending on return conditions:
	//	  status	 bf->move_state	 Description
	//    ---------	 --------------	 ----------------------------------------
	//	  STAT_EAGAIN	 <don't care>	 mr buffer has more segments to run
	//	  STAT_OK		 MOVE_STATE_RUN	 mr and bf buffers are done
	//	  STAT_OK		 MOVE_STATE_NEW	 mr done; bf must be run again (it's been reused)

	if (status == STAT_EAGAIN) { 
		sr_request_status_report(SR_TIMED_REQUEST); // continue reporting mr buffer
	} else {
		mr.move_state = MOVE_OFF;					// reset mr buffer
		mr.section_state = SECTION_OFF;
		bf->nx->replannable = false;				// prevent overplanning (Note 2)
		if (bf->move_state == MOVE_RUN) {
			mp_free_run_buffer();					// free bf if it's actually done
		}
	}
	return (status);
}

/* Forward difference math explained:
 * 	We're using two quadratic bezier curves end-to-end, forming the concave and convex 
 *	section of the s-curve. For each half we have three points:
 *
 *    T[0] is the start point, or the entry or middle of the "s". This will be one of:
 *			- entry_velocity (acceleration concave),
 *			- cruise_velocity (deceleration concave), or
 *			- midpoint_velocity (convex)
 *	  T[1] is the "control point" set to T[0] for concave sections, and T[2] for convex
 *	  T[2] is the end point of the quadratic, which will be the midpoint or endpoint of the s.
 *
 *  TODO MATH EXPLANATION
 *  
 *    A = T[0] - 2*T[1] + T[2]
 *    B = 2 * (T[1] - T[0])
 *    C = T[0]
 *    h = (1/mr.segments)
 *
 *  forward_diff_1 = Ah^2+Bh = (T[0] - 2*T[1] + T[2])h*h + (2 * (T[1] - T[0]))h
 *  forward_diff_2 = 2Ah^2 = 2*(T[0] - 2*T[1] + T[2])h*h
 */

static void _init_forward_diffs(float t0, float t2)		// NB: t1 will always be == t0, so we don't pass it
{
	// A = T[0] - 2*T[1] + T[2], if T[0] == T[1], then it becomes - T[0] + T[2]
	float AH_squared = (t2 - t0) * square(1/mr.segments); // square(1/mr.segments) is H_squared

	// Ah^2 + Bh, and B=2 * (T[1] - T[0]), if T[0] == T[1], then it becomes simply Ah^2
	mr.forward_diff_1 = AH_squared;
	mr.forward_diff_2 = AH_squared * 2;
	mr.segment_velocity = t0;
}

/*
 * _exec_aline_head()
 */
static stat_t _exec_aline_head()
{
	if (mr.section_state == SECTION_NEW) {					// initialize the move singleton (mr)
		if (fp_ZERO(mr.head_length)) { 
			mr.section = SECTION_BODY;
			return(_exec_aline_body());						// skip ahead to the body generator
		}
		mr.midpoint_velocity = (mr.entry_velocity + mr.cruise_velocity) / 2;
		mr.gm.move_time = mr.head_length / mr.midpoint_velocity;	// time for entire accel region
		mr.segments = ceil(uSec(mr.gm.move_time) / (2 * cm.estd_segment_usec)); // # of segments in *each half*
		mr.segment_time = mr.gm.move_time / (2 * mr.segments);

		// 4 lines needed by __JERK_EXEC
		mr.accel_time = 2 * sqrt((mr.cruise_velocity - mr.entry_velocity) / mr.jerk);
		mr.midpoint_acceleration = 2 * (mr.cruise_velocity - mr.entry_velocity) / mr.accel_time;
		mr.segment_accel_time = mr.accel_time / (2 * mr.segments);	// time to advance for each segment
		mr.elapsed_accel_time = mr.segment_accel_time / 2;			// elapsed time starting point (offset)

		// line needed by fwd-diff exec
		_init_forward_diffs(mr.entry_velocity, mr.midpoint_velocity);

		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_time)) < MIN_SEGMENT_USEC) {
			return(STAT_GCODE_BLOCK_SKIPPED);				// exit without advancing position
		}
		mr.section = SECTION_HEAD;
		mr.section_state = SECTION_1st_HALF;
	}
	if (mr.section_state == SECTION_1st_HALF) {				// concave part of accel curve (period 1)
#ifdef __JERK_EXEC
		mr.segment_velocity = mr.entry_velocity + (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == STAT_OK) { 			// set up for second half
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = SECTION_2nd_HALF;
			mr.elapsed_accel_time = mr.segment_accel_time / 2;	// start time from midpoint of segment
		}
#else
		mr.segment_velocity += mr.forward_diff_1;
		if (_exec_aline_segment(false) == STAT_OK) { 			// set up for second half
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = SECTION_2nd_HALF;
			// Here's a trick: The second half of the S starts at the end of the first,
			//  And the only thing that changes is the sign of mr.forward_diff_2
			mr.forward_diff_2 = -mr.forward_diff_2;
		} else {
			mr.forward_diff_1 += mr.forward_diff_2;
		}
#endif
		return(STAT_EAGAIN);
	}
	if (mr.section_state == SECTION_2nd_HALF) {					// convex part of accel curve (period 2)
#ifdef __JERK_EXEC
		mr.segment_velocity = mr.midpoint_velocity +
			(mr.elapsed_accel_time * mr.midpoint_acceleration) -
			(square(mr.elapsed_accel_time) * mr.jerk_div2);
#else
		mr.segment_velocity += mr.forward_diff_1;
		mr.forward_diff_1 += mr.forward_diff_2;
#endif
		if (_exec_aline_segment(false) == STAT_OK) {			// OK means this section is done
			if ((fp_ZERO(mr.body_length)) && (fp_ZERO(mr.tail_length))) return(STAT_OK); // ends the move
			mr.section = SECTION_BODY;
			mr.section_state = SECTION_NEW;
		}
	}
	return(STAT_EAGAIN);
}

/*
 * _exec_aline_body()
 *
 *	The body is broken into little segments even though it is a straight line so that 
 *	feedholds can happen in the middle of a line with a minimum of latency
 */
static stat_t _exec_aline_body()
{
	if (mr.section_state == SECTION_NEW) {
		if (fp_ZERO(mr.body_length)) {
			mr.section = SECTION_TAIL;
			return(_exec_aline_tail());						// skip ahead to tail periods
		}
		mr.gm.move_time = mr.body_length / mr.cruise_velocity;
		mr.segments = ceil(uSec(mr.gm.move_time) / cm.estd_segment_usec);
		mr.segment_time = mr.gm.move_time / mr.segments;
		mr.segment_velocity = mr.cruise_velocity;
		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_time)) < MIN_SEGMENT_USEC) {
			return(STAT_GCODE_BLOCK_SKIPPED);				// exit without advancing position
		}
		mr.section = SECTION_BODY;
		mr.section_state = SECTION_2nd_HALF;				// uses PERIOD_2 so last segment detection works
	}
	if (mr.section_state == SECTION_2nd_HALF) {				// straight part (period 3)
		if (_exec_aline_segment(false) == STAT_OK) {		// OK means this section is done
			if (fp_ZERO(mr.tail_length)) return(STAT_OK);	// ends the move
			mr.section = SECTION_TAIL;
			mr.section_state = SECTION_NEW;
		}
	}
	return(STAT_EAGAIN);
}

/*
 * _exec_aline_tail()
 */

static stat_t _exec_aline_tail()
{
	if (mr.section_state == SECTION_NEW) {
		if (fp_ZERO(mr.tail_length)) { return(STAT_OK);}	// end the move
		mr.midpoint_velocity = (mr.cruise_velocity + mr.exit_velocity) / 2;
		mr.gm.move_time = mr.tail_length / mr.midpoint_velocity;
		mr.segments = ceil(uSec(mr.gm.move_time) / (2 * cm.estd_segment_usec));// # of segments in *each half*
		mr.segment_time = mr.gm.move_time / (2 * mr.segments);// time to advance for each segment

		// 4 lines needed by jerk-based exec
		mr.accel_time = 2 * sqrt((mr.cruise_velocity - mr.exit_velocity) / mr.jerk);
		mr.midpoint_acceleration = 2 * (mr.cruise_velocity - mr.exit_velocity) / mr.accel_time;
		mr.segment_accel_time = mr.accel_time / (2 * mr.segments);// time to advance for each segment
		mr.elapsed_accel_time = mr.segment_accel_time / 2; //compute time from midpoint of segment

		// line needed by fwd-diff exec
		_init_forward_diffs(mr.cruise_velocity, mr.midpoint_velocity);

		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_time)) < MIN_SEGMENT_USEC) {
			return(STAT_GCODE_BLOCK_SKIPPED);				// exit without advancing position
		}
		mr.section = SECTION_TAIL;
		mr.section_state = SECTION_1st_HALF;
	}
	if (mr.section_state == SECTION_1st_HALF) {				// convex part (period 4)
#ifdef __JERK_EXEC
		mr.segment_velocity = mr.cruise_velocity - (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == STAT_OK) {				// set up for second half
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = SECTION_2nd_HALF;
			mr.elapsed_accel_time = mr.segment_accel_time / 2;// start time from midpoint of segment
		}
#else
		mr.segment_velocity += mr.forward_diff_1;
		if (_exec_aline_segment(false) == STAT_OK) {				// set up for second half
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = SECTION_2nd_HALF;
			// Here's a trick: The second half of the S starts at the end of the first,
			//  And the only thing that changes is the sign of mr.forward_diff_2
			mr.forward_diff_2 = -mr.forward_diff_2;
			} else {
			mr.forward_diff_1 += mr.forward_diff_2;
		}
#endif
		return(STAT_EAGAIN);
	}
	if (mr.section_state == SECTION_2nd_HALF) {				// concave part (period 5)
#ifdef __JERK_EXEC
		mr.segment_velocity = mr.midpoint_velocity -
		(mr.elapsed_accel_time * mr.midpoint_acceleration) +
		(square(mr.elapsed_accel_time) * mr.jerk_div2);
#else
		mr.segment_velocity += mr.forward_diff_1;
		mr.forward_diff_1 += mr.forward_diff_2;
#endif
		return (_exec_aline_segment(true)); 					// ends the move or continues EAGAIN
	}
	return(STAT_EAGAIN);									// should never get here
}

/*
 * _exec_aline_segment() - segment runner helper
 */
static stat_t _exec_aline_segment(uint8_t correction_flag)
{
	float travel[AXES];
	float steps[MOTORS];
	uint8_t i;

#ifdef __ORIG_CORRECTION_CODE

	// Multiply computed length by the unit vector to get the contribution for each axis. 
	// Set the target in absolute coords and compute relative steps.
	// Don't do the endpoint correction if you are going into a hold
	if ((correction_flag == true) && (mr.segment_count == 1) && 
		(cm.motion_state == MOTION_RUN) && (cm.cycle_state == CYCLE_MACHINING)) {
//		printf("m[2]:%0.4f, %0.4f\n", (double)mr.gm.target[AXIS_Z], (double)mr.target[AXIS_Z]);	// +++++ DIAGNOSTIC
		for (i=0; i<AXES; i++) {
			mr.gm.target[i] = mr.target[i]; // correct any accumulated rounding errors in last segment
		}
	} else {
		float segment_length = mr.segment_velocity * mr.segment_time;
		for (i=0; i<AXES; i++) {
			mr.gm.target[i] = mr.position[i] + (mr.unit[i] * segment_length);
		}
	}
#else // new error correction
	// Multiply computed length by the unit vector to get the contribution for each axis. 
	// Set the target in absolute coords and compute relative steps.
	// Don't do the endpoint correction if you are going into a hold
	uint8_t end_flag = correction_flag;
	
	if ((end_flag == true) && (mr.segment_count == 1) && 
		(cm.motion_state == MOTION_RUN) && (cm.cycle_state == CYCLE_MACHINING)) {

//		printf("M[0]:%0.4f, %0.4f\n", (double)mr.gm.target[AXIS_X], (double)mr.target[AXIS_X]);	// +++++ DIAGNOSTIC
//		printf("M[1]:%0.4f, %0.4f\n", (double)mr.gm.target[AXIS_Y], (double)mr.target[AXIS_Y]);	// +++++ DIAGNOSTIC
//		printf("M[2]:%0.4f, %0.4f\n", (double)mr.gm.target[AXIS_Z], (double)mr.target[AXIS_Z]);	// +++++ DIAGNOSTIC

		// A positive correction means that distance will be added to the subsequent moves,
		// looked at another way, that the target fell short of the endpoint. Neg is opposite.
		for (i=0; i<AXES; i++) {
			mr.position_correction[i] += (mr.target[i] - mr.gm.target[i]);
		}
	} else {
		float segment_length = mr.segment_velocity * mr.segment_time;
		for (i=0; i<AXES; i++) {
			mr.gm.target[i] = mr.position[i] + (mr.unit[i] * segment_length);
		}
	}
	// corrected for position error
	for (i=0; i<AXIS_A; i++) {			// NOTE - only 3 axes.
		if (fabs(mr.position_correction[i]) > MIN_CORRECTION_MM) {
			mr.gm.target[i] += min(mr.position_correction[i], MAX_CORRECTION_MM);
			mr.position_correction[i] -= min(mr.position_correction[i], MAX_CORRECTION_MM);
//			if (i == AXIS_Z) {
//				printf("C[%d]:%0.4f, %0.4f\n", i, (double)mr.gm.target[AXIS_Z], (double)mr.position_correction[AXIS_Z]);	// +++++ DIAGNOSTIC
//			}			
		}
	}
#endif
	for (i=0; i<AXES; i++) {
		travel[i] = mr.gm.target[i] - mr.position[i];
	}

/* The above is a re-arranged version of this:
	for (uint8_t i=0; i < AXES; i++) {	// don't do the error correction if you are going into a hold
		if ((correction_flag == true) && (mr.segment_count == 1) && 
			(cm.motion_state == MOTION_RUN) && (cm.cycle_state == CYCLE_STARTED)) {
			mr.gm.target[i] = mr.endpoint[i];	// rounding error correction for last segment
		} else {
			mr.gm.target[i] = mr.position[i] + (mr.unit[i] * mr.segment_velocity * mr.segment_move_time);
		}
		travel[i] = mr.gm.target[i] - mr.position[i];
	}
*/
	// prep the segment for the steppers and adjust the variables for the next iteration
	ik_kinematics(travel, steps);
	if (st_prep_line(steps, mr.microseconds) == STAT_OK) {
		copy_axis_vector(mr.position, mr.gm.target); 	// update runtime position	
		mr.elapsed_accel_time += mr.segment_accel_time;	// line needed by __JERK_EXEC
	}
	if (--mr.segment_count == 0) return (STAT_OK);		// this section has run all its segments
	return (STAT_EAGAIN);								// this section still has more segments to run
}

/*
 * mp_print_motor_position()
 * mp_print_motor_positions()
 */

void mp_print_motor_position(const uint8_t motor)
{
/*
	printf("{\"m%d\":{\"tgt\":%0.3f,\"pos\":%0.3f,\"dly\":%0.3f,\"enc\":%0.3f,\"err\":%0.3f,mm:%0.5f}}\n",
		motor+1,
		(double)mr.target_steps[motor],
		(double)mr.position_steps[motor],
		(double)mr.delayed_steps[motor],
		(double)mr.encoder_steps[motor],
		(double)mr.encoder_error[motor],
		(double)(mr.encoder_error[motor] * st_cfg.mot[motor].units_per_step));
*/
}

void mp_print_motor_positions()
{
	printf("\n");
	mp_print_motor_position(MOTOR_1);
	mp_print_motor_position(MOTOR_2);
	mp_print_motor_position(MOTOR_3);
//	mp_print_motor_position(MOTOR_4);
/*
	for (uint8_t i=0; i<MOTORS; i++) {
		mp_print_motor_position(i);
	}
*/
}

#ifdef __cplusplus
}
#endif
