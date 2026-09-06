/******************************************************************************
 * ucap_p03.h — "P03 wake" receiver for the IBSTH2P inter-chip UART (V26_P03).
 * Pure C, no SDK includes; host-tested by tests/test_ucap_p03.c.
 *
 * Stock-firmware mechanism (see freezer_battery_data/v25_p10_design/
 * STOCK_FIRMWARE_WAKE_MECHANISM.md): the main MCU raises P03 before it sends
 * a frame. We keep UART0 initialised across sleep (the SDK re-inits it in the
 * wake hook), sleep with P03 armed as a rising-edge wake/IRQ, take the
 * MOD_UART0 sleep-lock on the edge (or on the first RX byte / any IO wake if
 * the edge was missed), and release it when a complete frame (CRC-good or
 * CRC-bad) has been received or a short timeout expires. No prediction, no
 * windows, every frame. Unlike stock, release never depends on a byte count.
 *
 * Measurement (the point of this build): per frame, lead = first UART byte -
 * P03 rise, high = P03 fall - rise, wake_lat = IRQ-live - wake stamp.
 * All stamps are 24-bit RTC ticks (32768 Hz nominal); deltas masked here.
 ******************************************************************************/
#ifndef _UCAP_P03_H_
#define _UCAP_P03_H_
#include <stdint.h>

#define P03_TIMEOUT_MS        250u   /* lock held at most this long without a complete frame */
#define P03_SANITY_ARMED_MS  2000u   /* ARMED older than this at the adv sweep = lost timer */
#define P03_RTC_MASK      0xFFFFFFu
#define P03_HIST_N             8u    /* lead histogram bins (0.1 ms units): <10,<20,<30,<50,<80,<120,<200,>=200 */

static inline uint32_t p03_dt(uint32_t later, uint32_t earlier) { return (later - earlier) & P03_RTC_MASK; }
/* ticks -> tenths of a millisecond: t * 10000/32768 = t*625/2048. Split so a full 24-bit t cannot overflow. */
static inline uint32_t p03_ticks_to_100us(uint32_t t) { return ((t >> 11) * 625u) + (((t & 2047u) * 625u) >> 11); }
#define P03_FIRST_BYTE_100US  10u   /* the first-byte stamp is taken when byte 1 has been RECEIVED (1-char FIFO trigger, uart.h):
                                       start bit -> stamp = 10 bits / 9600 baud = 1.04 ms; subtracted from the lead */
#define P03_LEAD_MAX_100US  5000u   /* leads above 500 ms are not a P03 -> frame relation: counted as no-edge */
static inline uint32_t p03_ticks_to_ms(uint32_t t) { return (t * 125u) >> 12; }

typedef enum { P03_ST_IDLE = 0, P03_ST_ARMED, P03_ST_SUSPENDED } p03_state_t;

typedef enum {
	P03_EV_RISE = 0,   /* P03 rising edge seen (IRQ callback, synthesised at wake, or wake-hook IO wake) ; ev.t0 = stamp, ev.src */
	P03_EV_RX_START,   /* first UART byte of a burst ; ev.t0 = stamp */
	P03_EV_FRAME,      /* complete 13-byte buffer ; ev.ok = CRC valid ; ev.t0 = completion stamp */
	P03_EV_TIMEOUT,    /* timeout timer expired */
	P03_EV_CONNECT,
	P03_EV_DISCONNECT,
	P03_EV_RECOVER
} p03_event_t;

/* ev.src for RISE */
#define P03_SRC_IRQ     0  /* GPIO IRQ callback while awake or re-armed after wake */
#define P03_SRC_WAKE    1  /* IO wake in the MOD_USR1 hook with P03 read HIGH (pulse still on): stamp = g_wakeup_rtc_tick */
#define P03_SRC_UNKNOWN 2  /* IO wake with P03 LOW: P10 start bit or a P03 pulse already over. Locks, but is NOT a lead anchor */

typedef struct { p03_event_t type; uint32_t tick; uint32_t t0; uint8_t ok; uint8_t src; } p03_ev_t;

/* actions; the glue executes them in this fixed order: STOP_TO -> UNLOCK -> LOCK -> START_TO -> BURST_RST
 * (so STOP_TO|START_TO re-arms the timer and UNLOCK|LOCK ends locked). Bit values carry no ordering. */
#define P03_ACT_LOCK      (1u << 0)   /* hal_pwrmgr_lock(MOD_UART0) (idempotent flag) */
#define P03_ACT_UNLOCK    (1u << 1)   /* hal_pwrmgr_unlock(MOD_UART0) */
#define P03_ACT_START_TO  (1u << 2)   /* osal timer P03_TIMEOUT_MS */
#define P03_ACT_STOP_TO   (1u << 3)
#define P03_ACT_BURST_RST (1u << 4)   /* clear the ISR first-byte latch */

typedef struct { uint32_t acts; } p03_out_t;

typedef struct {
	uint8_t  st;
	uint8_t  have_rise;      /* a rise stamp exists for the pending frame */
	uint8_t  have_first;     /* first-byte stamp exists for the pending frame */
	uint8_t  rise_src;
	uint32_t t_rise, t_first, state_tick;
	/* telemetry */
	uint16_t rises, rises_wake, rx_starts, rx_no_edge, frames, frames_bad, frames_no_edge, timeouts;
	uint16_t stale, connects, resumes, recovers, sanity_recovers, wakes_unknown, rise_after_rx, lead_rejected;
	uint16_t frames_bad_armed;   /* CRC-bad completions that kept the lock (see ARMED/FRAME) */
	uint16_t lead_last, lead_min, lead_max;   /* 0.1 ms; min=0xFFFF until first */
	uint16_t done_last;                       /* frame completion - first byte, 0.1 ms (frame duration incl. FIFO latency) */
	uint8_t  hist[P03_HIST_N];                /* saturating */
	uint8_t  lead_ms_last;                    /* BTHome 0x09 (255 = none yet / >254) */
} ucap_p03_t;

static void p03_init(ucap_p03_t *p)
{
	uint8_t i; uint8_t *b = (uint8_t *)p;
	for (i = 0; i < sizeof(*p); i++) b[i] = 0;
	p->st = P03_ST_IDLE; p->lead_min = 0xFFFF; p->lead_ms_last = 255;
}
static inline int p03_connected(const ucap_p03_t *p) { return p->st == P03_ST_SUSPENDED; }
static inline void p03_sat16(uint16_t *c) { if (*c < 0xFFFF) (*c)++; }
static inline void p03_sat8(uint8_t *c) { if (*c < 255) (*c)++; }

static void p03_hist_push(ucap_p03_t *p, uint32_t lead_100us)
{
	static const uint16_t edges[P03_HIST_N - 1] = { 10, 20, 30, 50, 80, 120, 200 };
	uint8_t i;
	for (i = 0; i < P03_HIST_N - 1; i++) if (lead_100us < edges[i]) break;
	p03_sat8(&p->hist[i]);
}

static void p03_record_frame(ucap_p03_t *p, const p03_ev_t *ev)
{
	p03_sat16(&p->frames);
	if (!ev->ok) p03_sat16(&p->frames_bad);
	if (p->have_rise && p->have_first
	    && p03_ticks_to_100us(p03_dt(p->t_first, p->t_rise)) <= P03_LEAD_MAX_100US) {
		uint32_t lead = p03_ticks_to_100us(p03_dt(p->t_first, p->t_rise));
		lead = (lead > P03_FIRST_BYTE_100US) ? lead - P03_FIRST_BYTE_100US : 0;   /* stamp is end of byte 1 */
		p->lead_last = (uint16_t)lead;
		if (lead < p->lead_min) p->lead_min = (uint16_t)lead;
		if (lead > p->lead_max) p->lead_max = (uint16_t)lead;
		p03_hist_push(p, lead);
		p->lead_ms_last = (uint8_t)((lead / 10u) > 254u ? 254u : (lead / 10u));
	} else {
		if (p->have_rise && p->have_first) p03_sat16(&p->lead_rejected);   /* rise/first order or gap implausible */
		p03_sat16(&p->frames_no_edge);
	}
	if (p->have_first) {
		uint32_t d = p03_ticks_to_100us(p03_dt(ev->t0, p->t_first));
		p->done_last = (uint16_t)(d > 0xFFFF ? 0xFFFF : d);
	}
}

static uint32_t p03_to_idle(ucap_p03_t *p, uint32_t tick)
{
	p->st = P03_ST_IDLE; p->state_tick = tick;
	p->have_rise = 0; p->have_first = 0;
	return P03_ACT_UNLOCK | P03_ACT_STOP_TO | P03_ACT_BURST_RST;
}

static void p03_step(ucap_p03_t *p, const p03_ev_t *ev, p03_out_t *o)
{
	o->acts = 0;
	if (ev->type == P03_EV_RECOVER) {
		p03_sat16(&p->recovers);
		if (p->st == P03_ST_SUSPENDED) { o->acts = P03_ACT_UNLOCK | P03_ACT_STOP_TO | P03_ACT_BURST_RST; p->have_rise = p->have_first = 0; }
		else o->acts = p03_to_idle(p, ev->tick);
		return;
	}
	if (ev->type == P03_EV_CONNECT) {
		if (p->st == P03_ST_SUSPENDED) { p03_sat16(&p->stale); return; }
		p03_sat16(&p->connects);
		p->st = P03_ST_SUSPENDED; p->state_tick = ev->tick; p->have_rise = p->have_first = 0;
		o->acts = P03_ACT_UNLOCK | P03_ACT_STOP_TO | P03_ACT_BURST_RST;   /* MOD_USR0 keeps the chip awake */
		return;
	}
	if (ev->type == P03_EV_DISCONNECT) {
		if (p->st != P03_ST_SUSPENDED) { p03_sat16(&p->stale); return; }
		p03_sat16(&p->resumes);
		o->acts = p03_to_idle(p, ev->tick);
		return;
	}
	switch (p->st) {
	case P03_ST_IDLE:
		if (ev->type == P03_EV_RISE) {
			if (ev->src == P03_SRC_UNKNOWN) {          /* IO wake, P03 low: hold the chip awake but do not anchor a lead */
				p03_sat16(&p->wakes_unknown);
				p->have_rise = 0;
			} else {
				p03_sat16(&p->rises); if (ev->src == P03_SRC_WAKE) p03_sat16(&p->rises_wake);
				p->have_rise = 1; p->t_rise = ev->t0; p->rise_src = ev->src;
			}
			p->have_first = 0;
			p->st = P03_ST_ARMED; p->state_tick = ev->tick;
			o->acts = P03_ACT_LOCK | P03_ACT_START_TO;
		} else if (ev->type == P03_EV_RX_START) {
			p03_sat16(&p->rx_starts); p03_sat16(&p->rx_no_edge);
			p->have_first = 1; p->t_first = ev->t0; p->have_rise = 0;
			p->st = P03_ST_ARMED; p->state_tick = ev->tick;
			o->acts = P03_ACT_LOCK | P03_ACT_START_TO;
		} else if (ev->type == P03_EV_FRAME) {          /* no edge, no rx-start seen: record and stay unlocked */
			p03_record_frame(p, ev);
			o->acts = P03_ACT_UNLOCK | P03_ACT_BURST_RST;
		} else p03_sat16(&p->stale);                  /* TIMEOUT in IDLE */
		break;
	case P03_ST_ARMED:
		if (ev->type == P03_EV_RX_START) {
			p03_sat16(&p->rx_starts);
			if (!p->have_first) { p->have_first = 1; p->t_first = ev->t0; }
		} else if (ev->type == P03_EV_RISE) {
			if (ev->src == P03_SRC_UNKNOWN) { p03_sat16(&p->wakes_unknown); }
			else {
				p03_sat16(&p->rises); if (ev->src == P03_SRC_WAKE) p03_sat16(&p->rises_wake);
				if (p->have_first) p03_sat16(&p->rise_after_rx);      /* edge after the first byte: not a lead anchor */
				else if (!p->have_rise) { p->have_rise = 1; p->t_rise = ev->t0; p->rise_src = ev->src; }
				else p03_sat16(&p->stale);                            /* second rise before the frame */
			}
		} else if (ev->type == P03_EV_FRAME) {
			p03_record_frame(p, ev);
			if (ev->ok) {
				o->acts = p03_to_idle(p, ev->tick);
			} else {
				p03_sat16(&p->frames_bad_armed);
				/* A CRC-bad completion is often a stale partial buffer completed by the
				 * bytes of the frame now arriving: bytes may still be in flight, so keep
				 * the lock, re-arm the timeout (bounded), and let the good frame or the
				 * timeout end the burst. The ISR latch is left alone on purpose. */
				p->have_first = 0;
				o->acts = P03_ACT_STOP_TO | P03_ACT_START_TO;
			}
		} else if (ev->type == P03_EV_TIMEOUT) {
			p03_sat16(&p->timeouts);
			o->acts = p03_to_idle(p, ev->tick);
		}
		break;
	case P03_ST_SUSPENDED:
		if (ev->type == P03_EV_FRAME) { p03_sat16(&p->frames); if (!ev->ok) p03_sat16(&p->frames_bad); o->acts = P03_ACT_BURST_RST; }
		else if (ev->type == P03_EV_RISE) { if (ev->src != P03_SRC_UNKNOWN) p03_sat16(&p->rises); o->acts = P03_ACT_BURST_RST; } /* count; re-open the ISR latch */
		else if (ev->type == P03_EV_RX_START) { p03_sat16(&p->rx_starts); }               /* FRAME resets the latch */
		else p03_sat16(&p->stale);
		break;
	}
}

/* adv_measure sweep: 1 = dispatch RECOVER */
static int p03_sanity_due(ucap_p03_t *p, uint32_t now)
{
	if (p->st != P03_ST_ARMED) return 0;
	if (p03_ticks_to_ms(p03_dt(now, p->state_tick)) <= P03_SANITY_ARMED_MS) return 0;
	p03_sat16(&p->sanity_recovers);
	return 1;
}
#endif /* _UCAP_P03_H_ */
