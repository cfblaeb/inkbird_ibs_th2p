/******************************************************************************
 * ucap_p10.h — "P10 alone" scheduler for the IBSTH2P inter-chip UART (V25_P10).
 * Pure C, no SDK includes; host-tested by tests/test_ucap_p10.c.
 *
 * Every main-MCU frame start bit wakes the PHY6222 through GPIO P10. The
 * frame that wakes us is lost; its timestamp t_e is used to open ONE short
 * UART listen window for the NEXT frame at t_e + T_est - guard. Then back
 * to waiting for an edge. A miss never opens another window.
 *
 * This header is the WHOLE state machine: p10_step() maps (state, event) to
 * (new state, action bitmask). It knows nothing about OSAL, pwrmgr or pins;
 * the glue in cmd_parser.c executes the actions in bit order. All stamps
 * are 24-bit RTC ticks (RC32K, nominal 32768 Hz); deltas are masked here.
 ******************************************************************************/
#ifndef _UCAP_P10_H_
#define _UCAP_P10_H_
#include <stdint.h>

/* ---- constants (ms unless noted) ---- */
#define P10_T_SEED_MS        10390u  /* the one measured unit (plan 661-666) */
#define P10_T_MIN_MS          7000u  /* rejects button remainders < 7 s (-33 % of the measured 10.39 s) */
#define P10_T_MAX_MS         13999u  /* +35 %; MUST stay < 2*P10_T_MIN_MS so that 2T of any in-band T is
                                      * rejected and a double period can never train (the original
                                      * 16000 broke this for T in [6, 8) s: review finding, host test T29) */
#define P10_GUARD_BASE_MS      100u  /* one-sided guard around the predicted frame start */
#define P10_GUARD_MAX_MS       400u  /* guard doubles every P10_MISS_WIDEN consecutive misses:
                                      * 100 -> 200 (2 misses) -> 400 (4 misses); back to 100 on a hit.
                                      * 400 covers an uncorrected prediction bias < 400 ms (RC32K
                                      * error < 3.8 % with G3 correction unavailable); beyond that the
                                      * unit stays all-miss and visible (miss_streak, health 0). */
#define P10_MISS_WIDEN           2u
#define P10_FRAME_WIRE_MS       14u  /* 13 bytes x 10 bits / 9600 = 13.54 ms */
#define P10_FRAME_RX_MS         18u  /* start bit -> UART ISR completion: wire + ~4 char FIFO timeout */
#define P10_SLACK_MS            12u  /* late-frame slack after the trailing guard */
#define P10_VERIFY_MS           20u  /* edge-burst observation length */
#define P10_VERIFY_MIN_EDGES     4u  /* a frame gives >= 12 falling edges in 13.5 ms, a glitch 1-3 */
#define P10_MIN_TIMER_MS         5u  /* never arm an OSAL timer shorter than this */
#define P10_ANCHOR_MAX_AGE_S   200u  /* < 512 s RTC wrap, with margin */
#define P10_HIST_N              32u  /* health window (bitmask) */
#define P10_RTC_MASK      0xFFFFFFu
/* RC32K tracking: 16 MHz cycles per 16 RC32K cycles (AP_AON->RTCTRCCNT, ll_sleep.h:44).
 * Exactly 32768 Hz -> 7812.5. The SDK sleeps for crystal-true ms; our stamps are RC ticks. */
#define P10_RC_CT_NOM2        15625u /* 2 * 7812.5 */
#define P10_RC_CT_MIN          7421u /* -5 %: below this the average has not converged (boots at 3906) */
#define P10_RC_CT_MAX          8203u /* +5 % */
/* Sanity sweep (called from adv_measure every ~10 s): a state older than this is a lost timer. */
#define P10_SANITY_VERIFY_MS    1000u
#define P10_SANITY_WINDOW_MS    2000u
#define P10_SANITY_WAIT_OPEN_MS 20000u /* > T_MAX + guard */

#if P10_T_MAX_MS >= 2u * P10_T_MIN_MS
#error "P10_T_MAX_MS must be < 2*P10_T_MIN_MS (double-period ambiguity)"
#endif
#if (2u * P10_GUARD_MAX_MS + P10_FRAME_RX_MS + P10_SLACK_MS) >= P10_SANITY_WINDOW_MS
#error "widest window must close before the sanity sweep declares it lost"
#endif
#if P10_T_SEED_MS < P10_T_MIN_MS || P10_T_SEED_MS > P10_T_MAX_MS || P10_SANITY_WAIT_OPEN_MS <= P10_T_MAX_MS + P10_GUARD_MAX_MS
#error "P10 seed / sanity constants out of band"
#endif

static inline uint32_t p10_dt_ticks(uint32_t later, uint32_t earlier) { return (later - earlier) & P10_RTC_MASK; }
static inline uint32_t p10_ticks_to_ms(uint32_t t) { return (t * 125u) >> 12; }   /* exact for any 24-bit delta */
static inline uint32_t p10_ms_to_ticks(uint32_t ms) { return (ms << 12) / 125u; }

/* RC32K-corrected ticks -> true ms. ct = g_counter_traking_avg; out-of-range ct -> nominal. */
static inline uint32_t p10_ticks_to_ms_rc(uint32_t t, uint32_t ct)
{
	uint32_t ms = p10_ticks_to_ms(t);                 /* <= 512000 for a 24-bit delta */
	int32_t dev, corr;
	if (ct < P10_RC_CT_MIN || ct > P10_RC_CT_MAX) return ms;
	dev  = (int32_t)(2u * ct) - (int32_t)P10_RC_CT_NOM2;   /* |dev| <= 781 */
	corr = ((int32_t)ms * dev) / (int32_t)P10_RC_CT_NOM2;   /* |ms*dev| < 2^29 */
	return (uint32_t)((int32_t)ms + corr);
}

typedef enum { P10_ST_WAIT_EDGE = 0, P10_ST_VERIFY, P10_ST_WAIT_OPEN, P10_ST_WINDOW, P10_ST_SUSPENDED } p10_state_t;

typedef enum {
	P10_EV_EDGE = 0,    /* a burst started at ev.t0 (GPIO IRQ or wake stamp) */
	P10_EV_VERIFY_TO,   /* verify timer; ev.n = falling edges counted since t0 */
	P10_EV_OPEN,        /* open timer (ev.n = live burst count, used if it lands in VERIFY) */
	P10_EV_CLOSE,       /* close timer; ev.n != 0 -> a partial frame sat in the framer */
	P10_EV_FRAME,       /* complete 13-byte frame; ev.ok = CRC valid; ev.tick = ISR completion stamp */
	P10_EV_CONNECT,     /* GAPROLE_CONNECTED */
	P10_EV_DISCONNECT,  /* GAPROLE_WAITING[_AFTER_TIMEOUT] */
	P10_EV_RECOVER      /* invariant violated / SDK call failed / sanity sweep: force a known state */
} p10_event_t;

typedef struct {
	p10_event_t type;
	uint32_t tick;   /* RTC now (24-bit); for FRAME: the ISR completion stamp */
	uint32_t sec;    /* coarse uptime seconds (clkt.utc_time_sec) for anchor ageing */
	uint32_t t0;     /* EDGE/VERIFY_TO/OPEN: burst first-edge stamp */
	uint32_t ct;     /* RC32K tracking count (g_counter_traking_avg), 0 = unknown */
	uint8_t  n;      /* VERIFY_TO/OPEN: edges in burst (saturated at 255); CLOSE: partial-frame flag */
	uint8_t  ok;     /* FRAME: CRC ok */
	uint8_t  src;    /* EDGE: 0 = GPIO IRQ, 1 = wake stamp (telemetry only) */
} p10_ev_t;

/* Actions for the glue. Bit order == execution order:
 * stops -> unlocks -> UART off -> GPIO arm -> UART on -> locks -> timers -> ISR reset. */
#define P10_ACT_STOP_VERIFY   (1u << 0)
#define P10_ACT_STOP_OPEN     (1u << 1)
#define P10_ACT_STOP_CLOSE    (1u << 2)
#define P10_ACT_UNLOCK_VERIFY (1u << 3)   /* hal_pwrmgr_unlock(MOD_USR1) */
#define P10_ACT_UART_OFF      (1u << 4)   /* hal_pwrmgr_unlock(MOD_UART0); if uart_on: hal_uart_deinit */
#define P10_ACT_GPIO_ARM      (1u << 5)   /* pull-up + hal_gpioin_enable(P10) (register as fallback) */
#define P10_ACT_UART_ON_FREE  (1u << 6)   /* hal_uart_init, no lock (SUSPENDED) */
#define P10_ACT_UART_ON_LOCK  (1u << 7)   /* hal_uart_init + hal_pwrmgr_lock(MOD_UART0) (WINDOW) */
#define P10_ACT_LOCK_VERIFY   (1u << 8)   /* hal_pwrmgr_lock(MOD_USR1) */
#define P10_ACT_START_VERIFY  (1u << 9)   /* timer P10_VERIFY_MS */
#define P10_ACT_START_OPEN    (1u << 10)  /* timer out.open_ms */
#define P10_ACT_START_CLOSE   (1u << 11)  /* timer out.close_ms */
#define P10_ACT_ISR_RESET     (1u << 12)  /* clear the burst counter (critical section) */

typedef struct { uint32_t acts; uint32_t open_ms; uint32_t close_ms; } p10_out_t;

typedef struct {
	/* scheduler */
	uint8_t  st;             /* p10_state_t */
	uint8_t  verify_from;    /* P10_ST_WAIT_EDGE or P10_ST_WAIT_OPEN */
	uint8_t  last_missed;    /* 1 after a miss until the next hit (gates edge->T learning) */
	uint8_t  miss_streak;
	uint16_t t_est_ms;
	uint16_t guard_ms;
	uint32_t t_e;            /* stamp of the edge the pending window is anchored to */
	uint32_t state_tick;     /* stamp of the last state change (sanity ageing) */
	uint32_t wait_open_tick; /* stamp when the OPEN timer was armed (sanity ageing across strays) */
	uint32_t win_open_tick;  /* stamp when WINDOW was entered */
	uint32_t anchor_tick;    /* last observed frame start (edge, stray or hit-derived) */
	uint32_t anchor_sec;
	uint8_t  anchor_valid;
	/* telemetry (u16 wrap is fine, consumers diff them) */
	uint16_t edges, glitches, strays, windows, hits, hits_bad, misses;
	uint16_t stale_evt, frame_oos, win_aborted, partial_at_close;
	uint8_t  connects, resumes, recovers, sanity_recovers;   /* saturating */
	uint16_t last_dt_ms;     /* last accepted period sample (edge-to-edge or hit-derived) */
	uint16_t last_hit_pos_ms;/* frame completion relative to window open (ideal = guard + 18) */
	uint16_t rc_ct;          /* last ct used (0 = none/nominal) */
	uint8_t  t_src;          /* 0 seed, 1 hit, 2 edge(k=1), 3 edge(k=2) */
	uint8_t  hist_n;         /* windows in hist (<= 32) */
	uint32_t hist;           /* bit i = outcome of window (now - i); 1 = CRC-good frame */
	uint8_t  health;         /* CRC-good yield % over the last hist_n windows (BTHome 0x09) */
} ucap_p10_t;

static void p10_init(ucap_p10_t *p)
{
	uint8_t i; uint8_t *b = (uint8_t *)p;
	for (i = 0; i < sizeof(*p); i++) b[i] = 0;     /* no <string.h> dependency */
	p->st = P10_ST_WAIT_EDGE;
	p->t_est_ms = P10_T_SEED_MS;
	p->guard_ms = P10_GUARD_BASE_MS;
}

static inline int p10_connected(const ucap_p10_t *p) { return p->st == P10_ST_SUSPENDED; }

static uint32_t p10_window_ms(const ucap_p10_t *p)
{	return 2u * p->guard_ms + P10_FRAME_RX_MS + P10_SLACK_MS; }

static inline void p10_sat8(uint8_t *c) { if (*c < 255) (*c)++; }

/* Delay from `now` to window open for a frame expected at t_e + T_est. */
static uint32_t p10_open_delay_ms(const ucap_p10_t *p, uint32_t now, uint32_t ct)
{
	uint32_t since = p10_ticks_to_ms_rc(p10_dt_ticks(now, p->t_e), ct);
	uint32_t lead = (uint32_t)p->guard_ms + since;
	if (lead + P10_MIN_TIMER_MS >= p->t_est_ms) return P10_MIN_TIMER_MS;  /* degenerate: keep moving */
	return (uint32_t)p->t_est_ms - lead;
}

static void p10_hist_push(ucap_p10_t *p, uint8_t good)
{
	uint32_t ones; uint8_t i;
	p->hist = (p->hist << 1) | (good ? 1u : 0u);
	if (p->hist_n < P10_HIST_N) p->hist_n++;
	for (ones = 0, i = 0; i < p->hist_n; i++) ones += (p->hist >> i) & 1u;
	p->health = (uint8_t)((ones * 100u + p->hist_n / 2u) / p->hist_n);
}

static void p10_anchor_set(ucap_p10_t *p, uint32_t tick, uint32_t sec)
{	p->anchor_tick = tick; p->anchor_sec = sec; p->anchor_valid = 1; }

static int p10_anchor_usable(const ucap_p10_t *p, uint32_t sec)
{	return p->anchor_valid && (sec - p->anchor_sec) <= P10_ANCHOR_MAX_AGE_S; }

/* Learn from an edge-to-edge delta. Only after a miss (the hit path is
 * unambiguous and preferred). k=1 preferred; k=2 only after >= 2 misses. */
static void p10_learn_edge(ucap_p10_t *p, uint32_t dt_ms)
{
	if (!p->last_missed) return;
	if (dt_ms >= P10_T_MIN_MS && dt_ms <= P10_T_MAX_MS) {
		p->t_est_ms = (uint16_t)dt_ms; p->t_src = 2; p->last_dt_ms = (uint16_t)dt_ms; return;
	}
	if (p->miss_streak >= 2 && dt_ms / 2u >= P10_T_MIN_MS && dt_ms / 2u <= P10_T_MAX_MS) {
		p->t_est_ms = (uint16_t)(dt_ms / 2u); p->t_src = 3; p->last_dt_ms = (uint16_t)(dt_ms / 2u);
	}
}

/* Learn from a caught frame: T = ms(t_done - t_e) - FRAME_RX. Bounded by construction. */
static void p10_learn_hit(ucap_p10_t *p, uint32_t t_done, uint32_t ct)
{
	uint32_t t = p10_ticks_to_ms_rc(p10_dt_ticks(t_done, p->t_e), ct);
	if (t > P10_FRAME_RX_MS) t -= P10_FRAME_RX_MS;
	if (t >= P10_T_MIN_MS && t <= P10_T_MAX_MS) { p->t_est_ms = (uint16_t)t; p->t_src = 1; p->last_dt_ms = (uint16_t)t; }
}

static void p10_set_state(ucap_p10_t *p, uint8_t st, uint32_t tick)
{	p->st = st; p->state_tick = tick; }

/* Common teardown to WAIT_EDGE with the pin back on GPIO. */
static uint32_t p10_to_wait_edge(ucap_p10_t *p, uint32_t tick)
{	p10_set_state(p, P10_ST_WAIT_EDGE, tick); return P10_ACT_UART_OFF | P10_ACT_GPIO_ARM | P10_ACT_ISR_RESET; }

static void p10_finish_verify(ucap_p10_t *p, const p10_ev_t *ev, p10_out_t *o)
{
	o->acts |= P10_ACT_UNLOCK_VERIFY | P10_ACT_ISR_RESET;
	if (ev->n >= P10_VERIFY_MIN_EDGES) {
		if (p->verify_from == P10_ST_WAIT_EDGE) {
			p->edges++;
			if (p10_anchor_usable(p, ev->sec))
				p10_learn_edge(p, p10_ticks_to_ms_rc(p10_dt_ticks(ev->t0, p->anchor_tick), ev->ct));
			p->t_e = ev->t0;
			p10_anchor_set(p, ev->t0, ev->sec);
			p10_set_state(p, P10_ST_WAIT_OPEN, ev->tick);
			p->wait_open_tick = ev->tick;
			o->acts |= P10_ACT_START_OPEN;
			o->open_ms = p10_open_delay_ms(p, ev->tick, ev->ct);
		} else {                      /* stray while waiting for the window: anchor, never retarget */
			p->strays++;
			p10_anchor_set(p, ev->t0, ev->sec);
			p10_set_state(p, P10_ST_WAIT_OPEN, ev->tick);   /* OPEN timer still armed */
		}
	} else {
		p->glitches++;
		p10_set_state(p, p->verify_from, ev->tick);
	}
}

static void p10_open_window(ucap_p10_t *p, const p10_ev_t *ev, p10_out_t *o)
{
	p10_set_state(p, P10_ST_WINDOW, ev->tick);
	p->windows++; p->win_open_tick = ev->tick;
	o->acts |= P10_ACT_UART_ON_LOCK | P10_ACT_START_CLOSE;
	o->close_ms = p10_window_ms(p);
}

static void p10_step(ucap_p10_t *p, const p10_ev_t *ev, p10_out_t *o)
{
	o->acts = 0; o->open_ms = 0; o->close_ms = 0;
	if (ev->ct) p->rc_ct = (uint16_t)ev->ct;

	/* state-independent events first */
	if (ev->type == P10_EV_RECOVER) {
		p10_sat8(&p->recovers);
		o->acts = P10_ACT_STOP_VERIFY | P10_ACT_STOP_OPEN | P10_ACT_STOP_CLOSE | P10_ACT_UNLOCK_VERIFY;
		p->anchor_valid = 0;
		if (p->st == P10_ST_SUSPENDED) { o->acts |= P10_ACT_UART_OFF | P10_ACT_UART_ON_FREE | P10_ACT_ISR_RESET; p->state_tick = ev->tick; }
		else o->acts |= p10_to_wait_edge(p, ev->tick);
		return;
	}
	if (ev->type == P10_EV_CONNECT) {
		if (p->st == P10_ST_SUSPENDED) { p->stale_evt++; return; }
		p10_sat8(&p->connects);
		if (p->st == P10_ST_WINDOW) p->win_aborted++;
		o->acts = P10_ACT_STOP_VERIFY | P10_ACT_STOP_OPEN | P10_ACT_STOP_CLOSE | P10_ACT_UNLOCK_VERIFY
		        | P10_ACT_UART_OFF | P10_ACT_UART_ON_FREE | P10_ACT_ISR_RESET;
		p->anchor_valid = 0;
		p10_set_state(p, P10_ST_SUSPENDED, ev->tick);
		return;
	}
	if (ev->type == P10_EV_DISCONNECT) {
		if (p->st != P10_ST_SUSPENDED) { p->stale_evt++; return; }   /* WAITING without CONNECTED: no-op */
		p10_sat8(&p->resumes);
		p->anchor_valid = 0;
		o->acts = p10_to_wait_edge(p, ev->tick);
		return;
	}

	switch (p->st) {
	case P10_ST_WAIT_EDGE:
	case P10_ST_WAIT_OPEN:
		if (ev->type == P10_EV_EDGE) {
			p->verify_from = p->st;
			p10_set_state(p, P10_ST_VERIFY, ev->tick);
			o->acts = P10_ACT_LOCK_VERIFY | P10_ACT_START_VERIFY;
		} else if (ev->type == P10_EV_OPEN && p->st == P10_ST_WAIT_OPEN) {
			p10_open_window(p, ev, o);
		} else if (ev->type == P10_EV_FRAME) {
			p->frame_oos++;
		} else p->stale_evt++;
		break;

	case P10_ST_VERIFY:
		if (ev->type == P10_EV_VERIFY_TO) {
			p10_finish_verify(p, ev, o);
		} else if (ev->type == P10_EV_OPEN && p->verify_from == P10_ST_WAIT_OPEN) {
			/* window due while a burst is being verified: settle the burst with
			 * the live count (ev->n), then open. */
			o->acts |= P10_ACT_STOP_VERIFY;
			p10_finish_verify(p, ev, o);
			p10_open_window(p, ev, o);
		} else p->stale_evt++;      /* EDGE re-post: the ISR keeps counting; nothing to do */
		break;

	case P10_ST_WINDOW:
		if (ev->type == P10_EV_FRAME) {
			p->hits++; if (!ev->ok) p->hits_bad++;
			p->last_hit_pos_ms = (uint16_t)p10_ticks_to_ms(p10_dt_ticks(ev->tick, p->win_open_tick));
			p10_hist_push(p, ev->ok);                       /* health = CRC-good yield */
			p10_learn_hit(p, ev->tick, ev->ct);             /* timing from ANY complete frame */
			p10_anchor_set(p, (ev->tick - p10_ms_to_ticks(P10_FRAME_RX_MS)) & P10_RTC_MASK, ev->sec);
			p->last_missed = 0; p->miss_streak = 0; p->guard_ms = P10_GUARD_BASE_MS;
			o->acts = P10_ACT_STOP_CLOSE | p10_to_wait_edge(p, ev->tick);
		} else if (ev->type == P10_EV_CLOSE) {
			p->misses++; p10_hist_push(p, 0);
			if (ev->n) p->partial_at_close++;
			p->last_missed = 1; if (p->miss_streak < 255) p->miss_streak++;
			{	/* guard ladder: x2 every P10_MISS_WIDEN consecutive misses, capped */
				uint32_t g = P10_GUARD_BASE_MS; uint8_t k = p->miss_streak / P10_MISS_WIDEN;
				while (k-- && g < P10_GUARD_MAX_MS) g *= 2u;
				p->guard_ms = (uint16_t)g;
			}
			o->acts = p10_to_wait_edge(p, ev->tick);
		} else p->stale_evt++;
		break;

	case P10_ST_SUSPENDED:
		if (ev->type == P10_EV_FRAME) { /* data path already updated by the ISR; nothing to schedule */ }
		else p->stale_evt++;
		break;
	}
}

/* Sanity sweep (task context, every advertising event). Returns 1 if the
 * caller must dispatch P10_EV_RECOVER: the state has outlived its timer. */
static int p10_sanity_due(ucap_p10_t *p, uint32_t now)
{
	uint32_t age, lim, since = p->state_tick;
	switch (p->st) {
	case P10_ST_VERIFY:    lim = P10_SANITY_VERIFY_MS; break;
	case P10_ST_WINDOW:    lim = P10_SANITY_WINDOW_MS; break;
	case P10_ST_WAIT_OPEN: lim = P10_SANITY_WAIT_OPEN_MS; since = p->wait_open_tick; break;
	default: return 0;
	}
	age = p10_ticks_to_ms(p10_dt_ticks(now, since));
	if (age <= lim) return 0;
	p10_sat8(&p->sanity_recovers);
	return 1;
}
#endif /* _UCAP_P10_H_ */
