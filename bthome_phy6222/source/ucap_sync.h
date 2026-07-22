/******************************************************************************
 * ucap_sync.h — frame-synced listen-window scheduler for the IBSTH2P
 * inter-chip UART stream ("wake-on-RX", V21).
 *
 * Pure C with no SDK dependencies so it can be unit-tested on the host
 * (tests/test_ucap_sync.c).
 *
 * The main MCU streams one frame every ~10.4 s on its own free-running
 * clock (measured 10.34-10.41 s on one unit, drifting with temperature;
 * see IBSTH2P_PROJECT_PLAN.md "Frame-period probe"). Holding the UART RX
 * powered while waiting costs 1-2 mA, so instead the scheduler predicts
 * the next frame from the arrival time of the last one and opens a short
 * listen window around the prediction.
 *
 * Nothing is assumed about the exact period or its unit-to-unit spread:
 * the period estimate trains per device (EMA over measured inter-frame
 * deltas within sane bounds) and the window guard only narrows after a
 * streak of in-window catches, widening again on any miss. Failure
 * degrades stepwise — wider windows, then full-period reacquire, then a
 * low-duty retry cadence — never to a permanently-open receiver.
 *
 * The caller owns clocks and timers; this module only does arithmetic:
 *   good frame arrived   ucap_sync_on_frame(&s, dt_ms) -> open delay, ms
 *   window missed        ucap_sync_on_miss(&s)         -> open delay, ms
 *                                                         (0 = reopen now)
 *   window length        ucap_sync_window_ms(&s)       -> ms
 * dt_ms is the time from the previous good frame to this one, or
 * UCAP_SYNC_DT_UNKNOWN when that gap is not a clean single period (first
 * frame after boot or after a missed window).
 ******************************************************************************/
#ifndef _UCAP_SYNC_H_
#define _UCAP_SYNC_H_

#include <stdint.h>

#define UCAP_SYNC_DT_UNKNOWN	0xFFFFFFFFu

// Starting period estimate: the one measured unit. Only a seed — the
// estimate trains to the actual device within a few frames.
#define UCAP_SYNC_T_INIT_MS	10390u
// Sane bounds on a single frame period. A delta outside them (button-
// inserted extra frame, frames missed in between) re-anchors the phase but
// must not train the estimate.
#define UCAP_SYNC_T_MIN_MS	8000u
#define UCAP_SYNC_T_MAX_MS	13000u

// Listen-window guard around the predicted frame start. Wide by default;
// narrows only on sustained hits, so an unfamiliar or drifting unit simply
// stays at wider (still cheap) windows.
#define UCAP_SYNC_GUARD_INIT_MS	250u
#define UCAP_SYNC_GUARD_MIN_MS	60u
#define UCAP_SYNC_GUARD_MAX_MS	500u
#define UCAP_SYNC_SHRINK_HITS	8u

// One frame is 13 bytes at 9600 8N1 = ~13.5 ms on the wire, plus slack.
#define UCAP_SYNC_FRAME_MS	20u

// Miss escalation: after UCAP_SYNC_MISS_REACQ consecutive misses the next
// window spans a full period (guaranteed catch if the stream is alive);
// after UCAP_SYNC_MISS_BACKOFF the full-period attempts drop to one per
// UCAP_SYNC_BACKOFF_MS so a dead main MCU cannot pin the receiver on.
#define UCAP_SYNC_MISS_REACQ	2u
#define UCAP_SYNC_MISS_BACKOFF	6u
#define UCAP_SYNC_BACKOFF_MS	300000u

typedef struct {
	uint16_t t_est_ms;    // per-device period estimate
	uint16_t guard_ms;    // current one-sided window guard
	uint8_t  hit_streak;  // consecutive in-window catches (for narrowing)
	uint8_t  miss_streak; // consecutive missed windows (for escalation)
} ucap_sync_t;

static void ucap_sync_init(ucap_sync_t *s)
{
	s->t_est_ms = UCAP_SYNC_T_INIT_MS;
	s->guard_ms = UCAP_SYNC_GUARD_INIT_MS;
	s->hit_streak = 0;
	s->miss_streak = 0;
}

// Length of the listen window the last returned open-delay leads into.
static uint32_t ucap_sync_window_ms(const ucap_sync_t *s)
{
	if (s->miss_streak >= UCAP_SYNC_MISS_REACQ)
		// Reacquire: cover a full period (plus margin) so one window is
		// guaranteed to contain a frame if the stream is alive.
		return (uint32_t)s->t_est_ms + s->t_est_ms / 4u;
	return 2u * s->guard_ms + UCAP_SYNC_FRAME_MS;
}

// A valid frame arrived (its timestamp is the frame's *completion*).
// Returns the delay from now until the next window should open.
static uint32_t ucap_sync_on_frame(ucap_sync_t *s, uint32_t dt_ms)
{
	uint32_t lead;

	s->miss_streak = 0;
	if (dt_ms >= UCAP_SYNC_T_MIN_MS && dt_ms <= UCAP_SYNC_T_MAX_MS) {
		// Clean single period: train the per-device estimate.
		s->t_est_ms = (uint16_t)((int32_t)s->t_est_ms
				+ ((int32_t)dt_ms - (int32_t)s->t_est_ms) / 4);
		if (++s->hit_streak >= UCAP_SYNC_SHRINK_HITS) {
			s->hit_streak = 0;
			if (s->guard_ms / 2u >= UCAP_SYNC_GUARD_MIN_MS)
				s->guard_ms /= 2u;
			else
				s->guard_ms = UCAP_SYNC_GUARD_MIN_MS;
		}
	} else {
		// Off-cadence frame (boot, button-inserted extra frame, or first
		// catch after misses): keep it as the new phase anchor, but don't
		// train the estimate or narrow the window from it.
		s->hit_streak = 0;
	}

	// Next frame completes ~t_est after this one and starts ~FRAME_MS
	// before completing; open guard_ms before that start.
	lead = (uint32_t)s->guard_ms + UCAP_SYNC_FRAME_MS;
	if (lead >= s->t_est_ms)
		return 1; // degenerate: keep making progress rather than jam
	return (uint32_t)s->t_est_ms - lead;
}

// The window closed without a valid frame. Returns the delay until the
// next window opens (0 = reopen immediately); the window length must be
// re-read via ucap_sync_window_ms(), which this call widens.
static uint32_t ucap_sync_on_miss(ucap_sync_t *s)
{
	uint32_t old_guard = s->guard_ms;
	uint32_t lead;

	s->hit_streak = 0;
	if (s->miss_streak < 255u)
		s->miss_streak++;

	if (s->miss_streak >= UCAP_SYNC_MISS_BACKOFF)
		// Stream looks dead: low-duty full-period retries.
		return UCAP_SYNC_BACKOFF_MS;
	if (s->miss_streak >= UCAP_SYNC_MISS_REACQ)
		// Phase lost: reopen now with a full-period window.
		return 0;

	// Single miss: widen the guard and aim at the next period. "Now" is
	// the old window's close = prediction + old_guard; the next frame
	// starts ~(t_est - FRAME_MS) after the prediction.
	if (s->guard_ms * 2u <= UCAP_SYNC_GUARD_MAX_MS)
		s->guard_ms *= 2u;
	else
		s->guard_ms = UCAP_SYNC_GUARD_MAX_MS;
	lead = old_guard + (uint32_t)s->guard_ms + UCAP_SYNC_FRAME_MS;
	if (lead >= s->t_est_ms)
		return 1;
	return (uint32_t)s->t_est_ms - lead;
}

#endif // _UCAP_SYNC_H_
