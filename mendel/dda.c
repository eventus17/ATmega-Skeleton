#include	"dda.h"

#include	<string.h>
#include	<avr/interrupt.h>

#include	"timer.h"
#include	"serial.h"
#include	"sermsg.h"
#include	"dda_queue.h"
#include	"debug.h"
#include	"sersendf.h"

#ifndef	ABS
#define	ABS(v)		(((v) >= 0)?(v):(-(v)))
#endif

#ifndef	ABSDELTA
#define	ABSDELTA(a, b)	(((a) >= (b))?((a) - (b)):((b) - (a)))
#endif

/*
	step timeout
*/

uint8_t	steptimeout = 0;

/*
	position tracking
*/

TARGET startpoint __attribute__ ((__section__ (".bss")));
TARGET current_position __attribute__ ((__section__ (".bss")));

/*
	utility functions
*/

// courtesy of http://www.oroboro.com/rafael/docserv.php/index/programming/article/distance
uint32_t approx_distance( uint32_t dx, uint32_t dy )
{
	uint32_t min, max, approx;

	if ( dx < dy )
	{
		min = dx;
		max = dy;
	} else {
		min = dy;
		max = dx;
	}

	approx = ( max * 1007 ) + ( min * 441 );
	if ( max < ( min << 4 ))
		approx -= ( max * 40 );

	// add 512 for proper rounding
	return (( approx + 512 ) >> 10 );
}

// courtesy of http://www.oroboro.com/rafael/docserv.php/index/programming/article/distance
uint32_t approx_distance_3( uint32_t dx, uint32_t dy, uint32_t dz )
{
	uint32_t min, med, max, approx;

	if ( dx < dy )
	{
		min = dy;
		med = dx;
	} else {
		min = dx;
		med = dy;
	}

	if ( dz < min )
	{
		max = med;
		med = min;
		min = dz;
	} else if ( dz < med ) {
		max = med;
		med = dz;
	} else {
		max = dz;
	}

	approx = ( max * 860 ) + ( med * 851 ) + ( min * 520 );
	if ( max < ( med << 1 )) approx -= ( max * 294 );
	if ( max < ( min << 2 )) approx -= ( max * 113 );
	if ( med < ( min << 2 )) approx -= ( med *  40 );

	// add 512 for proper rounding
	return (( approx + 512 ) >> 10 );
}

uint32_t delta32(uint32_t v1, uint32_t v2) {
	if (v1 >= v2)
		return v1 - v2;
	return v2 - v1;
}

// this is an ultra-crude pseudo-logarithm routine, such that:
// 2 ^ msbloc(v) >= v
const uint8_t	msbloc (uint32_t v) {
	uint8_t i;
	uint32_t c;
	for (i = 31, c = 0x80000000; i; i--) {
		if (v & c)
			return i;
		c >>= 1;
	}
	return 0;
}

/*
	CREATE a dda given current_position and a target, save to passed location so we can write directly into the queue
*/

void dda_create(DDA *dda, TARGET *target) {
	uint32_t	distance;

	// initialise DDA to a known state
	dda->live = 0;
	dda->waitfor_temp = 0;

	if (debug_flags & DEBUG_DDA)
		serial_writestr_P(PSTR("\n{DDA_CREATE: ["));

	// we end at the passed target
	memcpy(&(dda->endpoint), target, sizeof(TARGET));

	dda->x_delta = ABS(target->X - startpoint.X);
	dda->y_delta = ABS(target->Y - startpoint.Y);
	dda->z_delta = ABS(target->Z - startpoint.Z);
	dda->e_delta = ABS(target->E - startpoint.E);

	dda->x_direction = (target->X >= startpoint.X)?1:0;
	dda->y_direction = (target->Y >= startpoint.Y)?1:0;
	dda->z_direction = (target->Z >= startpoint.Z)?1:0;
	dda->e_direction = (target->E >= startpoint.E)?1:0;

	if (debug_flags & DEBUG_DDA) {
		if (dda->x_direction == 0)
			serial_writechar('-');
		serwrite_uint32(dda->x_delta); serial_writechar(',');
		if (dda->y_direction == 0)
			serial_writechar('-');
		serwrite_uint32(dda->y_delta); serial_writechar(',');
		if (dda->z_direction == 0)
			serial_writechar('-');
		serwrite_uint32(dda->z_delta); serial_writechar(',');
		if (dda->e_direction == 0)
			serial_writechar('-');
		serwrite_uint32(dda->e_delta);

		serial_writestr_P(PSTR("] ["));
	}

	dda->total_steps = dda->x_delta;
	if (dda->y_delta > dda->total_steps)
		dda->total_steps = dda->y_delta;
	if (dda->z_delta > dda->total_steps)
		dda->total_steps = dda->z_delta;
	if (dda->e_delta > dda->total_steps)
		dda->total_steps = dda->e_delta;

	if (debug_flags & DEBUG_DDA) {
		serial_writestr_P(PSTR("ts:")); serwrite_uint32(dda->total_steps);
	}

	if (dda->total_steps == 0) {
		dda->nullmove = 1;
	}
	else {
		// get steppers ready to go
		steptimeout = 0;
		power_on();

		dda->x_counter = dda->y_counter = dda->z_counter = dda->e_counter =
			-(dda->total_steps >> 1);

		// since it's unusual to combine X, Y and Z changes in a single move on reprap, check if we can use simpler approximations before trying the full 3d approximation.
		if (dda->z_delta == 0)
			distance = approx_distance(dda->x_delta * UM_PER_STEP_X, dda->y_delta * UM_PER_STEP_Y);
		else if (dda->x_delta == 0 && dda->y_delta == 0)
			distance = dda->z_delta * UM_PER_STEP_Z;
		else
			distance = approx_distance_3(dda->x_delta * UM_PER_STEP_X, dda->y_delta * UM_PER_STEP_Y, dda->z_delta * UM_PER_STEP_Z);

		if (distance < 2)
			distance = dda->e_delta * UM_PER_STEP_E;

		if (debug_flags & DEBUG_DDA) {
			serial_writestr_P(PSTR(",ds:")); serwrite_uint32(distance);
		}

		// pre-calculate move speed in millimeter microseconds per step minute for less math in interrupt context
		// mm (distance) * 60000000 us/min / step (total_steps) = mm.us per step.min
		//   note: um (distance) * 60000 == mm * 60000000
		// so in the interrupt we must simply calculate
		// mm.us per step.min / mm per min (F) = us per step

		// break this calculation up a bit and lose some precision because 300,000um * 60000 is too big for a uint32
		// calculate this with a uint64 if you need the precision, but it'll take longer so routines with lots of short moves may suffer
		// 2^32/6000 is about 715mm which should be plenty

		// changed * 10 to * (F_CPU / 100000) so we can work in cpu_ticks rather than microseconds.
		// timer.c setTimer() routine altered for same reason

		// changed distance * 6000 .. * F_CPU / 100000 to
		//         distance * 2400 .. * F_CPU / 40000 so we can move a distance of up to 1800mm without overflowing
		uint32_t move_duration = ((distance * 2400) / dda->total_steps) * (F_CPU / 40000);

		// c is initial step time in IOclk ticks
		dda->c = (move_duration / startpoint.F) << 8;

		if (debug_flags & DEBUG_DDA) {
			serial_writestr_P(PSTR(",md:")); serwrite_uint32(move_duration);
			serial_writestr_P(PSTR(",c:")); serwrite_uint32(dda->c >> 8);
		}

		if (startpoint.F != target->F) {
			uint32_t stF = startpoint.F / 4;
			uint32_t enF = target->F / 4;
			// now some constant acceleration stuff, courtesy of http://www.embedded.com/columns/technicalinsights/56800129?printable=true
			uint32_t ssq = (stF * stF);
			uint32_t esq = (enF * enF);
			int32_t dsq = (int32_t) (esq - ssq) / 4;

			uint8_t msb_ssq = msbloc(ssq);
			uint8_t msb_tot = msbloc(dda->total_steps);

			dda->end_c = (move_duration / target->F) << 8;
			// the raw equation WILL overflow at high step rates, but 64 bit math routines take waay too much space
			// at 65536 mm/min (1092mm/s), ssq/esq overflows, and dsq is also close to overflowing if esq/ssq is small
			// but if ssq-esq is small, ssq/dsq is only a few bits
			// we'll have to do it a few different ways depending on the msb locations of each
			if ((msb_tot + msb_ssq) <= 30) {
				// we have room to do all the multiplies first
				if (debug_flags & DEBUG_DDA)
					serial_writechar('A');
				dda->n = ((int32_t) (dda->total_steps * ssq) / dsq) + 1;
			}
			else if (msb_tot >= msb_ssq) {
				// total steps has more precision
				if (debug_flags & DEBUG_DDA)
					serial_writechar('B');
				dda->n = (((int32_t) dda->total_steps / dsq) * (int32_t) ssq) + 1;
			}
			else {
				// otherwise
				if (debug_flags & DEBUG_DDA)
					serial_writechar('C');
				dda->n = (((int32_t) ssq / dsq) * (int32_t) dda->total_steps) + 1;
			}

			if (debug_flags & DEBUG_DDA) {
// 				serial_writestr_P(PSTR("\n{DDA:CA end_c:")); serwrite_uint32(dda->end_c >> 8);
// 				serial_writestr_P(PSTR(", n:")); serwrite_int32(dda->n);
// 				serial_writestr_P(PSTR(", md:")); serwrite_uint32(move_duration);
// 				serial_writestr_P(PSTR(", ssq:")); serwrite_uint32(ssq);
// 				serial_writestr_P(PSTR(", esq:")); serwrite_uint32(esq);
// 				serial_writestr_P(PSTR(", dsq:")); serwrite_int32(dsq);
// 				serial_writestr_P(PSTR(", msbssq:")); serwrite_uint8(msb_ssq);
// 				serial_writestr_P(PSTR(", msbtot:")); serwrite_uint8(msb_tot);
// 				serial_writestr_P(PSTR("}\n"));
				sersendf_P(PSTR("\n{DDA:CA end_c:%lu, n:%ld, md:%lu, ssq:%lu, esq:%lu, dsq:%lu, msbssq:%u, msbtot:%u}\n"), dda->end_c >> 8, dda->n, move_duration, ssq, esq, dsq, msb_ssq, msb_tot);
			}

			dda->accel = 1;
		}
		else
			dda->accel = 0;
	}

	if (debug_flags & DEBUG_DDA)
		serial_writestr_P(PSTR("] }\n"));

	// next dda starts where we finish
	memcpy(&startpoint, target, sizeof(TARGET));
	// E is always relative, reset it here
	startpoint.E = 0;
}

/*
	Start a prepared DDA
*/

void dda_start(DDA *dda) {
	// called from interrupt context: keep it simple!
	if (dda->nullmove) {
		// just change speed?
		current_position.F = dda->endpoint.F;
	}
	else {
		if (dda->waitfor_temp) {
			serial_writestr_P(PSTR("Waiting for target temp\n"));
		}
		else {
			// ensure steppers are ready to go
			steptimeout = 0;
			power_on();

			// set direction outputs
			x_direction(dda->x_direction);
			y_direction(dda->y_direction);
			z_direction(dda->z_direction);
			e_direction(dda->e_direction);
		}

		// ensure this dda starts
		dda->live = 1;
	}

	// set timeout for first step
	setTimer(dda->c >> 8);
}

/*
	CAN STEP
*/

uint8_t	can_step(uint8_t min, uint8_t max, int32_t current, int32_t target, uint8_t dir) {
	if (dir) {
		// forwards/positive
		if (max)
			return 0;
		if (current >= target)
			return 0;
	}
	else {
		// backwards/negative
		if (min)
			return 0;
		if (target >= current)
			return 0;
	}

	return 255;
}

/*
	STEP
*/

void dda_step(DDA *dda) {
	// called from interrupt context! keep it as simple as possible
	uint8_t	step_option = 0;
#define	X_CAN_STEP	1
#define	Y_CAN_STEP	2
#define	Z_CAN_STEP	4
#define	E_CAN_STEP	8
#define	DID_STEP		128

// 		step_option |= can_step(x_min(), x_max(), current_position.X, dda->endpoint.X, dda->x_direction) & X_CAN_STEP;
	step_option |= can_step(0      , 0      , current_position.X, dda->endpoint.X, dda->x_direction) & X_CAN_STEP;
// 		step_option |= can_step(y_min(), y_max(), current_position.Y, dda->endpoint.Y, dda->y_direction) & Y_CAN_STEP;
	step_option |= can_step(0      , 0      , current_position.Y, dda->endpoint.Y, dda->y_direction) & Y_CAN_STEP;
// 		step_option |= can_step(z_min(), z_max(), current_position.Z, dda->endpoint.Z, dda->z_direction) & Z_CAN_STEP;
	step_option |= can_step(0      , 0      , current_position.Z, dda->endpoint.Z, dda->z_direction) & Z_CAN_STEP;
	step_option |= can_step(0      , 0      , current_position.E, dda->endpoint.E, dda->e_direction) & E_CAN_STEP;
// 	step_option |= can_step(0      , 0      , current_position.F, dda->endpoint.F, dda->f_direction) & F_CAN_STEP;

	if (step_option & X_CAN_STEP) {
		dda->x_counter -= dda->x_delta;
		if (dda->x_counter < 0) {
			x_step();
			step_option |= DID_STEP;
			if (dda->x_direction)
				current_position.X++;
			else
				current_position.X--;

			dda->x_counter += dda->total_steps;
		}
	}

	if (step_option & Y_CAN_STEP) {
		dda->y_counter -= dda->y_delta;
		if (dda->y_counter < 0) {
			y_step();
			step_option |= DID_STEP;
			if (dda->y_direction)
				current_position.Y++;
			else
				current_position.Y--;

			dda->y_counter += dda->total_steps;
		}
	}

	if (step_option & Z_CAN_STEP) {
		dda->z_counter -= dda->z_delta;
		if (dda->z_counter < 0) {
			z_step();
			step_option |= DID_STEP;
			if (dda->z_direction)
				current_position.Z++;
			else
				current_position.Z--;

			dda->z_counter += dda->total_steps;
		}
	}

	if (step_option & E_CAN_STEP) {
		dda->e_counter -= dda->e_delta;
		if (dda->e_counter < 0) {
			e_step();
			step_option |= DID_STEP;
			if (dda->e_direction)
				current_position.E++;
			else
				current_position.E--;

			dda->e_counter += dda->total_steps;
		}
	}

	#if STEP_INTERRUPT_INTERRUPTIBLE
		// since we have sent steps to all the motors that will be stepping and the rest of this function isn't so time critical,
		// this interrupt can now be interruptible
		// however we must ensure that we don't step again while computing the below, so disable *this* interrupt but allow others to fire
// 		disableTimerInterrupt();
		sei();
	#endif

	// linear acceleration magic, courtesy of http://www.embedded.com/columns/technicalinsights/56800129?printable=true
	if (dda->accel) {
		if (
				((dda->n > 0) && (dda->c > dda->end_c)) ||
				((dda->n < 0) && (dda->c < dda->end_c))
			 ) {
			dda->c = (int32_t) dda->c - ((int32_t) (dda->c * 2) / dda->n);
			dda->n += 4;
			setTimer(dda->c >> 8);
		}
		else if (dda->c != dda->end_c) {
			dda->c = dda->end_c;
			setTimer(dda->c >> 8);
		}
		// else we are already at target speed
	}

	if (step_option) {
		// we stepped, reset timeout
		steptimeout = 0;

	// if we could do anything at all, we're still running
	// otherwise, must have finished
	}
	else {
		dda->live = 0;
		// reset E- always relative
		current_position.E = 0;
		// linear acceleration code doesn't alter F during a move, so we must update it here
		// in theory, we *could* update F every step, but that would require a divide in interrupt context which should be avoided if at all possible
		current_position.F = dda->endpoint.F;
	}

	// turn off step outputs, hopefully they've been on long enough by now to register with the drivers
	// if not, too bad. or insert a (very!) small delay here, or fire up a spare timer or something.
	// we also hope that we don't step before the drivers register the low- limit maximum speed if you think this is a problem.
	unstep();
}
