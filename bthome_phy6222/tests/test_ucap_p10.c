/* Host test for the V25_P10 "P10 alone" scheduler (source/ucap_p10.h).
 *
 * Build & run (gcc 15 defaults to C23; be explicit):
 *   gcc -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -o test_ucap_p10 test_ucap_p10.c && ./test_ucap_p10
 *
 * Exit code = number of failed checks. Cases T1..T28 follow SPEC_V25_P10 section 10; T29/T30 pin
 * the two review fixes (T_MAX < 2*T_MIN so a double period can never train; guard ladder 100/200/400).
 *
 * Harness:
 *  (a) Fake hardware model: uart_on, uart_locked (MOD_UART0), usr1_locked (MOD_USR1),
 *      gpio_armed, the three one-shot timers as absolute deadlines and the ISR burst
 *      counter (p10_hw.n/t0/src). p10_out_t.acts are applied in bit order exactly like
 *      p10_exec() in cmd_parser.c. After EVERY step the model asserts (spec 3.1/3.3):
 *        WINDOW    <=> uart_on && uart_locked && !usr1_locked
 *        VERIFY    <=> usr1_locked && !uart_on && gpio_armed
 *        SUSPENDED <=> uart_on && !uart_locked && !usr1_locked
 *        WAIT_*    <=> !uart_on && gpio_armed && !usr1_locked && !uart_locked
 *        VERIFY timer armed <=> VERIFY; OPEN timer armed <=> WAIT_OPEN or VERIFY(from WAIT_OPEN);
 *        CLOSE timer armed <=> WINDOW; UART_ON_* never while uart_on; never two UART_ON_*
 *        without an UART_OFF between; GPIO_ARM never without UART_OFF in the same step;
 *        START_OPEN only from VERIFY_TO(n >= 4) in VERIFY entered from WAIT_EDGE;
 *        windows == hits + misses + win_aborted + windows lost to RECOVER (+1 in WINDOW).
 *  (b) Frame generator: periodic frames with a per-frame period T(i) (drift model),
 *      +-jitter, optional boot pair, button-inserted frames, glitch bursts (1-3 edges),
 *      per-frame CRC-bad flag.
 *  (c) Wire model driven by the hardware model's state: edges reach the GPIO IRQ only
 *      while the pin is a GPIO input (WAIT_EDGE/VERIFY/WAIT_OPEN). A frame is the start
 *      edge + 12 more falling edges spread over 13.5 ms. When the chip is asleep the
 *      first edge wakes it; the IRQ is live wake_lat (1-3 ms) later (edges in between
 *      are lost) and the burst stamp t0 is the true start edge + stamp_off (the wake
 *      stamp g_wakeup_rtc_tick, src = 1, or a late IRQ stamp, src = 0). Task-context
 *      dispatch starts when the wake is complete. A frame is received iff the UART is on
 *      from its start edge until the ISR completes at start + 18 ms (FRAME delivered
 *      then); a frame straddling window open or close leaves bytes in the framer
 *      (CLOSE with n = 1). Pending OSAL events are dispatched in the priority order of
 *      thb2_main.c: RECOVER, FRAME, CLOSE, VERIFY, OPEN, EDGE. The sanity sweep runs
 *      every 10 s like adv_measure().
 *      RTC: ticks = true_time * 32.768/ms / (1 + e) with e the RC32K PERIOD error and
 *      ct = 7812.5 * (1 + e) (spec 10 writes both factors as (1+e); the header's
 *      correction sign requires a larger ct to mean a slower RC, so e is applied to the
 *      period here — |e| = 2 % runs are done with both signs). OSAL deadlines are
 *      crystal-true. ct can be forced (7812 = no correction, 3906 = unconverged average).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "../source/ucap_p10.h"

static int failures = 0;

#define CHECK(cond, name) do { \
	if (cond) printf("ok   %s\n", name); \
	else { printf("FAIL %s\n", name); failures++; } \
} while (0)

#define INF INT64_MAX
#define EVB(t) (1u << (t))

/* ------------------------------------------------------------------ helpers */
static uint32_t xs32(uint32_t *s)
{	uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
static int64_t rnd_range(uint32_t *s, int64_t lo, int64_t hi)
{	if (hi <= lo) return lo; return lo + (int64_t)(xs32(s) % (uint32_t)(hi - lo + 1)); }
static uint32_t MS(uint32_t ms) { return p10_ms_to_ticks(ms) & P10_RTC_MASK; }
static uint32_t absd(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* ------------------------------------------------------------------ (a) hardware model */
enum { TM_V = 0, TM_O, TM_C };

typedef struct {
	int uart_on, uart_locked, usr1_locked, gpio_armed;
	int64_t timer[3];          /* absolute deadline in us, -1 = not armed */
	int64_t uart_on_since, uart_on_us;
	int fr_bytes;              /* framer holds bytes of an incomplete frame (ucap.fr.in_frame) */
	uint8_t b_n, b_src; uint32_t b_t0;   /* ISR burst counter (p10_hw) */
	int open_armed;            /* START_OPEN executed and its OPEN not yet dispatched */
	int on_since_off;          /* an UART_ON_* executed without an UART_OFF since */
} hw_t;

typedef struct {
	ucap_p10_t p; hw_t hw;
	int64_t now;               /* us, for timer deadlines */
	uint32_t ct;               /* ct handed to every event by the direct-step helper */
	int inv_fails, lost_windows;
	int seen_st[5];
} mach_t;

static void m_init(mach_t *m)
{
	memset(m, 0, sizeof *m);
	p10_init(&m->p);
	m->hw.gpio_armed = 1;                      /* ucap_init() arms P10 before the first event */
	m->hw.timer[0] = m->hw.timer[1] = m->hw.timer[2] = -1;
	m->ct = 7812;
	m->seen_st[P10_ST_WAIT_EDGE] = 1;
}

static void inv_fail(mach_t *m, const char *what)
{
	if (m->inv_fails < 8) printf("  ! invariant: %s (st=%u from=%u)\n", what, m->p.st, m->p.verify_from);
	m->inv_fails++;
}

/* p10_exec() mirror: bit order == execution order */
static void hw_apply(mach_t *m, const p10_out_t *o, const p10_ev_t *ev, uint8_t prev_st, uint8_t prev_from)
{
	uint32_t a = o->acts; hw_t *h = &m->hw;
	if ((a & P10_ACT_GPIO_ARM) && !(a & P10_ACT_UART_OFF)) inv_fail(m, "GPIO_ARM without UART_OFF");
	if ((a & P10_ACT_UART_ON_FREE) && (a & P10_ACT_UART_ON_LOCK)) inv_fail(m, "UART_ON_FREE and UART_ON_LOCK together");
	if ((a & P10_ACT_START_OPEN) &&
	    !(ev->type == P10_EV_VERIFY_TO && prev_st == P10_ST_VERIFY && prev_from == P10_ST_WAIT_EDGE
	      && ev->n >= P10_VERIFY_MIN_EDGES))
		inv_fail(m, "START_OPEN not from VERIFY_TO(from WAIT_EDGE, n>=4)");
	if ((a & P10_ACT_START_OPEN) && o->open_ms < P10_MIN_TIMER_MS) inv_fail(m, "open_ms below P10_MIN_TIMER_MS");
	if ((a & P10_ACT_START_CLOSE) && o->close_ms != p10_window_ms(&m->p)) inv_fail(m, "close_ms != p10_window_ms");

	if (a & P10_ACT_STOP_VERIFY)   h->timer[TM_V] = -1;
	if (a & P10_ACT_STOP_OPEN)     h->timer[TM_O] = -1;
	if (a & P10_ACT_STOP_CLOSE)    h->timer[TM_C] = -1;
	if (a & P10_ACT_UNLOCK_VERIFY) h->usr1_locked = 0;
	if (a & P10_ACT_UART_OFF) {
		h->uart_locked = 0;                      /* unlock BEFORE deinit */
		if (h->uart_on) { h->uart_on = 0; h->uart_on_us += m->now - h->uart_on_since; }
		h->on_since_off = 0;
	}
	if (a & P10_ACT_GPIO_ARM)      h->gpio_armed = 1;
	if (a & (P10_ACT_UART_ON_FREE | P10_ACT_UART_ON_LOCK)) {
		if (h->uart_on) inv_fail(m, "UART_ON_* while the UART is on");
		if (h->on_since_off) inv_fail(m, "two UART_ON_* without UART_OFF");
		h->on_since_off = 1;
		h->uart_on = 1; h->uart_on_since = m->now;
		h->gpio_armed = 0;                       /* fmux -> hal_gpioin_disable */
		h->fr_bytes = 0;                         /* framer reset at window open (F1) */
		if (a & P10_ACT_UART_ON_LOCK) h->uart_locked = 1;   /* init BEFORE lock */
	}
	if (a & P10_ACT_LOCK_VERIFY)   h->usr1_locked = 1;
	if (a & P10_ACT_START_VERIFY)  h->timer[TM_V] = m->now + (int64_t)P10_VERIFY_MS * 1000;
	if (a & P10_ACT_START_OPEN)  { h->timer[TM_O] = m->now + (int64_t)o->open_ms * 1000; h->open_armed = 1; }
	if (a & P10_ACT_START_CLOSE)   h->timer[TM_C] = m->now + (int64_t)o->close_ms * 1000;
	if (a & P10_ACT_ISR_RESET)     h->b_n = 0;
}

static void hw_check(mach_t *m)
{
	const hw_t *h = &m->hw; const ucap_p10_t *p = &m->p;
	int ok, open_expected;
	switch (p->st) {
	case P10_ST_WINDOW:    ok = h->uart_on && h->uart_locked && !h->usr1_locked; break;
	case P10_ST_VERIFY:    ok = h->usr1_locked && !h->uart_on && !h->uart_locked && h->gpio_armed; break;
	case P10_ST_SUSPENDED: ok = h->uart_on && !h->uart_locked && !h->usr1_locked; break;
	case P10_ST_WAIT_EDGE:
	case P10_ST_WAIT_OPEN: ok = !h->uart_on && h->gpio_armed && !h->usr1_locked && !h->uart_locked; break;
	default: ok = 0;
	}
	if (!ok) inv_fail(m, "state <-> locks/pin mismatch");
	if ((h->timer[TM_V] >= 0) != (p->st == P10_ST_VERIFY)) inv_fail(m, "VERIFY timer armed <-> VERIFY");
	open_expected = p->st == P10_ST_WAIT_OPEN || (p->st == P10_ST_VERIFY && p->verify_from == P10_ST_WAIT_OPEN);
	if ((h->timer[TM_O] >= 0) != open_expected) inv_fail(m, "OPEN timer armed <-> WAIT_OPEN / VERIFY(from WAIT_OPEN)");
	if ((h->timer[TM_C] >= 0) != (p->st == P10_ST_WINDOW)) inv_fail(m, "CLOSE timer armed <-> WINDOW");
	if ((uint16_t)(p->hits + p->misses + p->win_aborted + m->lost_windows + (p->st == P10_ST_WINDOW ? 1 : 0)) != p->windows)
		inv_fail(m, "windows != hits + misses + aborted + lost (+1 in WINDOW)");
	if (p->guard_ms != P10_GUARD_BASE_MS && p->guard_ms != 2u * P10_GUARD_BASE_MS && p->guard_ms != P10_GUARD_MAX_MS)
		inv_fail(m, "guard not 100/200/400");
	if (p->guard_ms > P10_GUARD_BASE_MS && p->miss_streak < P10_MISS_WIDEN) inv_fail(m, "guard widened without a miss streak");
	if (p->t_est_ms < P10_T_MIN_MS || p->t_est_ms > P10_T_MAX_MS) inv_fail(m, "t_est out of bounds");
	if (p->hist_n > P10_HIST_N || p->health > 100) inv_fail(m, "hist/health out of range");
	if (p->st == P10_ST_VERIFY && p->verify_from != P10_ST_WAIT_EDGE && p->verify_from != P10_ST_WAIT_OPEN)
		inv_fail(m, "verify_from not WAIT_EDGE/WAIT_OPEN");
}

static void m_step(mach_t *m, p10_ev_t *ev, p10_out_t *o)
{
	uint8_t prev_st = m->p.st, prev_from = m->p.verify_from;
	hw_t *h = &m->hw;
	/* a delivered timer event means that timer has expired in the model */
	if (ev->type == P10_EV_VERIFY_TO) h->timer[TM_V] = -1;
	if (ev->type == P10_EV_OPEN)      h->timer[TM_O] = -1;
	if (ev->type == P10_EV_CLOSE)     h->timer[TM_C] = -1;
	if (ev->type == P10_EV_RECOVER && prev_st == P10_ST_WINDOW) m->lost_windows++;
	p10_step(&m->p, ev, o);
	if (m->p.st < 5) m->seen_st[m->p.st] = 1;
	hw_apply(m, o, ev, prev_st, prev_from);
	hw_check(m);
}

/* direct-step helper: times in ms (RTC tick = MS(ms), sec = ms/1000, now = ms*1000 us) */
static p10_out_t go(mach_t *m, p10_event_t type, uint32_t ms, uint32_t t0_ms, uint8_t n, uint8_t ok)
{
	p10_ev_t ev; p10_out_t o;
	memset(&ev, 0, sizeof ev);
	ev.type = type; ev.tick = MS(ms); ev.sec = ms / 1000u; ev.t0 = MS(t0_ms);
	ev.n = n; ev.ok = ok; ev.ct = m->ct;
	m->now = (int64_t)ms * 1000;
	m_step(m, &ev, &o);
	return o;
}
static uint32_t tmr_ms(const mach_t *m, int k) { return (uint32_t)(m->hw.timer[k] / 1000); }
static p10_out_t fire_verify(mach_t *m, uint32_t t0_ms, uint8_t n)
{	return go(m, P10_EV_VERIFY_TO, tmr_ms(m, TM_V), t0_ms, n, 0); }
static p10_out_t fire_open(mach_t *m, uint32_t t0_ms, uint8_t n)
{	return go(m, P10_EV_OPEN, tmr_ms(m, TM_O), t0_ms, n, 0); }
static p10_out_t fire_close(mach_t *m, uint8_t n)
{	return go(m, P10_EV_CLOSE, tmr_ms(m, TM_C), 0, n, 0); }

enum { R_WE = 0, R_VERIFY_WE, R_VERIFY_WO, R_WO, R_WINDOW, R_SUSP, R_COUNT };
static const char *r_name[R_COUNT] = { "WAIT_EDGE", "VERIFY(from WE)", "VERIFY(from WO)", "WAIT_OPEN", "WINDOW", "SUSPENDED" };

static void reach(mach_t *m, int which, uint32_t t)
{
	m_init(m);
	switch (which) {
	case R_WE: break;
	case R_VERIFY_WE: go(m, P10_EV_EDGE, t, t, 0, 0); break;
	case R_WO: go(m, P10_EV_EDGE, t, t, 0, 0); fire_verify(m, t, 12); break;
	case R_VERIFY_WO: go(m, P10_EV_EDGE, t, t, 0, 0); fire_verify(m, t, 12); go(m, P10_EV_EDGE, t + 5000, t + 5000, 0, 0); break;
	case R_WINDOW: go(m, P10_EV_EDGE, t, t, 0, 0); fire_verify(m, t, 12); fire_open(m, t, 0); break;
	case R_SUSP: go(m, P10_EV_CONNECT, t, 0, 0, 0); break;
	}
}

/* ------------------------------------------------------------------ (b)+(c) simulator */
typedef struct { int64_t start; uint8_t ok, button; } frame_t;
typedef struct { int64_t t; int fidx; int k; int seq; } wev_t;   /* fidx >= 0: frame edge k (13 = ISR done); < 0: glitch edge */

#define SEQ_MAX 8192
typedef struct {
	/* config */
	int32_t rc_ppm;            /* RC32K period error, ppm */
	uint32_t ct_forced;        /* 0 = derive from rc_ppm */
	uint32_t rtc_start;
	uint8_t src;               /* EDGE src reported for wake stamps */
	int64_t stamp_lo, stamp_hi;/* t0 stamp offset from the true start edge, us */
	int64_t lat_lo, lat_hi;    /* IRQ live after an IO wake, us */
	int64_t end_us;            /* 0 = last wire event + 100 ms */
	uint32_t seed;
	frame_t *fr; int nfr, capfr;
	int64_t *gl_t; int *gl_n; int ngl, capgl;
	wev_t *wev; int nwev;
	/* runtime */
	mach_t m; uint32_t rng;
	int64_t now, wake_done_at, awake_since, awake_us; int awake;
	uint32_t pending, frame_tick; uint8_t frame_ok;
	/* stats */
	int hits, misses, windows, frames_rx, wakes_io, miss_run, max_miss_run;
	int misses_before_first_hit, guard200_seen, guard400_seen, sanity_fired, win100, miss100, t_src2_seen;
	uint16_t seed_t_est;       /* 0 = P10_T_SEED_MS; else t_est_ms forced after p10_init (T29) */
	int64_t first_hit_us; uint8_t cur_win_guard;
	char seq[SEQ_MAX + 1]; int seqlen;
} sim_t;

static void sim_init(sim_t *s, uint32_t seed)
{
	memset(s, 0, sizeof *s);
	s->src = 1; s->stamp_lo = s->stamp_hi = -600;      /* wake stamp ~0.6 ms off the edge */
	s->lat_lo = 1000; s->lat_hi = 3000;
	s->seed = seed; s->first_hit_us = -1;
}
static void sim_free(sim_t *s) { free(s->fr); free(s->gl_t); free(s->gl_n); free(s->wev); }

static void sim_add_frame(sim_t *s, int64_t t, uint8_t ok, uint8_t button)
{
	if (s->nfr == s->capfr) { s->capfr = s->capfr ? 2 * s->capfr : 64; s->fr = realloc(s->fr, sizeof(frame_t) * (size_t)s->capfr); }
	s->fr[s->nfr].start = t; s->fr[s->nfr].ok = ok; s->fr[s->nfr].button = button; s->nfr++;
}
static void sim_add_glitch(sim_t *s, int64_t t, int n)
{
	if (s->ngl == s->capgl) {
		s->capgl = s->capgl ? 2 * s->capgl : 64;
		s->gl_t = realloc(s->gl_t, sizeof(int64_t) * (size_t)s->capgl);
		s->gl_n = realloc(s->gl_n, sizeof(int) * (size_t)s->capgl);
	}
	s->gl_t[s->ngl] = t; s->gl_n[s->ngl] = n; s->ngl++;
}
typedef int64_t (*period_fn)(int i, void *ctx);
static int64_t period_const(int i, void *ctx) { (void)i; return *(const int64_t *)ctx; }
/* T sweeps 10340 -> 10410 -> 10340 ms over n frames */
static int64_t period_tri(int i, void *ctx)
{	int n = *(const int *)ctx; int64_t half = n / 2, d = i < half ? i : n - i; return 10340000 + 70000 * d / half; }

static void sim_add_periodic(sim_t *s, uint32_t *rng, int64_t first, int n, period_fn fn, void *ctx, int64_t jitter_us)
{
	int64_t base = first; int i;
	for (i = 0; i < n; i++) { sim_add_frame(s, base + rnd_range(rng, -jitter_us, jitter_us), 1, 0); base += fn(i, ctx); }
}
static int64_t sim_last_frame(const sim_t *s)
{	int64_t t = 0; int i; for (i = 0; i < s->nfr; i++) if (s->fr[i].start > t) t = s->fr[i].start; return t; }

static int wev_cmp(const void *a, const void *b)
{
	const wev_t *x = a, *y = b;
	if (x->t != y->t) return x->t < y->t ? -1 : 1;
	return x->seq - y->seq;
}
static void sim_build(sim_t *s)
{
	int i, k, n = 0, total = s->nfr * 14;
	for (i = 0; i < s->ngl; i++) total += s->gl_n[i];
	s->wev = malloc(sizeof(wev_t) * (size_t)(total ? total : 1));
	for (i = 0; i < s->nfr; i++) {
		for (k = 0; k < 13; k++) { s->wev[n].t = s->fr[i].start + k * 1125; s->wev[n].fidx = i; s->wev[n].k = k; s->wev[n].seq = n; n++; }
		s->wev[n].t = s->fr[i].start + 18000; s->wev[n].fidx = i; s->wev[n].k = 13; s->wev[n].seq = n; n++;
	}
	for (i = 0; i < s->ngl; i++)
		for (k = 0; k < s->gl_n[i]; k++) { s->wev[n].t = s->gl_t[i] + k * 500; s->wev[n].fidx = -1; s->wev[n].k = k; s->wev[n].seq = n; n++; }
	s->nwev = n;
	qsort(s->wev, (size_t)n, sizeof(wev_t), wev_cmp);
}

static uint32_t s_rtc(const sim_t *s, int64_t t_us)
{
	double ticks = (double)t_us * 32.768e-3 * 1e6 / (1e6 + (double)s->rc_ppm);
	int64_t r = ticks >= 0 ? (int64_t)(ticks + 0.5) : -(int64_t)(-ticks + 0.5);
	return (uint32_t)(((uint64_t)(r + (int64_t)s->rtc_start)) & P10_RTC_MASK);
}
static uint32_t s_ct(const sim_t *s)
{	return s->ct_forced ? s->ct_forced : (uint32_t)(7812.5 * (1e6 + (double)s->rc_ppm) / 1e6 + 0.5); }

static void s_wake(sim_t *s) { if (!s->awake) { s->awake = 1; s->awake_since = s->now; } }
static void s_maybe_sleep(sim_t *s)
{
	const hw_t *h = &s->m.hw;
	if (s->awake && !h->usr1_locked && !h->uart_locked && !h->uart_on && !s->pending && s->now >= s->wake_done_at) {
		s->awake_us += s->now - s->awake_since; s->awake = 0;
	}
}

static void on_edge(sim_t *s, const wev_t *w)
{
	hw_t *h = &s->m.hw;
	if (!h->gpio_armed) {                                   /* pin on UART: bytes, not edges */
		if (h->uart_on && w->fidx >= 0 && w->k > 0) h->fr_bytes = 1;
		return;
	}
	if (!s->awake) {                                        /* IO wake */
		s_wake(s);
		s->wake_done_at = s->now + rnd_range(&s->rng, s->lat_lo, s->lat_hi);
		s->wakes_io++;
		if (h->b_n == 0) {                                  /* wake hook / synthesised callback */
			h->b_t0 = s_rtc(s, s->now + rnd_range(&s->rng, s->stamp_lo, s->stamp_hi));
			h->b_src = s->src; h->b_n = 1;
			s->pending |= EVB(P10_EV_EDGE);
		}
		return;
	}
	if (s->now < s->wake_done_at) return;                   /* IRQ not re-enabled yet: edge lost */
	if (h->b_n == 0) { h->b_t0 = s_rtc(s, s->now); h->b_src = 0; s->pending |= EVB(P10_EV_EDGE); }
	if (h->b_n < 255) h->b_n++;
}
static void on_frame_done(sim_t *s, const wev_t *w)
{
	hw_t *h = &s->m.hw; const frame_t *f = &s->fr[w->fidx];
	if (h->uart_on && h->uart_on_since <= f->start) {       /* complete frame in the UART ISR */
		s->frame_tick = s_rtc(s, s->now); s->frame_ok = f->ok;
		s->pending |= EVB(P10_EV_FRAME); h->fr_bytes = 0; s->frames_rx++;
	}
}

static void s_dispatch(sim_t *s)
{
	while (s->pending && s->now >= s->wake_done_at) {
		p10_ev_t ev; p10_out_t o; ucap_p10_t *p = &s->m.p; hw_t *h = &s->m.hw;
		uint16_t ph = p->hits, pm = p->misses, pw = p->windows;
		memset(&ev, 0, sizeof ev);
		ev.tick = s_rtc(s, s->now); ev.sec = (uint32_t)(s->now / 1000000); ev.ct = s_ct(s);
		if (s->pending & EVB(P10_EV_RECOVER))        { ev.type = P10_EV_RECOVER; }
		else if (s->pending & EVB(P10_EV_FRAME))     { ev.type = P10_EV_FRAME; ev.tick = s->frame_tick; ev.ok = s->frame_ok; }
		else if (s->pending & EVB(P10_EV_CLOSE))     { ev.type = P10_EV_CLOSE; ev.n = h->fr_bytes ? 1 : 0; }
		else if (s->pending & EVB(P10_EV_VERIFY_TO)) { ev.type = P10_EV_VERIFY_TO; ev.n = h->b_n; ev.t0 = h->b_t0; }
		else if (s->pending & EVB(P10_EV_OPEN))      { ev.type = P10_EV_OPEN; ev.n = h->b_n; ev.t0 = h->b_t0; }
		else                                         { ev.type = P10_EV_EDGE; ev.t0 = h->b_t0; ev.src = h->b_src; }
		s->pending &= ~EVB(ev.type);
		s->m.now = s->now;
		m_step(&s->m, &ev, &o);
		if (p->windows != pw) {
			s->windows++;
			if (!(ev.type == P10_EV_OPEN && h->open_armed)) inv_fail(&s->m, "window not opened by a START_OPEN-armed OPEN");
			h->open_armed = 0;
			s->cur_win_guard = (uint8_t)(p->guard_ms / 10);
			if (p->guard_ms == P10_GUARD_BASE_MS) s->win100++;
		}
		if (p->hits != ph) {
			s->hits++; s->miss_run = 0;
			if (s->first_hit_us < 0) { s->first_hit_us = s->now; s->misses_before_first_hit = s->misses; }
			if (s->seqlen < SEQ_MAX) s->seq[s->seqlen++] = 'H';
		}
		if (p->misses != pm) {
			s->misses++; s->miss_run++;
			if (s->miss_run > s->max_miss_run) s->max_miss_run = s->miss_run;
			if (s->cur_win_guard == P10_GUARD_BASE_MS / 10) s->miss100++;
			if (s->seqlen < SEQ_MAX) s->seq[s->seqlen++] = 'M';
		}
		if (p->guard_ms == 2u * P10_GUARD_BASE_MS) s->guard200_seen = 1;
		if (p->guard_ms == P10_GUARD_MAX_MS) s->guard400_seen = 1;
		if (p->t_src == 2 && s->first_hit_us < 0) s->t_src2_seen = 1;
	}
}

static void sim_run(sim_t *s)
{
	int i = 0, k; int64_t adv_at = 10000000;
	sim_build(s);
	m_init(&s->m);
	if (s->seed_t_est) s->m.p.t_est_ms = s->seed_t_est;
	s->rng = s->seed ? s->seed : 1;
	s->now = 0; s->wake_done_at = 0; s->awake = 0; s->awake_us = 0; s->pending = 0;
	if (!s->end_us) s->end_us = (s->nwev ? s->wev[s->nwev - 1].t : 0) + 100000;
	for (;;) {
		int64_t next = INF; hw_t *h = &s->m.hw;
		if (i < s->nwev) next = s->wev[i].t;
		for (k = 0; k < 3; k++) if (h->timer[k] >= 0 && h->timer[k] < next) next = h->timer[k];
		if (s->pending && s->wake_done_at < next) next = s->wake_done_at;
		if (adv_at < next) next = adv_at;
		if (next == INF || next > s->end_us) break;
		s->now = next;
		while (i < s->nwev && s->wev[i].t == s->now) {
			if (s->wev[i].fidx >= 0 && s->wev[i].k == 13) on_frame_done(s, &s->wev[i]);
			else on_edge(s, &s->wev[i]);
			i++;
		}
		for (k = 0; k < 3; k++) if (h->timer[k] == s->now) {   /* RTC wake, crystal-true */
			h->timer[k] = -1; s_wake(s);
			s->pending |= EVB(k == TM_V ? P10_EV_VERIFY_TO : k == TM_O ? P10_EV_OPEN : P10_EV_CLOSE);
		}
		if (s->now == adv_at) {                                /* adv_measure(): sanity sweep */
			adv_at += 10000000;
			if (p10_sanity_due(&s->m.p, s_rtc(s, s->now))) { s->pending |= EVB(P10_EV_RECOVER); s->sanity_fired++; }
		}
		s_dispatch(s);
		s_maybe_sleep(s);
	}
	if (s->m.hw.uart_on) s->m.hw.uart_on_us += s->end_us - s->m.hw.uart_on_since;
	if (s->awake) s->awake_us += s->end_us - s->awake_since;
	s->seq[s->seqlen] = 0;
}

/* common healthy-run assertions */
static int sim_clean(const sim_t *s)
{	return s->m.inv_fails == 0 && s->sanity_fired == 0 && s->m.p.recovers == 0; }

/* ================================================================== tests */
static void t1_init(void)
{
	ucap_p10_t p, z;
	p10_init(&p);
	CHECK(p.st == P10_ST_WAIT_EDGE && p.t_est_ms == 10390 && p.guard_ms == 100 && p10_window_ms(&p) == 230 && p.health == 0,
	      "T1 init: WAIT_EDGE, t_est 10390, guard 100, window 230, health 0");
	memset(&z, 0, sizeof z); z.st = P10_ST_WAIT_EDGE; z.t_est_ms = P10_T_SEED_MS; z.guard_ms = P10_GUARD_BASE_MS;
	CHECK(memcmp(&p, &z, sizeof p) == 0 && !p10_connected(&p), "T1 init: every other field zero");
}

static void t2_arith(void)
{
	ucap_p10_t p; uint32_t x = 123456;
	CHECK(p10_dt_ticks(100, 0xFFFF00) == 356 && p10_ticks_to_ms(32768) == 1000 && p10_ticks_to_ms(p10_dt_ticks(100, 0xFFFF00)) == 10,
	      "T2 dt_ticks wraps at 24 bits, 32768 ticks = 1000 ms");
	CHECK(p10_ticks_to_ms_rc(340000, 8203) == 10375 + 518 && p10_ticks_to_ms_rc(340000, 7421) == 9856,
	      "T2 rc correction at +-5 %");
	CHECK(p10_ticks_to_ms_rc(x, 3906) == p10_ticks_to_ms(x) && p10_ticks_to_ms_rc(x, 0) == p10_ticks_to_ms(x)
	      && p10_ticks_to_ms_rc(x, 7420) == p10_ticks_to_ms(x) && p10_ticks_to_ms_rc(x, 8204) == p10_ticks_to_ms(x),
	      "T2 unconverged / out-of-range ct -> nominal");
	CHECK(p10_ticks_to_ms_rc(340000, 7812) == 10375 && p10_ticks_to_ms_rc(340000, 7813) == 10375,
	      "T2 nominal ct -> no correction");
	p10_init(&p); p.t_e = MS(1000);
	CHECK(p10_open_delay_ms(&p, MS(1020), 7812) == 10390u - 100u - p10_ticks_to_ms_rc(p10_dt_ticks(MS(1020), MS(1000)), 7812),
	      "T2 open delay = T - guard - since");
	{
		int i, ok = 1;
		for (i = 0; i <= 12000; i += 7) {
			uint32_t now = MS(1000 + (uint32_t)i), d = p10_open_delay_ms(&p, now, 7812);
			uint32_t lead = 100u + p10_ticks_to_ms_rc(p10_dt_ticks(now, p.t_e), 7812);
			uint32_t want = (lead + P10_MIN_TIMER_MS >= 10390u) ? P10_MIN_TIMER_MS : 10390u - lead;
			ok &= d == want && d >= P10_MIN_TIMER_MS;
		}
		CHECK(ok && p10_open_delay_ms(&p, MS(1000 + 10300), 7812) == P10_MIN_TIMER_MS,
		      "T2 open delay = T - guard - since for since 0..12 s, floors at P10_MIN_TIMER_MS");
	}
	CHECK(p10_ms_to_ticks(1000) == 32768 && p10_ticks_to_ms(P10_RTC_MASK) == 511999, "T2 ms<->ticks, full 24-bit range");
}

static void t3_edge_verify_real(void)
{
	mach_t m; p10_out_t o;
	m_init(&m);
	o = go(&m, P10_EV_EDGE, 1000, 1000, 0, 0);
	CHECK(o.acts == (P10_ACT_LOCK_VERIFY | P10_ACT_START_VERIFY) && m.p.st == P10_ST_VERIFY && m.p.verify_from == P10_ST_WAIT_EDGE,
	      "T3 EDGE in WAIT_EDGE -> VERIFY [LOCK_VERIFY|START_VERIFY]");
	o = fire_verify(&m, 1000, 12);
	CHECK(o.acts == (P10_ACT_UNLOCK_VERIFY | P10_ACT_ISR_RESET | P10_ACT_START_OPEN) && m.p.st == P10_ST_WAIT_OPEN,
	      "T3 VERIFY_TO(n=12) -> WAIT_OPEN [UNLOCK_VERIFY|ISR_RESET|START_OPEN]");
	CHECK(absd(o.open_ms, 10390 - 100 - 20) <= 1 && m.p.edges == 1 && m.p.anchor_valid && m.p.t_e == MS(1000),
	      "T3 open_ms = 10390-100-20 (+-1), edges 1, anchor set");
	CHECK(m.inv_fails == 0, "T3 model invariants");
}

static void t4_glitch(void)
{
	mach_t m; p10_out_t o; uint8_t n; int ok = 1;
	for (n = 0; n < P10_VERIFY_MIN_EDGES; n++) {
		m_init(&m);
		go(&m, P10_EV_EDGE, 1000, 1000, 0, 0);
		o = fire_verify(&m, 1000, n);
		ok &= m.p.st == P10_ST_WAIT_EDGE && m.p.glitches == 1 && !(o.acts & P10_ACT_START_OPEN)
		      && (o.acts & P10_ACT_UNLOCK_VERIFY) && !m.hw.usr1_locked && m.p.edges == 0 && !m.p.anchor_valid;
	}
	CHECK(ok, "T4 VERIFY_TO(n=0..3) from WAIT_EDGE: glitch, back to WAIT_EDGE, USR1 released, no window");
	reach(&m, R_VERIFY_WO, 1000);
	o = fire_verify(&m, 6000, 2);
	CHECK(m.p.st == P10_ST_WAIT_OPEN && m.p.glitches == 1 && !(o.acts & P10_ACT_START_OPEN) && m.hw.timer[TM_O] >= 0
	      && !m.hw.usr1_locked && m.p.strays == 0 && m.p.anchor_tick == MS(1000),
	      "T4 glitch in VERIFY(from WAIT_OPEN): back to WAIT_OPEN, OPEN timer untouched, anchor untouched");
	CHECK(m.inv_fails == 0, "T4 model invariants");
}

static void t5_hit_good(void)
{
	mach_t m; p10_out_t o; uint32_t T = 10390, done = 1000 + T + 18, open_ms;
	reach(&m, R_WINDOW, 1000);
	open_ms = tmr_ms(&m, TM_C) - 230;
	o = go(&m, P10_EV_FRAME, done, 0, 0, 1);
	CHECK(m.p.st == P10_ST_WAIT_EDGE && m.p.hits == 1 && m.p.hits_bad == 0 && m.p.health == 100 && m.p.hist_n == 1,
	      "T5 FRAME(ok) in WINDOW: hit, health 100");
	CHECK(absd(m.p.t_est_ms, T) <= 2 && m.p.t_src == 1 && m.p.last_dt_ms == m.p.t_est_ms, "T5 t_est within 2 ms of T (hit-derived)");
	CHECK(m.p.anchor_tick == ((MS(done) - p10_ms_to_ticks(18)) & P10_RTC_MASK) && m.p.anchor_valid, "T5 anchor = tick - 18 ms");
	CHECK(o.acts == (P10_ACT_STOP_CLOSE | P10_ACT_UART_OFF | P10_ACT_GPIO_ARM | P10_ACT_ISR_RESET),
	      "T5 acts STOP_CLOSE|UART_OFF|GPIO_ARM|ISR_RESET");
	CHECK(m.p.guard_ms == 100 && m.p.last_missed == 0 && m.p.miss_streak == 0, "T5 guard 100, streak 0");
	CHECK(absd(m.p.last_hit_pos_ms, 118) <= 1 && absd(done - open_ms, 118) <= 1, "T5 last_hit_pos_ms ~ guard + 18");
	CHECK(m.inv_fails == 0 && !m.hw.uart_on && !m.hw.uart_locked && m.hw.gpio_armed, "T5 model: UART off, pin back on GPIO");
}

static void t6_hit_bad(void)
{
	mach_t m; p10_out_t o; uint32_t T = 10400;
	reach(&m, R_WINDOW, 1000);
	o = go(&m, P10_EV_FRAME, 1000 + T + 18, 0, 0, 0);
	CHECK(m.p.hits == 1 && m.p.hits_bad == 1 && m.p.health == 0 && m.p.hist_n == 1, "T6 CRC-bad hit: hits 1, hits_bad 1, health 0");
	CHECK(absd(m.p.t_est_ms, T) <= 2 && m.p.t_src == 1 && m.p.miss_streak == 0 && m.p.last_missed == 0, "T6 timing learnt from a CRC-bad frame");
	CHECK(o.acts == (P10_ACT_STOP_CLOSE | P10_ACT_UART_OFF | P10_ACT_GPIO_ARM | P10_ACT_ISR_RESET) && m.p.st == P10_ST_WAIT_EDGE,
	      "T6 same teardown acts");
	CHECK(m.inv_fails == 0, "T6 model invariants");
}

static void t7_miss_guard(void)
{
	mach_t m; p10_out_t o;
	reach(&m, R_WINDOW, 1000);
	o = fire_close(&m, 0);
	CHECK(m.p.misses == 1 && m.p.health == 0 && m.p.last_missed == 1 && m.p.guard_ms == 100 && m.p.miss_streak == 1
	      && o.acts == (P10_ACT_UART_OFF | P10_ACT_GPIO_ARM | P10_ACT_ISR_RESET) && m.p.st == P10_ST_WAIT_EDGE,
	      "T7 first miss: misses 1, health 0, last_missed 1, guard 100");
	go(&m, P10_EV_EDGE, 20000, 20000, 0, 0); fire_verify(&m, 20000, 12); o = fire_open(&m, 20000, 0);
	CHECK(o.close_ms == 230, "T7 close_ms 230 at guard 100");
	fire_close(&m, 0);
	CHECK(m.p.misses == 2 && m.p.miss_streak == 2 && m.p.guard_ms == 200 && m.p.health == 0, "T7 second consecutive miss: guard 200");
	go(&m, P10_EV_EDGE, 40000, 40000, 0, 0); o = fire_verify(&m, 40000, 12);
	/* edge-to-edge 20 s from the anchor at 20 s after two misses: k=2 learning -> 10000 */
	CHECK(m.p.t_src == 3 && absd(m.p.t_est_ms, 10000) <= 1 && m.p.last_dt_ms == m.p.t_est_ms && absd(o.open_ms, (uint32_t)m.p.t_est_ms - 200 - 20) <= 1,
	      "T7 k=2 edge learning after two misses; open lead uses guard 200");
	o = fire_open(&m, 40000, 0);
	CHECK(o.close_ms == 430 && p10_window_ms(&m.p) == 430, "T7 close_ms 430 on the next open");
	go(&m, P10_EV_FRAME, 40000 + 10000 + 18, 0, 0, 1);
	CHECK(m.p.guard_ms == 100 && m.p.miss_streak == 0 && m.p.last_missed == 0 && m.p.hits == 1 && m.p.health == 33,
	      "T7 hit -> guard 100, streak 0, health 1/3");
	go(&m, P10_EV_EDGE, 60000, 60000, 0, 0); fire_verify(&m, 60000, 12); fire_open(&m, 60000, 0);
	fire_close(&m, 1);
	CHECK(m.p.partial_at_close == 1 && m.p.misses == 3 && m.p.guard_ms == 100, "T7 CLOSE with n=1 -> partial_at_close 1, guard stays 100 after one miss");
	/* guard ladder (review fix): streak 2,3 -> 200; streak 4+ -> 400 (window 830); a hit -> 100 */
	go(&m, P10_EV_EDGE, 80000, 80000, 0, 0); fire_verify(&m, 80000, 12); fire_open(&m, 80000, 0); fire_close(&m, 0);
	CHECK(m.p.miss_streak == 2 && m.p.guard_ms == 200, "T7 ladder: streak 2 -> guard 200");
	go(&m, P10_EV_EDGE, 100000, 100000, 0, 0); fire_verify(&m, 100000, 12); o = fire_open(&m, 100000, 0);
	CHECK(o.close_ms == 430, "T7 ladder: close_ms 430 at guard 200");
	fire_close(&m, 0);
	CHECK(m.p.miss_streak == 3 && m.p.guard_ms == 200, "T7 ladder: streak 3 -> guard stays 200");
	go(&m, P10_EV_EDGE, 120000, 120000, 0, 0); fire_verify(&m, 120000, 12); fire_open(&m, 120000, 0); fire_close(&m, 0);
	CHECK(m.p.miss_streak == 4 && m.p.guard_ms == 400 && m.p.misses == 6, "T7 ladder: streak 4 -> guard 400");
	go(&m, P10_EV_EDGE, 140000, 140000, 0, 0); o = fire_verify(&m, 140000, 12);
	CHECK(absd(o.open_ms, (uint32_t)m.p.t_est_ms - 400 - 20) <= 1, "T7 ladder: open lead uses guard 400");
	o = fire_open(&m, 140000, 0);
	CHECK(o.close_ms == 830 && p10_window_ms(&m.p) == 830, "T7 ladder: close_ms 830 at guard 400");
	fire_close(&m, 0);
	CHECK(m.p.miss_streak == 5 && m.p.guard_ms == 400, "T7 ladder: streak 5 -> guard capped at 400");
	go(&m, P10_EV_EDGE, 160000, 160000, 0, 0); fire_verify(&m, 160000, 12); fire_open(&m, 160000, 0);
	go(&m, P10_EV_FRAME, 160000 + m.p.t_est_ms + 18, 0, 0, 1);
	CHECK(m.p.guard_ms == 100 && m.p.miss_streak == 0 && m.p.hits == 2, "T7 ladder: a hit resets guard to 100");
	CHECK(m.inv_fails == 0, "T7 model invariants");
}

static void t8_boot_burst(void)
{
	sim_t s; int i;
	sim_init(&s, 11);
	sim_add_frame(&s, 0, 1, 0); sim_add_frame(&s, 2000000, 1, 0);
	for (i = 0; i < 8; i++) sim_add_frame(&s, 12400000 + (int64_t)i * 10400000, 1, 0);
	sim_run(&s);
	CHECK(sim_clean(&s), "T8 boot burst: clean run");
	CHECK(s.seqlen >= 4 && strncmp(s.seq, "MHHH", 4) == 0 && s.m.p.strays == 1,
	      "T8 window from frame 1 misses, frame 2 is a stray, then hits");
	CHECK(s.first_hit_us >= 0 && s.first_hit_us / 1000 >= 22810 && s.first_hit_us / 1000 <= 22830, "T8 first hit = frame 4 at ~22.8 s");
	CHECK(absd(s.m.p.t_est_ms, 10400) <= 5 && s.misses == 1 && s.m.p.edges == (uint16_t)(s.windows), "T8 t_est within 5 ms of 10.4 s, one miss, every window from an edge");
	printf("     T8 seq=%s t_est=%u strays=%u glitches=%u\n", s.seq, s.m.p.t_est_ms, s.m.p.strays, s.m.p.glitches);
	sim_free(&s);
}

static void t9_button_between(void)
{
	sim_t s, b; int64_t T = 10400000; uint32_t rng = 5;
	sim_init(&b, 12); sim_add_periodic(&b, &rng, 0, 12, period_const, &T, 0); sim_run(&b);
	sim_init(&s, 12); rng = 5; sim_add_periodic(&s, &rng, 0, 12, period_const, &T, 0);
	sim_add_frame(&s, 2 * T + 4000000, 1, 1);           /* press 4 s after edge frame 2 */
	sim_run(&s);
	CHECK(sim_clean(&s) && sim_clean(&b), "T9 clean runs");
	CHECK(s.m.p.strays == 1 && s.hits == b.hits && s.misses == b.misses && s.misses == 0 && strcmp(s.seq, b.seq) == 0,
	      "T9 button between N and N+1: one stray, window still hits N+1");
	CHECK(absd(s.m.p.t_est_ms, b.m.p.t_est_ms) <= 2 && absd(s.m.p.t_est_ms, 10400) <= 2, "T9 t_est unchanged (+-2 ms)");
	sim_free(&s); sim_free(&b);
}

static void t10_button_as_edge(void)
{
	static const int64_t r_ms[5] = { 50, 500, 3000, 6000, 9500 };
	int i, ok = 1;
	for (i = 0; i < 5; i++) {
		sim_t s; int64_t T = 10400000; uint32_t rng = 7; int tail_ok = 1, j;
		sim_init(&s, 13); sim_add_periodic(&s, &rng, 0, 16, period_const, &T, 0);
		sim_add_frame(&s, 2 * T - r_ms[i] * 1000, 1, 1);  /* press r before periodic frame 2 (WAIT_EDGE after the hit on frame 1) */
		sim_run(&s);
		for (j = s.seqlen - 4; j < s.seqlen; j++) if (j < 0 || s.seq[j] != 'H') tail_ok = 0;
		printf("     T10 r=%lld ms: seq=%s misses=%d t_est=%u strays=%u\n", (long long)r_ms[i], s.seq, s.misses, s.m.p.t_est_ms, s.m.p.strays);
		ok &= sim_clean(&s) && (r_ms[i] <= 100 ? s.misses == 0 : s.misses == 1) && absd(s.m.p.t_est_ms, 10400) < 20 && tail_ok
		      && s.m.p.strays == 1;
		sim_free(&s);
	}
	CHECK(ok, "T10 button as edge N: r<=guard 0 misses, else exactly 1; t_est recovers < 20 ms; steady hits");
}

static void t11_two_presses(void)
{
	static const int64_t b_off_ms[4] = { 200, 1000, 3000, 5000 };
	int i, ok = 1;
	for (i = 0; i < 4; i++) {
		sim_t s; int64_t T = 10400000; uint32_t rng = 9; int tail_ok = 1, j;
		sim_init(&s, 14); sim_add_periodic(&s, &rng, 0, 20, period_const, &T, 0);
		sim_add_frame(&s, 2 * T - 5400000, 1, 1);           /* press A: edge; frame 2 becomes a stray, window misses at A+10.52 s */
		sim_add_frame(&s, 2 * T - 5400000 + 10520000 + b_off_ms[i] * 1000, 1, 1);   /* press B after the miss */
		sim_run(&s);
		for (j = s.seqlen - 5; j < s.seqlen; j++) if (j < 0 || s.seq[j] != 'H') tail_ok = 0;
		printf("     T11 B+%lld ms: seq=%s misses=%d t_est=%u\n", (long long)b_off_ms[i], s.seq, s.misses, s.m.p.t_est_ms);
		ok &= sim_clean(&s) && s.misses <= 3 && s.max_miss_run <= 3 && tail_ok && absd(s.m.p.t_est_ms, 10400) <= 2 && s.m.p.strays == 2;
		sim_free(&s);
	}
	CHECK(ok, "T11 two presses straddling a miss: converges within <= 3 misses, every window from a verified edge");
}

static void t12_seed_mismatch(void)
{
	static const int64_t T_ms[7] = { 7500, 8000, 9500, 9900, 10900, 12000, 13500 };   /* in band [7000, 13999] */
	int i, ok = 1, ok95 = 1;
	for (i = 0; i < 7; i++) {
		sim_t s; int64_t T = T_ms[i] * 1000; uint32_t rng = 21;
		sim_init(&s, 15); sim_add_periodic(&s, &rng, 0, 200, period_const, &T, 1000);
		sim_run(&s);
		printf("     T12 T=%lld: misses_before_hit=%d hits=%d/%d windows t_est=%u strays=%u t_src2=%d\n", (long long)T_ms[i],
		       s.misses_before_first_hit, s.hits, s.windows, s.m.p.t_est_ms, s.m.p.strays, s.t_src2_seen);
		ok &= sim_clean(&s) && s.misses_before_first_hit <= 2 && absd(s.m.p.t_est_ms, (uint32_t)T_ms[i]) <= 5;
		ok95 &= s.hits * 100 >= s.windows * 95;
		if (T_ms[i] == 9500) CHECK(s.m.p.strays >= 1 && s.t_src2_seen, "T12 T=9.5 s: stray anchors, k=1 edge learning after the miss");
		sim_free(&s);
	}
	CHECK(ok, "T12 seed mismatch: <= 2 misses to first hit, final t_est within 5 ms");
	CHECK(ok95, "T12 seed mismatch: >= 95 % hits over 200 frames");
}

static void t13_setup(sim_t *s, uint32_t seed, uint32_t rtc_start)
{
	int n = 2000; uint32_t rng = 31;
	sim_init(s, seed); s->rtc_start = rtc_start;
	sim_add_periodic(s, &rng, 500000, n, period_tri, &n, 1000);
}
static int t13_good(const sim_t *s, int strict_windows)
{
	int64_t span = s->end_us;
	return sim_clean(s) && s->hits * 100 >= s->windows * 97 && (!strict_windows || abs(s->windows - 1000) <= 3)
	       && s->m.hw.uart_on_us * 1000 < span * 13 && s->max_miss_run <= 2;
}
static char t13_seq[SEQ_MAX + 1];
static void t13_drift(void)
{
	sim_t s; t13_setup(&s, 41, 0); sim_run(&s);
	printf("     T13 hits=%d misses=%d windows=%d uart duty=%.3f %% awake duty=%.3f %% t_est=%u glitches=%u\n", s.hits, s.misses, s.windows,
	       100.0 * (double)s.m.hw.uart_on_us / (double)s.end_us, 100.0 * (double)s.awake_us / (double)s.end_us, s.m.p.t_est_ms, s.m.p.glitches);
	CHECK(sim_clean(&s), "T13 drift: clean run (no invariant failure, no sanity recover)");
	CHECK(s.hits * 100 >= s.windows * 97, "T13 drift: hits >= 97 % of windows");
	CHECK(abs(s.windows - 1000) <= 3, "T13 drift: one window per two frames (+-3)");
	CHECK(s.m.hw.uart_on_us * 1000 < s.end_us * 13, "T13 drift: UART-on duty < 1.3 %");
	CHECK(s.max_miss_run <= 2, "T13 drift: no consecutive-miss run > 2");
	strcpy(t13_seq, s.seq);
	sim_free(&s);
}

static void t14_preopen_band(void)
{
	int d, ok = 1, ok_guard = 1;
	for (d = 1; d <= 19; d++) {
		sim_t s; int64_t T = (10289 - d) * 1000LL; uint32_t rng = 3; int j, tail_ok = 1;
		sim_init(&s, 50 + (uint32_t)d); s.lat_lo = s.lat_hi = 2000;
		sim_add_periodic(&s, &rng, 0, 14, period_const, &T, 0);
		sim_run(&s);
		for (j = s.seqlen - 3; j < s.seqlen; j++) if (j < 0 || s.seq[j] != 'H') tail_ok = 0;
		printf("     T14 d=%2d: seq=%s guard200=%d t_est=%u strays=%u glitches=%u stale=%u\n", d, s.seq, s.guard200_seen, s.m.p.t_est_ms,
		       s.m.p.strays, s.m.p.glitches, s.m.p.stale_evt);
		ok &= sim_clean(&s) && s.first_hit_us >= 0 && s.misses_before_first_hit <= 2 && s.misses_before_first_hit >= 1
		      && s.m.p.guard_ms == 100 && tail_ok && absd(s.m.p.t_est_ms, (uint32_t)(T / 1000)) <= 2;
		ok_guard &= s.guard200_seen == (s.misses_before_first_hit == 2);
		sim_free(&s);
	}
	CHECK(ok, "T14 pre-open band d=1..19 ms: a hit within 3 windows, guard back to 100, t_est re-measured");
	CHECK(ok_guard, "T14 pre-open band: guard widens to 200 exactly when two misses precede the hit");
}

static void t15_rtc_wrap(void)
{
	sim_t s; t13_setup(&s, 41, (1u << 24) - 30u * 32768u); sim_run(&s);
	CHECK(t13_good(&s, 1), "T15 RTC wrap: T13 quality with the RTC starting 30 s before the 24-bit wrap");
	CHECK(strcmp(s.seq, t13_seq) == 0 && s.seqlen > 900, "T15 RTC wrap: identical hit/miss sequence to T13");
	sim_free(&s);
}

static void t16_anchor_age(void)
{
	mach_t m; uint16_t t0;
	reach(&m, R_WINDOW, 1000); fire_close(&m, 0);          /* miss: edge learning enabled */
	t0 = m.p.t_est_ms;
	CHECK(m.p.last_missed && m.p.anchor_valid && p10_anchor_usable(&m.p, 201) && !p10_anchor_usable(&m.p, 202),
	      "T16 anchor usable up to 200 s");
	go(&m, P10_EV_EDGE, 251000, 251000, 0, 0); fire_verify(&m, 251000, 12);
	CHECK(m.p.t_est_ms == t0 && m.p.t_src == 0 && m.p.last_dt_ms == 0, "T16 stale anchor (250 s): no learning");
	CHECK(m.p.anchor_valid && m.p.anchor_sec == 251 && m.p.anchor_tick == MS(251000) && m.p.st == P10_ST_WAIT_OPEN, "T16 anchor re-set from the new edge");
	/* control: same trace with a 10.4 s gap learns */
	reach(&m, R_WINDOW, 1000); fire_close(&m, 0);
	go(&m, P10_EV_EDGE, 1000 + 20800, 1000 + 20800, 0, 0); fire_verify(&m, 1000 + 20800, 12);
	CHECK(m.p.t_est_ms == t0 && m.p.t_src == 0, "T16 control: 2T edge-to-edge after one miss is rejected (k=2 needs two misses)");
	fire_open(&m, 0, 0); fire_close(&m, 0);
	go(&m, P10_EV_EDGE, 1000 + 41600, 1000 + 41600, 0, 0); fire_verify(&m, 1000 + 41600, 12);
	CHECK(absd(m.p.t_est_ms, 10400) <= 1 && m.p.t_src == 3 && m.p.guard_ms == 200, "T16 control: 2T after two misses learns k=2");
	CHECK(m.inv_fails == 0, "T16 model invariants");
}

static void t17_connect(void)
{
	int w, ok = 1, ok_abort = 1, ok_dup = 1;
	for (w = 0; w < R_COUNT; w++) {
		mach_t m; p10_out_t o; uint16_t stale;
		if (w == R_SUSP) continue;
		reach(&m, w, 1000);
		o = go(&m, P10_EV_CONNECT, 30000, 0, 0, 0);
		ok &= m.p.st == P10_ST_SUSPENDED && p10_connected(&m.p) && m.hw.uart_on && !m.hw.uart_locked && !m.hw.usr1_locked
		      && m.hw.timer[0] < 0 && m.hw.timer[1] < 0 && m.hw.timer[2] < 0 && m.p.connects == 1 && !m.p.anchor_valid
		      && (o.acts & (P10_ACT_STOP_VERIFY | P10_ACT_STOP_OPEN | P10_ACT_STOP_CLOSE | P10_ACT_UNLOCK_VERIFY | P10_ACT_UART_OFF
		                    | P10_ACT_UART_ON_FREE | P10_ACT_ISR_RESET)) == (P10_ACT_STOP_VERIFY | P10_ACT_STOP_OPEN | P10_ACT_STOP_CLOSE
		                    | P10_ACT_UNLOCK_VERIFY | P10_ACT_UART_OFF | P10_ACT_UART_ON_FREE | P10_ACT_ISR_RESET)
		      && !(o.acts & (P10_ACT_UART_ON_LOCK | P10_ACT_LOCK_VERIFY | P10_ACT_START_VERIFY | P10_ACT_START_OPEN | P10_ACT_START_CLOSE))
		      && m.inv_fails == 0;
		ok_abort &= m.p.win_aborted == (w == R_WINDOW ? 1 : 0);
		stale = m.p.stale_evt;
		o = go(&m, P10_EV_CONNECT, 31000, 0, 0, 0);
		ok_dup &= o.acts == 0 && m.p.stale_evt == stale + 1 && m.p.connects == 1 && m.p.st == P10_ST_SUSPENDED && m.inv_fails == 0;
		if (!ok) printf("     T17 failed from %s\n", r_name[w]);
	}
	CHECK(ok, "T17 CONNECT from every state: timers stopped, USR1/UART0 released, UART on unlocked, SUSPENDED");
	CHECK(ok_abort, "T17 win_aborted only from WINDOW");
	CHECK(ok_dup, "T17 second CONNECT: stale_evt, no acts");
}

static void t18_disconnect(void)
{
	int w, ok = 1; mach_t m; p10_out_t o;
	reach(&m, R_SUSP, 1000);
	o = go(&m, P10_EV_DISCONNECT, 5000, 0, 0, 0);
	CHECK(m.p.st == P10_ST_WAIT_EDGE && o.acts == (P10_ACT_UART_OFF | P10_ACT_GPIO_ARM | P10_ACT_ISR_RESET) && !m.p.anchor_valid
	      && m.p.resumes == 1 && !m.hw.uart_on && m.hw.gpio_armed && m.inv_fails == 0,
	      "T18 DISCONNECT from SUSPENDED -> WAIT_EDGE [UART_OFF|GPIO_ARM|ISR_RESET], anchor invalid");
	for (w = 0; w < R_COUNT; w++) {
		uint16_t stale; uint8_t st;
		if (w == R_SUSP) continue;
		reach(&m, w, 1000); stale = m.p.stale_evt; st = m.p.st;
		o = go(&m, P10_EV_DISCONNECT, 20000, 0, 0, 0);
		ok &= o.acts == 0 && m.p.stale_evt == stale + 1 && m.p.st == st && m.p.resumes == 0 && m.inv_fails == 0;
	}
	CHECK(ok, "T18 DISCONNECT from any other state: stale_evt++, acts 0, state untouched");
}

static void t19_stale(void)
{
	mach_t m; p10_out_t o; int ok = 1;
	reach(&m, R_WE, 1000); o = go(&m, P10_EV_OPEN, 2000, 0, 0, 0);
	ok &= o.acts == 0 && m.p.stale_evt == 1 && m.p.st == P10_ST_WAIT_EDGE;
	o = go(&m, P10_EV_VERIFY_TO, 2100, 0, 12, 0); ok &= o.acts == 0 && m.p.stale_evt == 2;
	o = go(&m, P10_EV_CLOSE, 2200, 0, 0, 0);       ok &= o.acts == 0 && m.p.stale_evt == 3;
	o = go(&m, P10_EV_FRAME, 2300, 0, 0, 1);       ok &= o.acts == 0 && m.p.frame_oos == 1 && m.p.hits == 0 && m.p.stale_evt == 3;
	CHECK(ok, "T19 WAIT_EDGE: OPEN/VERIFY_TO/CLOSE stale, FRAME -> frame_oos, acts 0");
	ok = 1;
	reach(&m, R_WO, 1000); o = go(&m, P10_EV_CLOSE, 3000, 0, 0, 0);
	ok &= o.acts == 0 && m.p.stale_evt == 1 && m.p.st == P10_ST_WAIT_OPEN && m.hw.timer[TM_O] >= 0;
	o = go(&m, P10_EV_VERIFY_TO, 3100, 0, 12, 0); ok &= o.acts == 0 && m.p.stale_evt == 2;
	o = go(&m, P10_EV_FRAME, 3200, 0, 0, 1);       ok &= o.acts == 0 && m.p.frame_oos == 1 && m.p.st == P10_ST_WAIT_OPEN;
	CHECK(ok, "T19 WAIT_OPEN: CLOSE/VERIFY_TO stale, FRAME -> frame_oos, OPEN timer kept");
	ok = 1;
	reach(&m, R_WINDOW, 1000); o = go(&m, P10_EV_VERIFY_TO, 12000, 0, 12, 0);
	ok &= o.acts == 0 && m.p.stale_evt == 1 && m.p.st == P10_ST_WINDOW;
	o = go(&m, P10_EV_OPEN, 12010, 0, 0, 0); ok &= o.acts == 0 && m.p.stale_evt == 2;
	o = go(&m, P10_EV_EDGE, 12020, 12020, 0, 0); ok &= o.acts == 0 && m.p.stale_evt == 3 && m.p.st == P10_ST_WINDOW;
	CHECK(ok, "T19 WINDOW: VERIFY_TO/OPEN/EDGE stale, acts 0");
	ok = 1;
	reach(&m, R_VERIFY_WE, 1000); o = go(&m, P10_EV_EDGE, 1005, 1005, 0, 0);
	ok &= o.acts == 0 && m.p.stale_evt == 1 && m.p.st == P10_ST_VERIFY && m.hw.usr1_locked;
	o = go(&m, P10_EV_OPEN, 1010, 1000, 5, 0); ok &= o.acts == 0 && m.p.stale_evt == 2 && m.p.st == P10_ST_VERIFY;
	o = go(&m, P10_EV_CLOSE, 1012, 0, 0, 0);   ok &= o.acts == 0 && m.p.stale_evt == 3;
	o = go(&m, P10_EV_FRAME, 1014, 0, 0, 1);   ok &= o.acts == 0 && m.p.stale_evt == 4 && m.p.frame_oos == 0;
	CHECK(ok, "T19 VERIFY(from WAIT_EDGE): EDGE re-post, OPEN, CLOSE, FRAME stale, acts 0");
	ok = 1;
	reach(&m, R_SUSP, 1000); o = go(&m, P10_EV_FRAME, 5000, 0, 0, 1);
	ok &= o.acts == 0 && m.p.stale_evt == 0 && m.p.frame_oos == 0 && m.p.hits == 0;
	o = go(&m, P10_EV_EDGE, 5001, 5001, 0, 0);  ok &= o.acts == 0 && m.p.stale_evt == 1;
	o = go(&m, P10_EV_OPEN, 5002, 0, 0, 0);     ok &= o.acts == 0 && m.p.stale_evt == 2 && m.p.st == P10_ST_SUSPENDED;
	CHECK(ok && m.inv_fails == 0, "T19 SUSPENDED: FRAME ignored silently, others stale, acts 0");
}

static void t20_open_during_verify(void)
{
	mach_t m; p10_out_t o; uint32_t need = P10_ACT_STOP_VERIFY | P10_ACT_UNLOCK_VERIFY | P10_ACT_UART_ON_LOCK | P10_ACT_START_CLOSE;
	reach(&m, R_WO, 1000);
	go(&m, P10_EV_EDGE, tmr_ms(&m, TM_O) - 10, tmr_ms(&m, TM_O) - 10, 0, 0);
	CHECK(m.p.st == P10_ST_VERIFY && m.p.verify_from == P10_ST_WAIT_OPEN && m.hw.timer[TM_O] >= 0 && m.hw.usr1_locked,
	      "T20 EDGE in WAIT_OPEN -> VERIFY(from WAIT_OPEN), OPEN timer kept");
	o = fire_open(&m, tmr_ms(&m, TM_O) - 10, 5);
	CHECK(m.p.st == P10_ST_WINDOW && m.p.strays == 1 && m.p.windows == 1 && (o.acts & need) == need && (o.acts & P10_ACT_ISR_RESET)
	      && !(o.acts & P10_ACT_START_OPEN) && o.close_ms == 230 && m.p.anchor_tick == MS(tmr_ms(&m, TM_C) - 230 - 10),
	      "T20 OPEN in VERIFY, n>=4: stray anchored, window opens [STOP_VERIFY|UNLOCK_VERIFY|UART_ON_LOCK|START_CLOSE]");
	CHECK(!m.hw.usr1_locked && m.hw.uart_locked && m.hw.uart_on && m.inv_fails == 0, "T20 model: USR1 released before UART0 locked");
	reach(&m, R_WO, 1000);
	go(&m, P10_EV_EDGE, tmr_ms(&m, TM_O) - 3, tmr_ms(&m, TM_O) - 3, 0, 0);
	o = fire_open(&m, tmr_ms(&m, TM_O) - 3, 2);
	CHECK(m.p.st == P10_ST_WINDOW && m.p.glitches == 1 && m.p.strays == 0 && (o.acts & need) == need && m.p.anchor_tick == MS(1000)
	      && !m.hw.usr1_locked && m.inv_fails == 0,
	      "T20 OPEN in VERIFY, n<4: glitch, window opens, anchor untouched");
	reach(&m, R_VERIFY_WE, 1000);
	o = go(&m, P10_EV_OPEN, 1010, 1000, 12, 0);
	CHECK(o.acts == 0 && m.p.st == P10_ST_VERIFY && m.p.stale_evt == 1 && m.p.windows == 0, "T20 OPEN in VERIFY(from WAIT_EDGE) is stale");
}

static void t21_recover(void)
{
	int w, ok = 1;
	for (w = 0; w < R_COUNT; w++) {
		mach_t m; p10_out_t o; uint32_t stops = P10_ACT_STOP_VERIFY | P10_ACT_STOP_OPEN | P10_ACT_STOP_CLOSE | P10_ACT_UNLOCK_VERIFY;
		reach(&m, w, 1000);
		o = go(&m, P10_EV_RECOVER, 40000, 0, 0, 0);
		ok &= (o.acts & stops) == stops && m.p.recovers == 1 && !m.p.anchor_valid && !m.hw.usr1_locked && !m.hw.uart_locked
		      && m.hw.timer[0] < 0 && m.hw.timer[1] < 0 && m.hw.timer[2] < 0 && m.inv_fails == 0;
		if (w == R_SUSP)
			ok &= m.p.st == P10_ST_SUSPENDED && m.hw.uart_on && (o.acts & (P10_ACT_UART_OFF | P10_ACT_UART_ON_FREE | P10_ACT_ISR_RESET))
			      == (P10_ACT_UART_OFF | P10_ACT_UART_ON_FREE | P10_ACT_ISR_RESET);
		else
			ok &= m.p.st == P10_ST_WAIT_EDGE && !m.hw.uart_on && m.hw.gpio_armed
			      && (o.acts & (P10_ACT_UART_OFF | P10_ACT_GPIO_ARM | P10_ACT_ISR_RESET)) == (P10_ACT_UART_OFF | P10_ACT_GPIO_ARM | P10_ACT_ISR_RESET)
			      && !(o.acts & P10_ACT_UART_ON_FREE);
		/* the flow resumes after a recover */
		if (w != R_SUSP) { go(&m, P10_EV_EDGE, 50000, 50000, 0, 0); fire_verify(&m, 50000, 12); ok &= m.p.st == P10_ST_WAIT_OPEN && m.inv_fails == 0; }
		if (!ok) printf("     T21 failed from %s\n", r_name[w]);
	}
	CHECK(ok, "T21 RECOVER from every state: WAIT_EDGE (SUSPENDED re-inited unlocked), locks released, timers stopped, recovers 1, anchor invalid");
	{
		mach_t m; int i;
		reach(&m, R_WE, 1000);
		for (i = 0; i < 300; i++) go(&m, P10_EV_RECOVER, 2000 + (uint32_t)i, 0, 0, 0);
		CHECK(m.p.recovers == 255 && m.inv_fails == 0, "T21 recovers saturates at 255");
	}
}

static void t22_glitch_storm(void)
{
	sim_t b, s; int64_t T = 10400000; uint32_t rng = 77; int i; int64_t span;
	sim_init(&b, 61); sim_add_periodic(&b, &rng, 300000, 400, period_const, &T, 1000); sim_run(&b);
	rng = 77;
	sim_init(&s, 61); sim_add_periodic(&s, &rng, 300000, 400, period_const, &T, 1000);
	span = sim_last_frame(&s);
	for (i = 0; i < 1000; i++) sim_add_glitch(&s, rnd_range(&rng, 0, span), (int)rnd_range(&rng, 1, 3));
	sim_run(&s);
	printf("     T22 base hits=%d/%d; storm hits=%d/%d glitches=%u edges=%u strays=%u stale=%u\n", b.hits, b.windows, s.hits, s.windows,
	       s.m.p.glitches, s.m.p.edges, s.m.p.strays, s.m.p.stale_evt);
	CHECK(sim_clean(&b) && sim_clean(&s), "T22 glitch storm: clean runs (USR1 never left locked, no sanity recover)");
	CHECK(s.m.p.glitches >= 900 && s.windows == b.windows, "T22 glitch storm: ~1000 glitches counted, no window opened by a glitch");
	CHECK(s.hits == b.hits && s.misses == b.misses, "T22 glitch storm: hit ratio equal to the glitch-free run");
	sim_free(&b); sim_free(&s);
}

static void t23_health_ring(void)
{
	ucap_p10_t p; int i, ok = 1; uint32_t ones;
	p10_init(&p);
	for (i = 0; i < 40; i++) {
		uint8_t g = (uint8_t)((i * 7) % 3 != 0);
		p10_hist_push(&p, g);
		if (i < 32) ok &= p.hist_n == i + 1; else ok &= p.hist_n == 32;
		ones = 0; { int k; for (k = 0; k < p.hist_n; k++) ones += (p.hist >> k) & 1u; }
		ok &= p.health == (ones * 100u + p.hist_n / 2u) / p.hist_n;
	}
	CHECK(ok && p.hist_n == 32, "T23 health ring: hist_n caps at 32, health = round(100*ones/hist_n)");
	p10_init(&p); p10_hist_push(&p, 0); p10_hist_push(&p, 0); p10_hist_push(&p, 1);
	CHECK(p.health == 33 && p.hist_n == 3 && p.hist == 1, "T23 1 good of 3 -> 33");
	p10_init(&p); for (i = 0; i < 32; i++) p10_hist_push(&p, 1);
	CHECK(p.health == 100 && p.hist == 0xFFFFFFFFu, "T23 32 good -> 100");
	p10_hist_push(&p, 0);
	CHECK(p.health == 97 && p.hist_n == 32, "T23 oldest outcome drops out: 31/32 -> 97");
}

static void t24_fuzz(void)
{
	mach_t m; uint32_t rng = 0xC0FFEE; int i, n_types[8] = {0};
	int uart_on_pairs_ok = 1;
	m_init(&m);
	for (i = 0; i < 200000; i++) {
		p10_ev_t ev; p10_out_t o; uint32_t r = xs32(&rng);
		memset(&ev, 0, sizeof ev);
		ev.type = (p10_event_t)(r % 8);
		if (ev.type >= P10_EV_CONNECT && (xs32(&rng) % 16) != 0) ev.type = (p10_event_t)(xs32(&rng) % 5);   /* connection/recover rarer */
		ev.tick = xs32(&rng) & P10_RTC_MASK;
		ev.t0 = (xs32(&rng) & 1) ? ((ev.tick - (xs32(&rng) % 4000)) & P10_RTC_MASK) : (xs32(&rng) & P10_RTC_MASK);
		ev.sec = xs32(&rng) % 1000;
		switch (xs32(&rng) % 4) {
		case 0: ev.ct = 0; break;
		case 1: ev.ct = xs32(&rng) % 20000; break;
		default: ev.ct = 7421 + xs32(&rng) % 783; break;
		}
		ev.n = (xs32(&rng) & 1) ? (uint8_t)(xs32(&rng) % 4) : (uint8_t)(4 + xs32(&rng) % 252);
		ev.ok = xs32(&rng) & 1; ev.src = xs32(&rng) & 1;
		m.now = (int64_t)i * 1000;
		m_step(&m, &ev, &o);
		n_types[ev.type]++;
		if (m.inv_fails > 8) break;
	}
	CHECK(m.inv_fails == 0, "T24 fuzz 200k events: hardware-model invariants never fail (locks, pin, timers, UART_ON pairing, START_OPEN origin)");
	CHECK(m.seen_st[0] && m.seen_st[1] && m.seen_st[2] && m.seen_st[3] && m.seen_st[4] && n_types[P10_EV_RECOVER] > 100
	      && n_types[P10_EV_CONNECT] > 100 && uart_on_pairs_ok, "T24 fuzz covered all five states and all event types");
	printf("     T24 windows=%u hits=%u misses=%u aborted=%u lost=%d stale=%u recovers=%u\n", m.p.windows, m.p.hits, m.p.misses,
	       m.p.win_aborted, m.lost_windows, m.p.stale_evt, m.p.recovers);
}

static void t25_stamp_modes(void)
{
	sim_t s; int ok = 1;
	t13_setup(&s, 43, 0); s.src = 1; s.stamp_lo = s.stamp_hi = -600; sim_run(&s);
	printf("     T25 wake stamp -0.6 ms: hits=%d/%d pos=%u\n", s.hits, s.windows, s.m.p.last_hit_pos_ms);
	ok &= t13_good(&s, 1) && absd(s.m.p.last_hit_pos_ms, 118) <= 3; sim_free(&s);
	t13_setup(&s, 43, 0); s.src = 1; s.stamp_lo = s.stamp_hi = 600; sim_run(&s);
	printf("     T25 wake stamp +0.6 ms: hits=%d/%d pos=%u\n", s.hits, s.windows, s.m.p.last_hit_pos_ms);
	ok &= t13_good(&s, 1) && absd(s.m.p.last_hit_pos_ms, 118) <= 3; sim_free(&s);
	t13_setup(&s, 43, 0); s.src = 0; s.stamp_lo = 1000; s.stamp_hi = 13000; sim_run(&s);
	printf("     T25 IRQ stamp 1..13 ms late: hits=%d/%d pos=%u\n", s.hits, s.windows, s.m.p.last_hit_pos_ms);
	ok &= t13_good(&s, 1) && absd(s.m.p.last_hit_pos_ms, 118) <= 15; sim_free(&s);
	t13_setup(&s, 43, 0); s.src = 0; s.stamp_lo = s.stamp_hi = 13000; sim_run(&s);
	printf("     T25 IRQ stamp fixed 13 ms late: hits=%d/%d pos=%u\n", s.hits, s.windows, s.m.p.last_hit_pos_ms);
	ok &= t13_good(&s, 1); sim_free(&s);
	CHECK(ok, "T25 wake stamp (src=1, +-0.6 ms) and late IRQ stamp (src=0, up to 13 ms): all give >= 97 % hits in T13");
}

static void t26_rc_error(void)
{
	int sign, ok_corr = 1, ok_uncorr = 1, ok_unconv = 1;
	char seq0[SEQ_MAX + 1];
	for (sign = -1; sign <= 1; sign += 2) {
		sim_t s;
		t13_setup(&s, 45, 0); s.rc_ppm = sign * 20000; sim_run(&s);
		printf("     T26 e=%+d %% corrected (ct=%u): hits=%d/%d rc_ct=%u\n", sign * 2, s_ct(&s), s.hits, s.windows, s.m.p.rc_ct);
		ok_corr &= t13_good(&s, 1) && s.m.p.rc_ct == s_ct(&s); sim_free(&s);
		t13_setup(&s, 45, 0); s.rc_ppm = sign * 20000; s.ct_forced = 7812; sim_run(&s);
		printf("     T26 e=%+d %% uncorrected (ct=7812): hits=%d/%d, guard-100 windows %d missed %d, max miss run %d, guard400=%d\n",
		       sign * 2, s.hits, s.windows, s.win100, s.miss100, s.max_miss_run, s.guard400_seen);
		/* Bias 208 ms. +2 %: M,M,H cycles (guard 200 catches the frame with ~4 ms to spare, guard 100
		 * never does) -> 33 % hits. -2 %: guard 200 opens 8 ms after the frame start (chopped), so
		 * M,M,M,M,H cycles: the guard-400 rung of the ladder catches it -> 20 % hits. Either way no
		 * guard-100 window after the seeded first one is ever hit, and data keeps flowing. */
		ok_uncorr &= s.m.inv_fails == 0 && s.miss100 >= s.win100 - 1 && s.win100 >= 3 && s.hits * 100 <= s.windows * 50
		             && s.hits * 6 >= s.windows && s.max_miss_run <= 4 && (sign > 0 || s.guard400_seen); sim_free(&s);
		/* ct below P10_RC_CT_MIN (1) vs unconverged (3906): both fall back to nominal */
		{
			sim_t u;
			t13_setup(&s, 45, 0); s.rc_ppm = sign * 20000; s.ct_forced = 1; sim_run(&s);
			strcpy(seq0, s.seq);
			t13_setup(&u, 45, 0); u.rc_ppm = sign * 20000; u.ct_forced = 3906; sim_run(&u);
			printf("     T26 e=%+d %% unconverged (ct=3906): hits=%d/%d, guard-100 windows %d missed %d\n", sign * 2, u.hits, u.windows, u.win100, u.miss100);
			ok_unconv &= u.m.inv_fails == 0 && strcmp(u.seq, seq0) == 0 && u.miss100 >= u.win100 - 1 && u.hits * 100 <= u.windows * 50
			             && u.m.p.rc_ct == 3906;
			sim_free(&s); sim_free(&u);
		}
	}
	CHECK(ok_corr, "T26 RC32K +-2 % with ct = 7812.5*(1+e): >= 97 % hits (G3 correction works)");
	CHECK(ok_uncorr, "T26 RC32K +-2 % with ct forced to 7812: every guard-100 window after the seeded first misses, 17-50 % hits via the guard ladder");
	CHECK(ok_unconv, "T26 ct = 3906 (unconverged) behaves as uncorrected (identical sequence to nominal)");
}

static void t27_sanity(void)
{
	mach_t m; int ok = 1; uint32_t wo;
	reach(&m, R_VERIFY_WE, 1000);
	ok &= !p10_sanity_due(&m.p, MS(1900)) && p10_sanity_due(&m.p, MS(2100)) && m.p.sanity_recovers == 1;
	CHECK(ok, "T27 VERIFY older than 1 s -> sanity due");
	reach(&m, R_WINDOW, 1000); wo = tmr_ms(&m, TM_C) - 230;
	ok = !p10_sanity_due(&m.p, MS(wo + 1900)) && p10_sanity_due(&m.p, MS(wo + 2100)) && m.p.sanity_recovers == 1;
	CHECK(ok, "T27 WINDOW older than 2 s -> sanity due");
	reach(&m, R_WO, 1000);
	ok = !p10_sanity_due(&m.p, MS(1020 + 19000)) && p10_sanity_due(&m.p, MS(1020 + 21000)) && m.p.sanity_recovers == 1;
	CHECK(ok, "T27 WAIT_OPEN older than 20 s -> sanity due");
	reach(&m, R_WO, 1000);
	go(&m, P10_EV_EDGE, 9000, 9000, 0, 0); fire_verify(&m, 9000, 12);
	ok = m.p.strays == 1 && m.p.st == P10_ST_WAIT_OPEN && !p10_sanity_due(&m.p, MS(1020 + 15000)) && p10_sanity_due(&m.p, MS(1020 + 21000))
	     && !p10_sanity_due(&m.p, MS(1020 + 19000));
	CHECK(ok, "T27 strays in WAIT_OPEN do not reset the WAIT_OPEN age");
	ok = 1;
	reach(&m, R_WE, 1000);   ok &= !p10_sanity_due(&m.p, MS(400000));
	reach(&m, R_SUSP, 1000); ok &= !p10_sanity_due(&m.p, MS(400000));
	reach(&m, R_VERIFY_WE, 1000); ok &= !p10_sanity_due(&m.p, MS(1000 + 20)) && !p10_sanity_due(&m.p, MS(1000 + 999));
	reach(&m, R_WINDOW, 1000);    ok &= !p10_sanity_due(&m.p, MS(tmr_ms(&m, TM_C)));
	reach(&m, R_WO, 1000);        ok &= !p10_sanity_due(&m.p, MS(tmr_ms(&m, TM_O))) && !p10_sanity_due(&m.p, MS(1020 + 16200));
	CHECK(ok && m.p.sanity_recovers == 0, "T27 healthy timings and idle states return 0");
	/* sanity -> RECOVER path end to end */
	reach(&m, R_WINDOW, 1000);
	if (p10_sanity_due(&m.p, MS(20000))) go(&m, P10_EV_RECOVER, 20000, 0, 0, 0);
	CHECK(m.p.st == P10_ST_WAIT_EDGE && m.p.recovers == 1 && m.p.sanity_recovers == 1 && !m.hw.uart_on && m.hw.gpio_armed && m.inv_fails == 0,
	      "T27 sanity-triggered RECOVER from a stuck WINDOW lands in WAIT_EDGE with the UART off");
}

static void t28_silent(void)
{
	sim_t s;
	sim_init(&s, 99); s.end_us = 3600LL * 1000000;
	sim_run(&s);
	CHECK(s.windows == 0 && s.misses == 0 && s.hits == 0 && s.m.p.health == 0 && s.m.p.edges == 0 && s.m.p.glitches == 0,
	      "T28 silent main MCU for 1 h: no windows, no misses, health 0");
	CHECK(s.m.hw.timer[0] < 0 && s.m.hw.timer[1] < 0 && s.m.hw.timer[2] < 0 && s.m.p.st == P10_ST_WAIT_EDGE, "T28 no timers armed, WAIT_EDGE");
	CHECK(s.awake_us == 0 && s.m.hw.uart_on_us == 0 && s.sanity_fired == 0 && s.m.inv_fails == 0, "T28 zero awake / UART time beyond the harness");
	sim_free(&s);
}

/* T29 (review finding): with T_MAX >= 2*T_MIN the k=1 edge learner accepted dt = 2T after a
 * pre-open-band miss (frame N+1 chopped at window open, n < 4 -> not anchored) and the hit path
 * then confirmed 2T forever (data every 3T, health 100). T_MAX < 2*T_MIN closes it: 2T is
 * rejected, the second straddle widens the guard, k=2 learns T. */
static void t29_double_period(void)
{
	static const uint32_t Ts[3] = { 7000, 7500, 7900 };
	mach_t m; int i, d, ok = 1;
	uint32_t T = 7200;                                         /* 2T = 14400 > T_MAX; T itself 200 ms inside the band */
	/* (a) direct trace, the reviewer's x10 scenario */
	m_init(&m); m.p.t_est_ms = (uint16_t)(T + 100);         /* window opens exactly at frame N+1 */
	go(&m, P10_EV_EDGE, 0, 0, 0, 0); fire_verify(&m, 0, 12);
	fire_open(&m, 0, 0); fire_close(&m, 1);                    /* N+1 chopped: miss, not anchored */
	go(&m, P10_EV_EDGE, 2 * T, 2 * T, 0, 0); fire_verify(&m, 2 * T, 12);
	CHECK(m.p.t_est_ms == T + 100 && m.p.t_src == 0, "T29a edge N+2 after one miss: dt = 2T (14400) rejected by T_MAX");
	fire_open(&m, 0, 0); fire_close(&m, 1);                    /* N+3 chopped again: streak 2, guard 200 */
	go(&m, P10_EV_EDGE, 4 * T, 4 * T, 0, 0); fire_verify(&m, 4 * T, 12);
	CHECK(absd(m.p.t_est_ms, T) <= 1 && m.p.t_src == 3 && m.p.guard_ms == 200, "T29a edge N+4 after two misses: k=2 learns T");
	fire_open(&m, 0, 0); go(&m, P10_EV_FRAME, 5 * T + 18, 0, 0, 1);
	CHECK(m.p.hits == 1 && absd(m.p.t_est_ms, T) <= 1 && m.p.t_src == 1 && m.inv_fails == 0, "T29a next window hits at T");
	/* (b) simulator: seed t_est = T + 101..105 so frame N+1 straddles the open by 1..5 ms */
	for (i = 0; i < 3; i++) for (d = 101; d <= 105; d++) {
		sim_t s; int64_t Tus = (int64_t)Ts[i] * 1000; uint32_t rng = 5;
		sim_init(&s, 300 + (uint32_t)(10 * i + d)); s.seed_t_est = (uint16_t)(Ts[i] + (uint32_t)d);
		sim_add_periodic(&s, &rng, 0, 80, period_const, &Tus, 0);
		sim_run(&s);
		if (d == 101 || d == 105)
			printf("     T29b T=%u seed=%u: seq=%.24s... misses_before_hit=%d hits=%d/%d windows t_est=%u\n", Ts[i], Ts[i] + d,
			       s.seq, s.misses_before_first_hit, s.hits, s.windows, s.m.p.t_est_ms);
		ok &= sim_clean(&s) && s.first_hit_us >= 0 && s.misses_before_first_hit <= 3 && absd(s.m.p.t_est_ms, Ts[i]) <= 5
		      && abs(s.windows - 40) <= 3 && s.hits * 100 >= s.windows * 90;
		sim_free(&s);
	}
	CHECK(ok, "T29b T = 7.0/7.5/7.9 s straddling the open: never locks at 2T, one window per two frames, >= 90 % hits");
}

/* T30: what the guard ladder buys when G3 is unavailable (ct out of range -> raw RC error).
 * |bias| = |e| * 10.4 s. Guard 400 covers |bias| < 400 ms (|e| < 3.8 %); 4.5 % is documented as
 * beyond the ladder: all-miss, visible as miss_streak saturating with health 0. */
static void t30_bias_limits(void)
{
	int sign, ok_in = 1, ok_out = 1;
	for (sign = -1; sign <= 1; sign += 2) {
		sim_t s;
		t13_setup(&s, 47, 0); s.rc_ppm = sign * 35000; s.ct_forced = 7812; sim_run(&s);
		printf("     T30 e=%+.1f %% uncorrected: hits=%d/%d max miss run %d guard400=%d\n", sign * 3.5, s.hits, s.windows, s.max_miss_run, s.guard400_seen);
		ok_in &= s.m.inv_fails == 0 && s.guard400_seen && s.hits * 6 >= s.windows && s.max_miss_run <= 4; sim_free(&s);
		t13_setup(&s, 47, 0); s.rc_ppm = sign * 45000; s.ct_forced = 7812; sim_run(&s);
		printf("     T30 e=%+.1f %% uncorrected: hits=%d/%d misses=%d guard=%u streak=%u health=%u\n", sign * 4.5, s.hits, s.windows, s.misses,
		       s.m.p.guard_ms, s.m.p.miss_streak, s.m.p.health);
		ok_out &= s.m.inv_fails == 0 && s.hits <= 1 && s.m.p.guard_ms == P10_GUARD_MAX_MS && s.m.p.miss_streak >= 100 && s.m.p.health == 0
		          && s.sanity_fired == 0; sim_free(&s);
	}
	CHECK(ok_in, "T30 uncorrected +-3.5 % bias: guard 400 keeps data flowing (>= 1 hit in 6 windows, miss runs <= 4)");
	CHECK(ok_out, "T30 uncorrected +-4.5 % bias: beyond the ladder -> all-miss, guard 400, miss_streak climbing, health 0 (documented)");
}

int main(void)
{
	t1_init(); t2_arith(); t3_edge_verify_real(); t4_glitch(); t5_hit_good(); t6_hit_bad(); t7_miss_guard();
	t8_boot_burst(); t9_button_between(); t10_button_as_edge(); t11_two_presses(); t12_seed_mismatch();
	t13_drift(); t14_preopen_band(); t15_rtc_wrap(); t16_anchor_age(); t17_connect(); t18_disconnect();
	t19_stale(); t20_open_during_verify(); t21_recover(); t22_glitch_storm(); t23_health_ring(); t24_fuzz();
	t25_stamp_modes(); t26_rc_error(); t27_sanity(); t28_silent(); t29_double_period(); t30_bias_limits();
	printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all ok", failures, failures == 1 ? "" : "s");
	return failures;
}
