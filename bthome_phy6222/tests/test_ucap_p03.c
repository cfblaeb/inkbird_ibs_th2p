/*
 * Host tests for source/ucap_p03.h (V26_P03 "P03 wake" receiver).
 * Build & run:  gcc -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -o test_ucap_p03 test_ucap_p03.c && ./test_ucap_p03
 * Exit code = number of failed checks.
 */
#include <stdio.h>
#include <string.h>
#include "../source/ucap_p03.h"

static int fails = 0, checks = 0;
#define CHECK(c, name) do { checks++; if (c) printf("ok   %s\n", name); else { fails++; printf("FAIL %s\n", name); } } while (0)

/* fake hardware: lock flag, timer armed flag, ISR latch */
typedef struct { int locked, timer, latch; } hw_t;
/* same execution order as p03_dispatch() in cmd_parser.c: stop -> unlock -> lock -> start -> latch reset */
static void apply(hw_t *h, uint32_t acts) {
	if (acts & P03_ACT_STOP_TO) h->timer = 0;
	if (acts & P03_ACT_UNLOCK) h->locked = 0;
	if (acts & P03_ACT_LOCK) h->locked = 1;
	if (acts & P03_ACT_START_TO) h->timer = 1;
	if (acts & P03_ACT_BURST_RST) h->latch = 0;
}
static int invariants(const ucap_p03_t *p, const hw_t *h) {
	if (p->st == P03_ST_ARMED) return h->locked && h->timer;
	if (p->st == P03_ST_IDLE) return !h->locked && !h->timer;
	if (p->st == P03_ST_SUSPENDED) return !h->locked && !h->timer;
	return 0;
}
static uint32_t ms2t(double ms) { return (uint32_t)(ms * 32.768 + 0.5); }
static void step(ucap_p03_t *p, hw_t *h, p03_event_t type, uint32_t tick, uint32_t t0, uint8_t ok, uint8_t src) {
	p03_ev_t e = { type, tick & P03_RTC_MASK, t0 & P03_RTC_MASK, ok, src }; p03_out_t o;
	p03_step(p, &e, &o); apply(h, o.acts);
	if (!invariants(p, h)) { fails++; printf("FAIL invariant after ev %d in st %d (locked %d timer %d)\n", type, p->st, h->locked, h->timer); }
}

int main(void) {
	ucap_p03_t p; hw_t h = {0, 0, 0};
	/* T1 init */
	p03_init(&p);
	CHECK(p.st == P03_ST_IDLE && p.lead_min == 0xFFFF && p.lead_ms_last == 255 && p.frames == 0, "T1 init");
	/* T2 arithmetic */
	CHECK(p03_dt(100, 0xFFFF00) == 356, "T2 24-bit wrap delta");
	CHECK(p03_ticks_to_100us(32768) == 10000, "T2 ticks->0.1ms (1 s)");
	CHECK(p03_ticks_to_ms(32768) == 1000, "T2 ticks->ms");
	/* T3 rise -> armed (lock + timer), frame at +4.2 ms lead -> idle, lead recorded */
	uint32_t t = 1000;
	step(&p, &h, P03_EV_RISE, t, t, 0, P03_SRC_IRQ);
	CHECK(p.st == P03_ST_ARMED && h.locked && h.timer && p.rises == 1, "T3 rise arms and locks");
	step(&p, &h, P03_EV_RX_START, t + ms2t(4.2), t + ms2t(4.2), 0, 0);
	CHECK(p.st == P03_ST_ARMED && p.have_first, "T3 first byte latched, still armed");
	step(&p, &h, P03_EV_FRAME, t + ms2t(4.2 + 18), t + ms2t(4.2 + 18), 1, 0);
	CHECK(p.st == P03_ST_IDLE && !h.locked && !h.timer && p.frames == 1 && p.frames_bad == 0, "T3 frame releases lock and timer");
	CHECK(p.lead_last >= 31 && p.lead_last <= 33 && p.lead_min == p.lead_last && p.lead_max == p.lead_last, "T3 lead = 4.2 ms stamp - 1.0 ms first-byte = 3.2 ms (0.1 ms units)");
	CHECK(p.lead_ms_last == 3, "T3 BTHome lead ms = 3");
	CHECK(p.hist[3] == 1, "T3 histogram bin 3-5 ms");
	CHECK(p.done_last >= 179 && p.done_last <= 181, "T3 frame duration 18 ms");
	/* T4 timeout: rise without frame */
	t += ms2t(10400);
	step(&p, &h, P03_EV_RISE, t, t, 0, P03_SRC_WAKE);
	step(&p, &h, P03_EV_TIMEOUT, t + ms2t(250), 0, 0, 0);
	CHECK(p.st == P03_ST_IDLE && !h.locked && p.timeouts == 1 && p.rises_wake == 1, "T4 timeout releases, counted, wake-source counted");
	/* T5 frame with no edge: rx-start locks, frame releases, frames_no_edge++ */
	t += ms2t(10400);
	step(&p, &h, P03_EV_RX_START, t, t, 0, 0);
	CHECK(p.st == P03_ST_ARMED && h.locked && p.rx_no_edge == 1, "T5 rx-start without edge arms");
	step(&p, &h, P03_EV_FRAME, t + ms2t(18), t + ms2t(18), 0, 0);
	CHECK(p.st == P03_ST_ARMED && h.locked && h.timer && p.frames == 2 && p.frames_bad == 1 && p.frames_bad_armed == 1 && p.frames_no_edge == 1, "T5 CRC-bad completion keeps the lock and re-arms the timeout");
	step(&p, &h, P03_EV_FRAME, t + ms2t(40), t + ms2t(40), 1, 0);
	CHECK(p.st == P03_ST_IDLE && !h.locked && p.frames == 3 && p.frames_no_edge == 2, "T5 following good frame releases");
	CHECK(p.lead_last >= 31 && p.lead_last <= 33, "T5 lead unchanged (no rise)");
	/* T6 stale events */
	uint16_t st0 = p.stale;
	step(&p, &h, P03_EV_TIMEOUT, t, 0, 0, 0);
	CHECK(p.stale == st0 + 1 && p.st == P03_ST_IDLE, "T6 timeout in IDLE is stale");
	/* T7 frame in IDLE (edge and rx-start missed): recorded, no lock */
	step(&p, &h, P03_EV_FRAME, t + ms2t(20000), t + ms2t(20000), 1, 0);
	CHECK(p.st == P03_ST_IDLE && !h.locked && p.frames == 4 && p.frames_no_edge == 3, "T7 frame in IDLE recorded, stays unlocked");
	/* T8 second rise while armed is counted stale-ish (rises++, stale++) and keeps first stamp */
	t += ms2t(10400);
	step(&p, &h, P03_EV_RISE, t, t, 0, P03_SRC_IRQ);
	uint16_t r = p.rises, s2 = p.stale;
	step(&p, &h, P03_EV_RISE, t + ms2t(1), t + ms2t(1), 0, P03_SRC_IRQ);
	CHECK(p.rises == r + 1 && p.stale == s2 + 1 && p.t_rise == (t & P03_RTC_MASK), "T8 duplicate rise keeps the first stamp");
	step(&p, &h, P03_EV_RX_START, t + ms2t(3), t + ms2t(3), 0, 0);
	step(&p, &h, P03_EV_FRAME, t + ms2t(21), t + ms2t(21), 1, 0);
	CHECK(p.st == P03_ST_IDLE && p.lead_last >= 19 && p.lead_last <= 21, "T8 lead 3.0-1.0 = 2.0 ms from first rise");
	/* T9 connect from ARMED: unlock, stop timer; disconnect -> idle */
	t += ms2t(10400);
	step(&p, &h, P03_EV_RISE, t, t, 0, P03_SRC_IRQ);
	step(&p, &h, P03_EV_CONNECT, t + ms2t(5), 0, 0, 0);
	CHECK(p.st == P03_ST_SUSPENDED && !h.locked && !h.timer && p.connects == 1, "T9 connect suspends and releases");
	step(&p, &h, P03_EV_CONNECT, t + ms2t(6), 0, 0, 0);
	CHECK(p.connects == 1 && p.stale > s2 + 1, "T9 double connect is stale");
	step(&p, &h, P03_EV_RX_START, t + ms2t(100), t + ms2t(100), 0, 0);
	step(&p, &h, P03_EV_FRAME, t + ms2t(118), t + ms2t(118), 1, 0);
	CHECK(p.st == P03_ST_SUSPENDED && !h.locked && p.frames == 6, "T9 frames during connection counted, no lock");
	step(&p, &h, P03_EV_DISCONNECT, t + ms2t(200), 0, 0, 0);
	CHECK(p.st == P03_ST_IDLE && p.resumes == 1, "T9 disconnect resumes");
	step(&p, &h, P03_EV_DISCONNECT, t + ms2t(201), 0, 0, 0);
	CHECK(p.st == P03_ST_IDLE && p.resumes == 1, "T9 disconnect without connect is a no-op");
	/* T10 recover from ARMED */
	t += ms2t(10400);
	step(&p, &h, P03_EV_RISE, t, t, 0, P03_SRC_IRQ);
	step(&p, &h, P03_EV_RECOVER, t + ms2t(3000), 0, 0, 0);
	CHECK(p.st == P03_ST_IDLE && !h.locked && !h.timer && p.recovers == 1, "T10 recover releases everything");
	/* T11 sanity: ARMED older than 2 s */
	t += ms2t(10400);
	step(&p, &h, P03_EV_RISE, t, t, 0, P03_SRC_IRQ);
	CHECK(p03_sanity_due(&p, t + ms2t(1000)) == 0, "T11 sanity quiet at 1 s");
	CHECK(p03_sanity_due(&p, t + ms2t(2500)) == 1 && p.sanity_recovers == 1, "T11 sanity fires at 2.5 s");
	step(&p, &h, P03_EV_RECOVER, t + ms2t(2500), 0, 0, 0);
	CHECK(p03_sanity_due(&p, t + ms2t(9000)) == 0, "T11 sanity quiet in IDLE");
	/* T12 RTC wrap across a frame */
	uint32_t w = 0xFFFFFF - ms2t(2);
	step(&p, &h, P03_EV_RISE, w, w, 0, P03_SRC_IRQ);
	step(&p, &h, P03_EV_RX_START, w + ms2t(5), w + ms2t(5), 0, 0);
	step(&p, &h, P03_EV_FRAME, w + ms2t(23), w + ms2t(23), 1, 0);
	CHECK(p.st == P03_ST_IDLE && p.lead_last >= 39 && p.lead_last <= 41, "T12 lead across the 24-bit wrap = 5.0-1.0 ms");
	/* T13 histogram bins and saturation */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		double leads[] = {1.5, 2.5, 3.5, 5, 7, 11, 16, 31};   /* stamp = lead + 1.0 ms first-byte offset */
		for (int i = 0; i < 8; i++) {
			uint32_t b = 5000 + i * ms2t(10400);
			step(&q, &g, P03_EV_RISE, b, b, 0, 0);
			step(&q, &g, P03_EV_RX_START, b + ms2t(leads[i]), b + ms2t(leads[i]), 0, 0);
			step(&q, &g, P03_EV_FRAME, b + ms2t(leads[i] + 18), b + ms2t(leads[i] + 18), 1, 0);
		}
		int ok = 1; for (int i = 0; i < 8; i++) ok &= (q.hist[i] == 1);
		CHECK(ok, "T13 one lead per histogram bin");
		CHECK(q.lead_min >= 4 && q.lead_min <= 6 && q.lead_max >= 299 && q.lead_max <= 301, "T13 min/max (0.5 .. 30 ms)");
		for (int i = 0; i < 300; i++) { uint32_t b = 900000 + i * ms2t(10400); step(&q, &g, P03_EV_RISE, b, b, 0, 0); step(&q, &g, P03_EV_RX_START, b + ms2t(4.2), b + ms2t(4.2), 0, 0); step(&q, &g, P03_EV_FRAME, b + ms2t(22), b + ms2t(22), 1, 0); }   /* stamp 4.2 -> lead 3.2 ms -> bin 3 */
		CHECK(q.hist[3] == 255, "T13 bin saturates at 255");
	}
	/* T14 sim: 1000 periods, lead 3 ms, 5 % frames lost (timeout), never locked outside ARMED */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q); int badinv = 0, exp_to = 0, exp_bad = 0;
		for (int i = 0; i < 1000; i++) {
			uint32_t b = 77 + i * ms2t(10400);
			step(&q, &g, P03_EV_RISE, b, b, 0, (i % 7 == 0) ? P03_SRC_WAKE : P03_SRC_IRQ);
			if (i % 20 == 0) { step(&q, &g, P03_EV_TIMEOUT, b + ms2t(250), 0, 0, 0); exp_to++; continue; }
			step(&q, &g, P03_EV_RX_START, b + ms2t(3), b + ms2t(3), 0, 0);
			if (i % 13 == 0) {                 /* CRC-bad completion: lock kept, timeout re-armed, then no more bytes */
				step(&q, &g, P03_EV_FRAME, b + ms2t(21), b + ms2t(21), 0, 0);
				if (!g.locked || !g.timer) badinv++;
				step(&q, &g, P03_EV_TIMEOUT, b + ms2t(271), 0, 0, 0); exp_to++; exp_bad++;
			} else {
				step(&q, &g, P03_EV_FRAME, b + ms2t(21), b + ms2t(21), 1, 0);
			}
			if (g.locked) badinv++;
		}
		CHECK(q.frames == 950 && q.timeouts == exp_to && q.frames_bad == exp_bad && q.frames_bad_armed == exp_bad && q.rises == 1000 && badinv == 0, "T14 sim counts, CRC-bad keeps lock until timeout, no lock leaks");
		CHECK(q.lead_min >= 19 && q.lead_max <= 21, "T14 sim leads all 2.0 ms (3.0 stamp - 1.0)");
	}

	/* T15 rise after rx-start (edge missed, then IRQ): not a lead anchor; no garbage */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		uint32_t b = 5000;
		step(&q, &g, P03_EV_RX_START, b, b, 0, 0);
		step(&q, &g, P03_EV_RISE, b + ms2t(2), b + ms2t(2), 0, P03_SRC_IRQ);
		step(&q, &g, P03_EV_FRAME, b + ms2t(18), b + ms2t(18), 1, 0);
		CHECK(q.st == P03_ST_IDLE && q.rise_after_rx == 1 && q.frames_no_edge == 1 && q.lead_min == 0xFFFF && q.lead_ms_last == 255, "T15 rise after rx-start is not a lead");
	}
	/* T16 conversion has no 32-bit overflow at the 24-bit maximum */
	CHECK(p03_ticks_to_100us(0xFFFFFF) == 5119999 || p03_ticks_to_100us(0xFFFFFF) == 5119998, "T16 ticks->0.1ms at 2^24-1 = 511999.9 ms");
	CHECK(p03_ticks_to_100us(6871948) > 2000000, "T16 no overflow above 6.87 M ticks");
	/* T17 implausible lead (rise stamp AFTER first byte via wrap) is rejected */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		uint32_t b = 5000;
		step(&q, &g, P03_EV_RISE, b + ms2t(400), b + ms2t(400), 0, P03_SRC_IRQ);   /* rise stamp later than the byte (bad stamp) */
		step(&q, &g, P03_EV_RX_START, b, b, 0, 0);
		step(&q, &g, P03_EV_FRAME, b + ms2t(18), b + ms2t(18), 1, 0);
		CHECK(q.lead_rejected == 1 && q.frames_no_edge == 1 && q.lead_min == 0xFFFF, "T17 lead > 500 ms rejected, not recorded");
	}
	/* T18 unknown-source wake locks but anchors no lead */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		uint32_t b = 9000;
		step(&q, &g, P03_EV_RISE, b, b, 0, P03_SRC_UNKNOWN);
		CHECK(q.st == P03_ST_ARMED && g.locked && g.timer && q.wakes_unknown == 1 && q.rises == 0 && !q.have_rise, "T18 unknown wake arms without a rise anchor");
		step(&q, &g, P03_EV_RX_START, b + ms2t(2), b + ms2t(2), 0, 0);
		step(&q, &g, P03_EV_FRAME, b + ms2t(18), b + ms2t(18), 1, 0);
		CHECK(q.st == P03_ST_IDLE && q.frames_no_edge == 1 && q.lead_min == 0xFFFF, "T18 frame after unknown wake: no lead");
	}
	/* T19 two frames within one ARMED period (second lands in IDLE) */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		uint32_t b = 1000;
		step(&q, &g, P03_EV_RISE, b, b, 0, 0); step(&q, &g, P03_EV_RX_START, b + ms2t(3), b + ms2t(3), 0, 0);
		step(&q, &g, P03_EV_FRAME, b + ms2t(21), b + ms2t(21), 1, 0);
		step(&q, &g, P03_EV_FRAME, b + ms2t(40), b + ms2t(40), 1, 0);
		CHECK(q.st == P03_ST_IDLE && !g.locked && q.frames == 2 && q.frames_no_edge == 1, "T19 second frame in IDLE counted, stays unlocked");
	}
	/* T20 CONNECT while ARMED with FRAME and TIMEOUT pending afterwards */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		uint32_t b = 1000;
		step(&q, &g, P03_EV_RISE, b, b, 0, 0);
		step(&q, &g, P03_EV_CONNECT, b + ms2t(1), 0, 0, 0);
		step(&q, &g, P03_EV_FRAME, b + ms2t(20), b + ms2t(20), 1, 0);
		step(&q, &g, P03_EV_TIMEOUT, b + ms2t(250), 0, 0, 0);
		CHECK(q.st == P03_ST_SUSPENDED && !g.locked && !g.timer && q.frames == 1, "T20 connect then pending frame/timeout: suspended, unlocked");
		step(&q, &g, P03_EV_RECOVER, b + ms2t(300), 0, 0, 0);
		CHECK(q.st == P03_ST_SUSPENDED && q.recovers == 1 && !g.locked, "T20 recover in SUSPENDED stays suspended");
		CHECK(p03_sanity_due(&q, b + ms2t(90000)) == 0, "T20 sanity quiet in SUSPENDED");
		step(&q, &g, P03_EV_DISCONNECT, b + ms2t(400), 0, 0, 0);
		step(&q, &g, P03_EV_RISE, b + ms2t(401), b + ms2t(401), 0, 0);
		CHECK(q.st == P03_ST_ARMED && g.locked && g.timer, "T20 rise right after disconnect arms");
	}
	/* T21 histogram bin edges: lead exactly 1.0 ms (stamp 2.0) -> bin 1 */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		uint32_t b = 1000;
		step(&q, &g, P03_EV_RISE, b, b, 0, 0); step(&q, &g, P03_EV_RX_START, b + ms2t(2.0), b + ms2t(2.0), 0, 0);
		step(&q, &g, P03_EV_FRAME, b + ms2t(20), b + ms2t(20), 1, 0);
		CHECK(q.hist[1] == 1 && q.hist[0] == 0 && q.lead_last >= 9 && q.lead_last <= 11, "T21 lead 1.0 ms lands in bin 1-2");
	}
	/* T22 sat16: timeouts saturate at 65535 */
	{
		ucap_p03_t q; hw_t g = {0,0,0}; p03_init(&q);
		for (uint32_t i = 0; i < 70000u; i++) { step(&q, &g, P03_EV_RISE, i * 100, i * 100, 0, 0); step(&q, &g, P03_EV_TIMEOUT, i * 100 + 50, 0, 0, 0); }
		CHECK(q.timeouts == 0xFFFF && q.rises == 0xFFFF && q.st == P03_ST_IDLE && !g.locked, "T22 counters saturate, still consistent");
	}
	printf("\n%d checks, %d failures\n", checks, fails);
	return fails;
}
