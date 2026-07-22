/* Host unit test for the V21 wake-on-RX scheduler (source/ucap_sync.h).
 *
 * Build & run:  gcc -Wall -Wextra -o test_ucap_sync test_ucap_sync.c && ./test_ucap_sync
 *
 * Covers the scheduler arithmetic (training, guard adaptation, miss
 * escalation, recovery) plus an end-to-end simulation against a fake main
 * MCU with a drifting clock, checking both catch rate and listen duty.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../source/ucap_sync.h"

static int failures = 0;

#define CHECK(cond, name) do { \
	if (cond) printf("ok   %s\n", name); \
	else { printf("FAIL %s\n", name); failures++; } \
} while (0)

static void test_init(void)
{
	ucap_sync_t s;
	ucap_sync_init(&s);
	CHECK(s.t_est_ms == UCAP_SYNC_T_INIT_MS && s.guard_ms == UCAP_SYNC_GUARD_INIT_MS
			&& s.hit_streak == 0 && s.miss_streak == 0, "init state");
	CHECK(ucap_sync_window_ms(&s) == 2u * UCAP_SYNC_GUARD_INIT_MS + UCAP_SYNC_FRAME_MS,
			"init window length");
}

static void test_training(void)
{
	ucap_sync_t s;
	int i;
	ucap_sync_init(&s);
	for (i = 0; i < 100; i++)
		ucap_sync_on_frame(&s, 10200);
	CHECK(s.t_est_ms >= 10195 && s.t_est_ms <= 10205, "t_est converges to device period");
	CHECK(s.guard_ms == UCAP_SYNC_GUARD_MIN_MS, "guard narrows to minimum on sustained hits");
	{
		uint32_t d = ucap_sync_on_frame(&s, 10200);
		CHECK(d == (uint32_t)s.t_est_ms - s.guard_ms - UCAP_SYNC_FRAME_MS,
				"open delay = period - guard - frame time");
	}
}

static void test_off_cadence(void)
{
	ucap_sync_t s;
	uint16_t t0;
	ucap_sync_init(&s);
	ucap_sync_on_frame(&s, 10100);
	t0 = s.t_est_ms;
	ucap_sync_on_frame(&s, 5000);                  /* button-inserted extra frame */
	CHECK(s.t_est_ms == t0, "short dt does not train the estimate");
	ucap_sync_on_frame(&s, 20800);                 /* a missed frame in between */
	CHECK(s.t_est_ms == t0, "long dt does not train the estimate");
	ucap_sync_on_frame(&s, UCAP_SYNC_DT_UNKNOWN);  /* boot / post-miss */
	CHECK(s.t_est_ms == t0, "unknown dt does not train the estimate");
	CHECK(s.hit_streak == 0, "off-cadence frames reset the hit streak");
}

static void test_miss_escalation(void)
{
	ucap_sync_t s;
	uint32_t d;
	ucap_sync_init(&s);
	ucap_sync_on_frame(&s, 10390);

	d = ucap_sync_on_miss(&s);                     /* miss #1: widen, next period */
	CHECK(s.guard_ms == 2u * UCAP_SYNC_GUARD_INIT_MS, "first miss doubles the guard");
	CHECK(d > 0 && d < s.t_est_ms, "first miss re-aims at the next period");

	d = ucap_sync_on_miss(&s);                     /* miss #2: reacquire */
	CHECK(d == 0, "second miss reopens immediately");
	CHECK(ucap_sync_window_ms(&s) == (uint32_t)s.t_est_ms + s.t_est_ms / 4u,
			"reacquire window spans a full period");

	while (s.miss_streak < UCAP_SYNC_MISS_BACKOFF)
		d = ucap_sync_on_miss(&s);
	CHECK(d == UCAP_SYNC_BACKOFF_MS, "sustained misses back off to low duty");
	CHECK(ucap_sync_window_ms(&s) > s.t_est_ms, "backoff retries keep the full window");

	ucap_sync_on_frame(&s, UCAP_SYNC_DT_UNKNOWN);  /* stream is back */
	CHECK(s.miss_streak == 0, "a frame clears the miss streak");
	CHECK(ucap_sync_window_ms(&s) == 2u * s.guard_ms + UCAP_SYNC_FRAME_MS,
			"window returns to guard-based after recovery");
}

static void test_bounds(void)
{
	ucap_sync_t s;
	int i, zero_opens = 0;
	ucap_sync_init(&s);
	for (i = 0; i < 20; i++)
		ucap_sync_on_miss(&s);
	CHECK(s.guard_ms <= UCAP_SYNC_GUARD_MAX_MS, "guard never exceeds max");
	ucap_sync_init(&s);
	for (i = 0; i < 1000; i++)
		if (ucap_sync_on_frame(&s, 10390) == 0)
			zero_opens++;
	CHECK(zero_opens == 0, "on_frame always returns a positive delay");
	CHECK(s.guard_ms >= UCAP_SYNC_GUARD_MIN_MS, "guard never drops below min");
}

/* End-to-end simulation: a fake main MCU whose period drifts across the
 * measured envelope (10340..10410 ms) with +-1 ms jitter, against the real
 * open/close/miss flow the device glue implements. Verifies the catch rate
 * after acquisition and the listen duty cycle. Timestamps of caught frames
 * are frame *starts*; the scheduler sees completion-relative values, which
 * only shifts everything by the constant UCAP_SYNC_FRAME_MS. */
static uint32_t sim_emit(uint32_t *period, int *dir, int *frames)
{
	uint32_t p = *period;
	*period += *dir;
	if (*period >= 10410) *dir = -1;
	if (*period <= 10340) *dir = 1;
	(*frames)++;
	return p;
}

static void test_simulation(void)
{
	ucap_sync_t s;
	uint32_t frame_t = 500;                 /* start time of the next frame */
	uint32_t window_open = 0, window_len = 15000;  /* boot acquisition */
	uint32_t period = 10340, end = 0;
	int dir = 1;
	int frames = 0, caught = 0, missed = 0;
	uint64_t listen_ms = 0;
	uint32_t last_caught = 0;
	int have_prev = 0;

	ucap_sync_init(&s);
	while (frames < 2000) {
		uint32_t window_close = window_open + window_len;
		/* frames emitted while the receiver was off are unobserved */
		while (frame_t + UCAP_SYNC_FRAME_MS < window_open)
			frame_t += sim_emit(&period, &dir, &frames);

		if (frame_t >= window_open && frame_t + UCAP_SYNC_FRAME_MS <= window_close) {
			/* caught: window closes at frame completion (early release) */
			uint32_t done = frame_t + UCAP_SYNC_FRAME_MS;
			uint32_t dt = have_prev ? frame_t - last_caught : UCAP_SYNC_DT_UNKNOWN;
			listen_ms += done - window_open;
			last_caught = frame_t;
			have_prev = 1;
			caught++;
			window_open = done + ucap_sync_on_frame(&s, dt);
			window_len = ucap_sync_window_ms(&s);
			end = done;
			frame_t += sim_emit(&period, &dir, &frames);
		} else {
			/* window closes empty */
			uint32_t d;
			listen_ms += window_len;
			have_prev = 0;
			missed++;
			d = ucap_sync_on_miss(&s);
			/* a frame straddling the close is destroyed by the deinit */
			if (frame_t < window_close)
				frame_t += sim_emit(&period, &dir, &frames);
			window_open = window_close + d;
			window_len = ucap_sync_window_ms(&s);
			end = window_close;
		}
	}

	printf("     sim: %d frames emitted, %d caught, %d missed windows, "
			"duty %.2f%%\n", frames, caught, missed,
			100.0 * (double)listen_ms / (double)end);
	CHECK(caught >= frames - 3, "simulation: catches essentially every frame");
	CHECK(missed <= 3, "simulation: no repeated misses under measured drift");
	CHECK((double)listen_ms / (double)end < 0.02, "simulation: listen duty under 2%");
}

int main(void)
{
	test_init();
	test_training();
	test_off_cadence();
	test_miss_escalation();
	test_bounds();
	test_simulation();
	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
