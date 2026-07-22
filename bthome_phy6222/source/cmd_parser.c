/*
 * cmd_parser.c
 *
 *  Created on: 16 01 2024
 *      Author: pvvx
 */

/*********************************************************************
	INCLUDES
*/
#include "bcomdef.h"
#include "config.h"
#include "OSAL.h"
#include "linkdb.h"
#include "att.h"
#include "gatt.h"
#include "gatt_uuid.h"
#include "gattservapp.h"
#include "gapbondmgr.h"
#include "flash.h"
#include "flash_eep.h"
#include "thb2_main.h"
#include "sbp_profile.h"
#include "sensors.h"
#include "cmd_parser.h"
#include "devinfoservice.h"
#include "gapgattserver.h"
#include "ble_ota.h"
#include "thb2_peripheral.h"
#include "lcd.h"
#include "logger.h"
#include "trigger.h"
#include "buzzer.h"
#include "bthome_beacon.h"
#include "findmy_beacon.h"
#include "gpio.h"
#include "dev_i2c.h"
#include "uart.h"
#include "pwrmgr.h"
/*********************************************************************/

extern gapPeriConnectParams_t periConnParameters;

#define SEND_DATA_SIZE	16

#if DEVICE == DEVICE_IBSTH2P
// ============================================================
// V10: Inter-chip UART frame parser
// ============================================================
// The main MCU sends 13-byte frames on UART0 (TX=P09, RX=P10, 9600 baud):
//   'R' + 11 data bytes + 'E'
//
// Frame layout (0-indexed within the 13-byte frame), semantics confirmed by
// disassembling the stock PHY6222 firmware's parser (see "Stock inter-chip
// UART protocol" in IBSTH2P_PROJECT_PLAN.md):
//   [0]    = 0x52 'R' start marker
//   [1-2]  = type-dependent payload (temperature for frame types 0/2/3)
//   [3-4]  = temperature (LE int16, x0.01 °C) for frame type 1
//   [5-6]  = humidity (LE int16, x0.01 %)
//   [7]    = frame type: 1 = periodic measurement (the normal stream);
//            0/2/3 = variants with temperature in bytes [1-2]
//   [8]    = BLE enable state from the device button: 1 = on, 0 = off.
//            Stock firmware applies this to GAPROLE_ADVERT_ENABLED on every
//            valid frame (and drops a live connection when it goes to 0).
//   [9]    = battery percent computed by the main MCU (0x50-0x55 = 80-85%)
//   [10-11]= CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF) over
//            bytes [1..9], stored little-endian
//   [12]   = 0x45 'E' end marker
//
// Reverse channel (stock firmware only, not used here): PHY -> main MCU
// frames are 'S' + 9 bytes + 'E' with the same CRC over the first 7 payload
// bytes; the stock firmware raises a GPIO for ~300 us before transmitting
// to wake the main MCU's UART receiver.

#include "ucap_frame.h"
#ifdef UCAP_SYNC
#include "ucap_sync.h"
#endif

#define FRAME_SIZE  UCAP_FRAME_SIZE

// Raw byte capture — records first RAWCAP_SIZE bytes for debugging
#define RAWCAP_SIZE 64
static uint8_t rawcap_buf[RAWCAP_SIZE];
static volatile uint8_t rawcap_pos = 0;

#if defined(UCAP_PROBE) || defined(UCAP_SYNC)
// Raw RTC counter read (AP_AON->RTCCNT, 24-bit @32768 Hz, wraps every
// 512 s — delta consumers handle wrap with 24-bit modular arithmetic).
// Self-contained and ISR-safe; same double-read pattern as the reference
// in config.h.
static uint32_t ucap_rtc(void) {
	uint32_t t;
	do {
		t = *(volatile uint32_t *)0x4000f028;
	} while (t != *(volatile uint32_t *)0x4000f028);
	return t & 0xFFFFFF;
}
#endif

#ifdef UCAP_PROBE
// Frame-period probe: timestamp every valid frame so inter-frame deltas
// and the button byte can be read out over BLE (debug cmd op 4, plus a
// live notification per frame — see probe_make_msg / probe_notify).
// Ring of the last PROBE_LOG_SIZE frames; probe_cnt is the monotonic
// total, so entry i lives at probe_log[i % PROBE_LOG_SIZE].
#define PROBE_LOG_SIZE 64
typedef struct __attribute__((packed)) {
	uint32_t tik;    // RTC count at frame validation, 32768 Hz, 24 bit
	uint8_t  type7;  // frame[7]: frame type
	uint8_t  flags8; // frame[8]: button-latched BLE state
} probe_rec_t;
static probe_rec_t probe_log[PROBE_LOG_SIZE];
static volatile uint16_t probe_cnt = 0;
#define probe_rtc ucap_rtc
#endif // UCAP_PROBE

typedef struct {
	ucap_frame_t fr;   // framing state machine (see ucap_frame.h)

	// Stats
	volatile uint32_t total_bytes;
	volatile uint16_t good_frames;

	// Latest parsed sensor values (updated in ISR)
	volatile int16_t  last_temp;     // x0.01 °C
	volatile int16_t  last_humi;     // x0.01 %  (from frame[5-6])
	volatile uint8_t  last_byte1;    // frame[1] for debugging
	volatile uint8_t  last_byte2;    // frame[2] for debugging
	volatile uint8_t  last_byte9;    // frame[9] for debugging (unknown)
	volatile uint8_t  last_flags7;   // frame[7]
	volatile uint8_t  last_flags8;   // frame[8]

	volatile uint8_t  sensor_valid;
	// 1 while a grab window is open and the UART power lock is held.
	// Cleared (and the lock released) by the first good frame, so the
	// chip sleeps for the rest of the window instead of idling awake.
	volatile uint8_t  grab_active;
	uint8_t uart_inited;

	// Device-button tracking. Frame byte [8] is the button-driven BLE
	// enable state on the stock firmware (see protocol notes above); it
	// toggles on each physical button press. We only observe the latched
	// level during grab windows, so we count net changes: a single press
	// between windows registers as one click. btn_known suppresses a
	// phantom click on the very first frame after boot.
	volatile uint8_t  btn_known;
	volatile uint8_t  btn_last;      // last observed byte [8] level
	volatile uint32_t btn_clicks;    // monotonic count of detected changes

#ifdef UCAP_SYNC
	// Frame-synced scheduling (wake-on-RX, see ucap_sync.h). The ISR
	// timestamps each good frame and precomputes the delta to the previous
	// one; the task-context handlers below turn that into listen windows.
	volatile uint32_t last_tik;      // RTC ticks of the last good frame
	volatile uint32_t frame_dt_ms;   // delta to the previous good frame
	volatile uint8_t  have_prev;     // 0 -> frame_dt_ms is DT_UNKNOWN
	ucap_sync_t sync;
#endif
} uart_capture_t;

uart_capture_t ucap;

static uart_Cfg_t ucap_uart_cfg;  // saved for re-init after sleep

// Consume a frame already validated (marker + CRC) by ucap_frame_feed().
static void ucap_process_frame(void) {
	uint8_t *f = ucap.fr.buf;

	ucap.good_frames++;
	ucap.sensor_valid = 1;

	// Temperature: bytes 3-4, LE int16, x0.01 °C
	ucap.last_temp = (int16_t)(f[3] | (f[4] << 8));

	// Humidity: bytes 5-6, LE int16, x0.01 %
	ucap.last_humi = (int16_t)(f[5] | (f[6] << 8));

	// Debug fields
	ucap.last_byte1 = f[1];
	ucap.last_byte2 = f[2];
	ucap.last_byte9 = f[9];
	ucap.last_flags7 = f[7];
	ucap.last_flags8 = f[8];

	// Detect device-button activity from the latched byte [8] level.
	if (!ucap.btn_known) {
		ucap.btn_known = 1;
		ucap.btn_last = f[8];
	} else if (f[8] != ucap.btn_last) {
		ucap.btn_last = f[8];
		ucap.btn_clicks++;
	}

#ifdef UCAP_PROBE
	// Log the frame and wake the app task to stream it over BLE if a
	// client is listening (osal_set_event is ISR-safe).
	{
		probe_rec_t *r = &probe_log[probe_cnt % PROBE_LOG_SIZE];
		r->tik = probe_rtc();
		r->type7 = f[7];
		r->flags8 = f[8];
		probe_cnt++;
		osal_set_event(simpleBLEPeripheral_TaskID, SBP_PROBE_EVT);
	}
	// Probe build: keep the UART power lock permanently — the whole point
	// is to observe every frame, not just grab windows.
#else
	// Got a fresh frame: release the UART power lock now so the chip can
	// sleep for the remainder of the grab window. read_sensors() will
	// copy the captured values at the end of the window as before.
	// (hal_pwrmgr_unlock is flag-based and safe from IRQ context.)
	if (ucap.grab_active) {
		ucap.grab_active = 0;
		hal_pwrmgr_unlock(MOD_UART0);
	}
#ifdef UCAP_SYNC
	// Timestamp the frame and hand scheduling off to task context.
	{
		uint32_t t = ucap_rtc();
		if (ucap.have_prev) {
			uint32_t dt = (t - ucap.last_tik) & 0xFFFFFF; // 24-bit RTC
			ucap.frame_dt_ms = (dt * 125u) >> 12;         // ticks -> ms
		} else {
			ucap.frame_dt_ms = UCAP_SYNC_DT_UNKNOWN;
			ucap.have_prev = 1;
		}
		ucap.last_tik = t;
		osal_set_event(simpleBLEPeripheral_TaskID, SBP_UCAP_FRAME_EVT);
	}
#endif
#endif
}

static void ucap_callback(uart_Evt_t *pev) {
	if (pev->type != UART_EVT_TYPE_RX_DATA && pev->type != UART_EVT_TYPE_RX_DATA_TO)
		return;

	for (int i = 0; i < pev->len; i++) {
		uint8_t b = pev->data[i];
		ucap.total_bytes++;

		// Capture raw bytes (first RAWCAP_SIZE only)
		if (rawcap_pos < RAWCAP_SIZE) {
			rawcap_buf[rawcap_pos++] = b;
		}

		if (ucap_frame_feed(&ucap.fr, b))
			ucap_process_frame();
	}
}

int ucap_init(void) {
	memset(&ucap, 0, sizeof(ucap));
	memset(rawcap_buf, 0, sizeof(rawcap_buf));
	rawcap_pos = 0;
#ifdef UCAP_SYNC
	ucap_sync_init(&ucap.sync);
#endif

	uart_Cfg_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.tx_pin = GPIO_DUMMY;
	cfg.rx_pin = GPIO_P10;
	cfg.baudrate = 9600;
	cfg.use_fifo = TRUE;
	cfg.hw_fwctrl = FALSE;
	cfg.use_tx_buf = FALSE;
	cfg.parity = FALSE;
	cfg.evt_handler = ucap_callback;

	memcpy(&ucap_uart_cfg, &cfg, sizeof(cfg));  // save for re-init

	int ret = hal_uart_init(cfg, UART0);
	ucap.uart_inited = (ret == 0) ? 1 : 0;

	if (ret == 0) {
		// Lock UART to capture frames immediately on boot.
		// Unlocked by the first good frame, or at the first
		// read_sensors() call (~10 sec) as a fallback.
		hal_pwrmgr_lock(MOD_UART0);
		ucap.grab_active = 1;
	}

	return ret;
}

// Monotonic count of detected device-button changes. The advertising loop
// compares this against its own last-seen value to fire a BTHome button
// press event; see adv_measure() in thb2_main.c.
uint32_t ucap_get_clicks(void) {
	return ucap.btn_clicks;
}

// Re-init UART hardware and lock to prevent sleep during grab window.
// Called from start_measure() one adv cycle before read_sensors().
void ucap_start_grab(void) {
#ifdef UCAP_PROBE
	// Probe build: UART is permanently powered and locked since ucap_init;
	// a deinit/init here would only risk corrupting an in-flight frame.
	return;
#endif
	if (ucap.uart_inited) {
		// Window already open (boot acquisition, or a UCAP_SYNC listen
		// window): a deinit here would drop an in-flight frame.
		if (ucap.grab_active)
			return;
		// After sleep, UART hardware is powered down.
		// Lock alone doesn't restore it — must deinit+init to reconfigure.
		hal_uart_deinit(UART0);
		hal_uart_init(ucap_uart_cfg, UART0);
		hal_pwrmgr_lock(MOD_UART0);
		// Open the grab window last: the first good frame after this
		// point releases the lock early (see ucap_process_frame).
		ucap.grab_active = 1;
	}
}

// Copy captured sensor data into measured_data (called from read_sensors),
// then unlock UART so the chip can sleep until the next grab.
void ucap_update_measured_data(void) {
	if (ucap.sensor_valid) {
		measured_data.temp = ucap.last_temp;           // x0.01 °C
		measured_data.humi = ucap.last_humi;           // x0.01 % (already in correct unit)
	} else {
		// No valid frame yet — report negative debug counters
		measured_data.temp = -(int16_t)(ucap.good_frames ? ucap.good_frames : 1);
		measured_data.humi = -(int16_t)(ucap.fr.crc_bad ? ucap.fr.crc_bad : 1);
	}
#if !defined(UCAP_PROBE) && !defined(UCAP_SYNC)
	// Close the grab window. Normally the first good frame already
	// released the UART lock; this is the fallback for a window with no
	// frames (unlock of an already-unlocked module is a harmless no-op).
	// (Not under UCAP_SYNC: the sync engine owns window close, and this
	// runs every advertising event — it must not slam a window that was
	// just opened for the upcoming frame.)
	if (ucap.uart_inited) {
		ucap.grab_active = 0;
		hal_pwrmgr_unlock(MOD_UART0);
	}
#endif
}

#ifdef UCAP_SYNC
/*
 * Wake-on-RX scheduling (V21). The scheduler arithmetic lives in
 * ucap_sync.h (host-tested); these handlers own the OSAL timers and the
 * UART window, and run in task context off three events:
 *
 *   SBP_UCAP_FRAME_EVT  a good frame arrived (ISR already released the
 *                       UART lock and precomputed frame_dt_ms)
 *   SBP_UCAP_OPEN_EVT   time to open the next listen window
 *   SBP_UCAP_CLOSE_EVT  window timeout (a miss if the window is still open)
 *
 * Boot: ucap_init() opens the acquisition window; simpleBLEPeripheral_Init
 * arms a CLOSE timeout for it, so a silent main MCU degrades into the
 * miss/backoff path instead of holding the UART lock forever.
 */
void ucap_sync_frame_evt(void) {
	// The frame beat any pending window timeout; it's not a miss.
	osal_stop_timerEx(simpleBLEPeripheral_TaskID, SBP_UCAP_CLOSE_EVT);
	osal_start_timerEx(simpleBLEPeripheral_TaskID, SBP_UCAP_OPEN_EVT,
			ucap_sync_on_frame(&ucap.sync, ucap.frame_dt_ms));
}

void ucap_sync_open_evt(void) {
	ucap_start_grab();
	osal_start_timerEx(simpleBLEPeripheral_TaskID, SBP_UCAP_CLOSE_EVT,
			ucap_sync_window_ms(&ucap.sync));
}

void ucap_sync_close_evt(void) {
	uint32_t delay;
	// If the frame arrived between the timer firing and this handler
	// running, the ISR already closed the window and queued FRAME_EVT.
	if (!ucap.grab_active)
		return;
	ucap.grab_active = 0;
	hal_pwrmgr_unlock(MOD_UART0);
	// The next frame's delta will span at least one unobserved period.
	ucap.have_prev = 0;
	delay = ucap_sync_on_miss(&ucap.sync);
	if (delay == 0)
		ucap_sync_open_evt(); // reacquire: reopen immediately, full period
	else
		osal_start_timerEx(simpleBLEPeripheral_TaskID, SBP_UCAP_OPEN_EVT, delay);
}
#endif // UCAP_SYNC

#ifdef UCAP_PROBE
// Build the live probe notification for the most recent frame:
// [0]=CMD_ID_I2C_SCAN, [1]=0x51 (live marker), [2..3]=probe_cnt u16 LE,
// [4..7]=tik u32 LE (24-bit RTC @32768 Hz), [8]=type7, [9]=flags8,
// [10..11]=delta to previous frame in ms u16 LE (0xFFFF = unknown/>65 s).
// Called from task context (SBP_PROBE_EVT); returns 0 if nothing logged.
uint8_t probe_make_msg(uint8_t *pbuf) {
	uint16_t cnt = probe_cnt;
	if (cnt == 0)
		return 0;
	probe_rec_t *r = &probe_log[(uint16_t)(cnt - 1) % PROBE_LOG_SIZE];
	uint32_t dt_ms = 0xFFFF;
	if (cnt >= 2) {
		probe_rec_t *p = &probe_log[(uint16_t)(cnt - 2) % PROBE_LOG_SIZE];
		uint32_t dt = (r->tik - p->tik) & 0xFFFFFF; // wrap-safe, 24 bit
		if (dt < ((uint32_t)65 << 15)) // < 65 s: *1000 cannot overflow
			dt_ms = (dt * 1000) >> 15;
	}
	pbuf[0] = CMD_ID_I2C_SCAN;
	pbuf[1] = 0x51;
	pbuf[2] = cnt & 0xFF;
	pbuf[3] = (cnt >> 8) & 0xFF;
	pbuf[4] = r->tik & 0xFF;
	pbuf[5] = (r->tik >> 8) & 0xFF;
	pbuf[6] = (r->tik >> 16) & 0xFF;
	pbuf[7] = 0;
	pbuf[8] = r->type7;
	pbuf[9] = r->flags8;
	pbuf[10] = dt_ms & 0xFF;
	pbuf[11] = (dt_ms >> 8) & 0xFF;
	return 12;
}
#endif // UCAP_PROBE

#else // !DEVICE_IBSTH2P — V7 scan infrastructure for other devices

enum {
	SCAN_OP_START = 0,
	SCAN_OP_GET_CHUNK = 1,
	SCAN_OP_GET_SUMMARY = 2,
	SCAN_OP_ABORT = 3
};

enum {
	SCAN_STATUS_OK = 0,
	SCAN_STATUS_BUSY = 1,
	SCAN_STATUS_BAD_ARG = 2,
	SCAN_STATUS_TIMEOUT = 3,
	SCAN_STATUS_ABORTED = 4,
	SCAN_STATUS_INTERNAL = 5
};

enum {
	SCAN_PHASE_IDLE = 0,
	SCAN_PHASE_GPIO = 1,
	SCAN_PHASE_I2C = 2,
	SCAN_PHASE_UART = 3,
	SCAN_PHASE_SPI = 4,
	SCAN_PHASE_IDENTIFY = 5,
	SCAN_PHASE_DONE = 6
};

enum {
	SCAN_REC_PIN_FP = 1,
	SCAN_REC_I2C_ACK = 2,
	SCAN_REC_UART_STAT = 3,
	SCAN_REC_SPI_STAT = 4,
	SCAN_REC_ID_MATCH = 5,
	SCAN_REC_SUMMARY = 6
};

#define SCAN_MAX_CHUNKS		60
#define SCAN_MAX_PAYLOAD	24

typedef struct {
	uint8_t record_type;
	uint8_t record_count;
	uint8_t payload_len;
	uint8_t payload[SCAN_MAX_PAYLOAD];
} scan_chunk_t;

typedef struct {
	uint8_t active;
	uint8_t mode;
	uint8_t scan_id;
	uint8_t phase;
	uint8_t total_chunks;
	uint8_t summary_chunk;
	scan_chunk_t chunks[SCAN_MAX_CHUNKS];
} scan_state_t;

static scan_state_t g_scan_state;

// Forward declarations
static void scan_add_chunk(uint8_t rec_type, uint8_t rec_count, const uint8_t *payload, uint8_t payload_len);

// Frame-comparing UART listener (V7)
#define UART_FRAME_MARKER_MIN 0xa0
#define FCOMP_MAX_FRAME_LEN 24
#define FCOMP_MAX_DIFFS 4

typedef struct {
	uint8_t ref_frame[FCOMP_MAX_FRAME_LEN];
	uint8_t ref_len;
	uint8_t diff_frames[FCOMP_MAX_DIFFS][FCOMP_MAX_FRAME_LEN];
	uint8_t diff_lens[FCOMP_MAX_DIFFS];
	uint16_t diff_at[FCOMP_MAX_DIFFS];
	uint8_t cur_frame[FCOMP_MAX_FRAME_LEN];
	uint8_t cur_pos;
	uint16_t total_frames;
	uint8_t num_diffs;
	uint8_t in_frame;
	volatile uint8_t frame_ready; // set by ISR when a full frame completes
	uint8_t last_frame[FCOMP_MAX_FRAME_LEN]; // copy of last completed frame
	uint8_t last_len;
} fcomp_state_t;

static volatile fcomp_state_t fcomp;

static void fcomp_process_frame(void) {
	uint8_t pos = fcomp.cur_pos;
	if (pos < 3) return;
	// Save a copy of the just-completed frame
	memcpy((void*)fcomp.last_frame, (void*)fcomp.cur_frame, pos);
	fcomp.last_len = pos;
	fcomp.frame_ready = 1;
	fcomp.total_frames++;
	if (fcomp.ref_len == 0) {
		memcpy((void*)fcomp.ref_frame, (void*)fcomp.cur_frame, pos);
		fcomp.ref_len = pos;
	} else if (fcomp.num_diffs < FCOMP_MAX_DIFFS) {
		uint8_t differs = (pos != fcomp.ref_len);
		if (!differs)
			differs = memcmp((void*)fcomp.ref_frame, (void*)fcomp.cur_frame, pos) != 0;
		if (differs) {
			uint8_t idx = fcomp.num_diffs;
			memcpy((void*)fcomp.diff_frames[idx], (void*)fcomp.cur_frame, pos);
			fcomp.diff_lens[idx] = pos;
			fcomp.diff_at[idx] = fcomp.total_frames;
			fcomp.num_diffs++;
		}
	}
}

static void fcomp_callback(uart_Evt_t *pev) {
	if (pev->type != UART_EVT_TYPE_RX_DATA && pev->type != UART_EVT_TYPE_RX_DATA_TO)
		return;
	for (int i = 0; i < pev->len; i++) {
		uint8_t b = pev->data[i];
		if (b >= UART_FRAME_MARKER_MIN) {
			if (fcomp.in_frame)
				fcomp_process_frame();
			fcomp.cur_frame[0] = b;
			fcomp.cur_pos = 1;
			fcomp.in_frame = 1;
		} else if (fcomp.in_frame && fcomp.cur_pos < FCOMP_MAX_FRAME_LEN) {
			fcomp.cur_frame[fcomp.cur_pos++] = b;
		}
	}
}

static void fcomp_init(void) {
	memset((void*)&fcomp, 0, sizeof(fcomp));
}

static void fcomp_reset_diffs(void) {
	fcomp.total_frames = 0;
	fcomp.num_diffs = 0;
	fcomp.in_frame = 0;
	fcomp.cur_pos = 0;
}

static int fcomp_uart_init(void) {
	uart_Cfg_t ucfg;
	memset(&ucfg, 0, sizeof(ucfg));
	ucfg.tx_pin = GPIO_P10;
	ucfg.rx_pin = GPIO_P17;
	ucfg.baudrate = 9600;
	ucfg.use_fifo = TRUE;
	ucfg.hw_fwctrl = FALSE;
	ucfg.use_tx_buf = FALSE;
	ucfg.parity = FALSE;
	ucfg.evt_handler = fcomp_callback;
	return hal_uart_init(ucfg, UART1);
}

static void fcomp_listen(uint16_t ms) {
	uint32_t ticks = ((uint32_t)ms * 32768UL) / 1000UL;
	WaitRTCCount(ticks);
}

// Wait for next complete frame (up to timeout_ms). Returns 1 if frame received.
static uint8_t fcomp_wait_frame(uint16_t timeout_ms) {
	fcomp.frame_ready = 0;
	// Poll at ~32μs resolution
	uint32_t max_ticks = ((uint32_t)timeout_ms * 32768UL) / 1000UL;
	uint32_t ticks = 0;
	while (!fcomp.frame_ready && ticks < max_ticks) {
		WaitRTCCount(1); // ~32μs
		ticks++;
	}
	return fcomp.frame_ready;
}

// Timed response probe: wait for frame, send response, listen, check for change.
// Returns num_diffs seen in the listen_ms after sending.
static uint8_t fcomp_timed_probe(const uint8_t *tx_data, uint8_t tx_len, uint16_t listen_ms) {
	// Reset diff tracking but keep reference frame from initial listen
	fcomp_reset_diffs();

	// Wait for a complete heartbeat frame
	if (!fcomp_wait_frame(200))
		return 0;

	// Immediately send the response on TX (P10)
	if (tx_len > 0 && tx_data)
		hal_uart_send_buff(UART1, (uint8_t *)tx_data, tx_len);

	// Listen for subsequent frames and check if any differ from reference
	fcomp_listen(listen_ms);

	// Flush any remaining in-progress frame
	if (fcomp.in_frame && fcomp.cur_pos >= 3)
		fcomp_process_frame();

	return fcomp.num_diffs;
}

static int scan_make_response(uint8_t *obuf, uint8_t status, uint8_t op, uint8_t mode,
		uint8_t scan_id, uint8_t phase, uint8_t total_chunks, uint8_t chunk_idx,
		uint8_t record_type, uint8_t record_count, const uint8_t *payload, uint8_t payload_len) {
	obuf[0] = CMD_ID_I2C_SCAN;
	obuf[1] = status;
	obuf[2] = op;
	obuf[3] = mode;
	obuf[4] = scan_id;
	obuf[5] = phase;
	obuf[6] = total_chunks;
	obuf[7] = chunk_idx;
	obuf[8] = record_type;
	obuf[9] = record_count;
	if (payload_len && payload)
		memcpy(&obuf[10], payload, payload_len);
	return payload_len + 10;
}

#if 0 // V7: GPIO/I2C phases disabled
static uint8_t scan_pin_class(uint8_t f, uint8_t pu, uint8_t pd) {
	if (pu && !pd)
		return 1; // driven/pulled high
	if (!pu && pd)
		return 2; // driven/pulled low
	if (!f && !pu && !pd)
		return 0; // floating/unknown
	return 3; // unstable/mixed
}

static void scan_sample_pin(gpio_pin_e pin, uint8_t out[8]) {
	uint8_t f, pu, pd;
	hal_gpio_pin_init(pin, GPIO_INPUT);
	hal_gpio_pull_set(pin, GPIO_FLOATING);
	WaitRTCCount(64);
	f = hal_gpio_read(pin) ? 1 : 0;
	hal_gpio_pull_set(pin, GPIO_PULL_UP);
	WaitRTCCount(64);
	pu = hal_gpio_read(pin) ? 1 : 0;
	hal_gpio_pull_set(pin, GPIO_PULL_DOWN);
	WaitRTCCount(64);
	pd = hal_gpio_read(pin) ? 1 : 0;
	hal_gpio_pull_set(pin, GPIO_FLOATING);

	// Activity detection: sample rapidly for ~5ms and count transitions
	uint8_t transitions = 0;
	uint8_t prev = hal_gpio_read(pin) ? 1 : 0;
	for (int s = 0; s < 160; s++) { // ~160 * 32us = ~5ms
		WaitRTCCount(1);
		uint8_t cur = hal_gpio_read(pin) ? 1 : 0;
		if (cur != prev) {
			if (transitions < 255)
				transitions++;
			prev = cur;
		}
	}

	out[0] = (uint8_t)pin;
	out[1] = f;
	out[2] = pu;
	out[3] = pd;
	out[4] = 100 - ((f != pu) + (pu != pd) + (f != pd)) * 33;
	out[5] = scan_pin_class(f, pu, pd);
	out[6] = transitions;
	out[7] = 0; // reserved
}
#endif // V7: GPIO scan disabled

static void scan_add_chunk(uint8_t rec_type, uint8_t rec_count, const uint8_t *payload, uint8_t payload_len) {
	if (g_scan_state.total_chunks >= SCAN_MAX_CHUNKS || payload_len > SCAN_MAX_PAYLOAD)
		return;
	scan_chunk_t *c = &g_scan_state.chunks[g_scan_state.total_chunks++];
	c->record_type = rec_type;
	c->record_count = rec_count;
	c->payload_len = payload_len;
	if (payload_len)
		memcpy(c->payload, payload, payload_len);
}

#if 0 // V7: I2C probe disabled
// Active I2C probe: try to detect devices on a pin pair
static uint8_t scan_i2c_probe_pair(gpio_pin_e sda, gpio_pin_e scl, uint8_t *found_addrs, uint8_t max_found) {
	dev_i2c_t i2c_dev;
	uint8_t count = 0;
	uint8_t dummy;

	i2c_dev.scl = scl;
	i2c_dev.sda = sda;
	i2c_dev.speed = I2C_100KHZ;
	i2c_dev.i2c_num = 0;

	init_i2c(&i2c_dev);

	for (uint8_t addr = 0x08; addr < 0x78; addr++) {
		if (read_i2c_nabuf(&i2c_dev, addr, &dummy, 1) == 0) {
			if (count < max_found)
				found_addrs[count] = addr;
			count++;
		}
	}

	deinit_i2c(&i2c_dev);
	return count;
}
#endif // V7: GPIO/I2C phases disabled

static void scan_build_report(uint8_t mode, uint8_t scan_id) {
	uint8_t sensor_type = 0;
	uint8_t sensor_present = 0;

#if (DEV_SERVICES & SERVICE_THS)
	sensor_type = thsensor_cfg.sensor_type;
	sensor_present = (thsensor_cfg.sensor_type != TH_SENSOR_NONE) ? 1 : 0;
#endif

	memset(&g_scan_state, 0, sizeof(g_scan_state));
	g_scan_state.active = 1;
	g_scan_state.mode = mode;
	g_scan_state.scan_id = scan_id;
	g_scan_state.phase = SCAN_PHASE_UART; // V6: skip GPIO/I2C, go straight to UART

	// V7: Extended passive listen + Frame-synchronized response probing
	if (mode >= 1) {
		g_scan_state.phase = SCAN_PHASE_UART;

		fcomp_init();

		if (fcomp_uart_init() != 0) {
			uint8_t hdr[4] = { (uint8_t)GPIO_P17, 0, 0, 0xFF };
			scan_add_chunk(SCAN_REC_UART_STAT, 1, hdr, 4);
		} else {
			// --- Phase A: 15-second passive listen ---
			fcomp_listen(15000);

			// Flush any in-progress frame
			if (fcomp.in_frame && fcomp.cur_pos >= 3)
				fcomp_process_frame();

			// Emit reference frame: record type 10
			{
				uint8_t rec[SCAN_MAX_PAYLOAD];
				uint16_t tf = fcomp.total_frames;
				rec[0] = fcomp.ref_len;
				rec[1] = tf & 0xFF;
				rec[2] = (tf >> 8) & 0xFF;
				rec[3] = fcomp.num_diffs;
				uint8_t rl = fcomp.ref_len;
				if (rl > SCAN_MAX_PAYLOAD - 4) rl = SCAN_MAX_PAYLOAD - 4;
				memcpy(&rec[4], (void*)fcomp.ref_frame, rl);
				scan_add_chunk(10, 1, rec, 4 + rl);
			}

			// Emit diff frames: record type 11
			for (uint8_t di = 0; di < fcomp.num_diffs; di++) {
				uint8_t rec[SCAN_MAX_PAYLOAD];
				uint16_t at = fcomp.diff_at[di];
				rec[0] = di;
				rec[1] = at & 0xFF;
				rec[2] = (at >> 8) & 0xFF;
				rec[3] = fcomp.diff_lens[di];
				uint8_t dl = fcomp.diff_lens[di];
				if (dl > SCAN_MAX_PAYLOAD - 4) dl = SCAN_MAX_PAYLOAD - 4;
				memcpy(&rec[4], (void*)fcomp.diff_frames[di], dl);
				scan_add_chunk(11, 1, rec, 4 + dl);
			}

			// --- Phase B: Frame-synchronized response probing ---
			// Send responses IMMEDIATELY after receiving a complete heartbeat frame.
			// The original firmware likely replies within a tight timing window.
			// Try 12 different response patterns.
			static const uint8_t resp01[] = { 0x01 };
			static const uint8_t resp02[] = { 0x06 };
			static const uint8_t resp03[] = { 0xA5, 0x00 };
			static const uint8_t resp04[] = { 0xA5, 0x01 };
			static const uint8_t resp05[] = { 0xA4, 0x00 };
			static const uint8_t resp06[] = { 0xA5, 0x03 };
			static const uint8_t resp07[] = { 0xA5, 0x04 };
			static const uint8_t resp08[] = { 0xA5, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
			static const uint8_t resp09[] = { 0xA4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
			// resp10 (idx=9): echo the reference frame back (built dynamically)
			// resp11 (idx=10): reference frame with byte[1]=0x01
			// resp12 (idx=11): 0xA5 0x05 0x01 (possible "request data" command)
			static const uint8_t resp12[] = { 0xA5, 0x05, 0x01 };

			static const struct {
				const uint8_t *data;
				uint8_t len;
			} timed_resps[] = {
				{ resp01, 1 },    // 0: ACK byte 0x01
				{ resp02, 1 },    // 1: ACK byte 0x06
				{ resp03, 2 },    // 2: A5 00 (marker + null)
				{ resp04, 2 },    // 3: A5 01 (marker + cmd1)
				{ resp05, 2 },    // 4: A4 00 (alt marker + null)
				{ resp06, 2 },    // 5: A5 03 (marker + FC3-like)
				{ resp07, 2 },    // 6: A5 04 (marker + FC4-like)
				{ resp08, 8 },    // 7: A5 01 + 6 zeros (longer cmd)
				{ resp09, 8 },    // 8: A4 01 + 6 zeros (longer cmd)
				{ NULL, 0 },      // 9: echo reference (dynamic)
				{ NULL, 0 },      // 10: modified reference (dynamic)
				{ resp12, 3 },    // 11: A5 05 01 (possible read cmd)
			};

			uint8_t pi;
			for (pi = 0; pi < 12; pi++) {
				const uint8_t *tx_data;
				uint8_t tx_len;
				uint8_t dyn_buf[FCOMP_MAX_FRAME_LEN];

				if (pi == 9) {
					// Echo: send reference frame back
					tx_data = (uint8_t*)(void*)fcomp.ref_frame;
					tx_len = fcomp.ref_len;
				} else if (pi == 10) {
					// Modified ref: set byte[1] to 0x01
					memcpy(dyn_buf, (void*)fcomp.ref_frame, fcomp.ref_len);
					if (fcomp.ref_len > 1) dyn_buf[1] = 0x01;
					tx_data = dyn_buf;
					tx_len = fcomp.ref_len;
				} else {
					tx_data = timed_resps[pi].data;
					tx_len = timed_resps[pi].len;
				}

				uint8_t ndiffs = fcomp_timed_probe(tx_data, tx_len, 300);

				// Emit timed probe result: record type 16
				// [probe_idx, tx_len, frames_seen_lo, frames_seen_hi, num_diffs, tx_data...]
				{
					uint8_t rec[SCAN_MAX_PAYLOAD];
					uint16_t tf = fcomp.total_frames;
					rec[0] = pi;
					rec[1] = tx_len;
					rec[2] = tf & 0xFF;
					rec[3] = (tf >> 8) & 0xFF;
					rec[4] = ndiffs;
					uint8_t tl = tx_len;
					if (tl > SCAN_MAX_PAYLOAD - 5) tl = SCAN_MAX_PAYLOAD - 5;
					if (tl > 0 && tx_data)
						memcpy(&rec[5], tx_data, tl);
					scan_add_chunk(16, 1, rec, 5 + tl);
				}

				// Emit first diff if any: record type 17
				if (ndiffs > 0 && fcomp.diff_lens[0] > 0) {
					uint8_t rec[SCAN_MAX_PAYLOAD];
					rec[0] = pi;
					rec[1] = fcomp.diff_lens[0];
					uint8_t dl = fcomp.diff_lens[0];
					if (dl > SCAN_MAX_PAYLOAD - 2) dl = SCAN_MAX_PAYLOAD - 2;
					memcpy(&rec[2], (void*)fcomp.diff_frames[0], dl);
					scan_add_chunk(17, 1, rec, 2 + dl);
				}
			}

			hal_uart_deinit(UART1);
			hal_gpio_pin_init(GPIO_P17, GPIO_INPUT);
			hal_gpio_pull_set(GPIO_P17, GPIO_FLOATING);
			hal_gpio_pin_init(GPIO_P10, GPIO_INPUT);
			hal_gpio_pull_set(GPIO_P10, GPIO_FLOATING);
		}
	}

	// Summary
	{
		uint8_t summary[8];
		summary[0] = SCAN_PHASE_UART;
		summary[1] = (uint8_t)GPIO_P17; // RX pin
		summary[2] = (uint8_t)GPIO_P10; // TX pin
		summary[3] = 0;
		summary[4] = sensor_type;
		summary[5] = mode;
		summary[6] = 0;
		summary[7] = sensor_present;
		scan_add_chunk(SCAN_REC_SUMMARY, 1, summary, 8);
		g_scan_state.summary_chunk = g_scan_state.total_chunks - 1;
	}

	g_scan_state.phase = SCAN_PHASE_DONE;
}

#endif // DEVICE == DEVICE_IBSTH2P

const dev_id_t dev_id = {
		.pid = CMD_ID_DEVID,
		.revision = 1,
		.hw_version = DEVICE,
		.sw_version = APP_VERSION,
		.dev_spec_data = 0,
		.services = DEV_SERVICES
};

#if EFUSE_TEST
//extern void efuse_init(void);
//extern int efuse_read(int block,uint32_t* buf);
//extern int efuse_write_x(int block,uint32_t* buf,uint32_t us);

int efuse_read_p(int block, uint32_t* buf)
{
	uint32_t *reg = (uint32_t *)0x4000f160; // AP_PCRM->EFUSE0;
    block &= 3;
    AP_PCRM->SECURTY_STATE = 0x0f;
    AP_PCRM->efuse_cfg = BIT(16 + block); //enable o_sclk_prog_hcyc,sclk high duty during time, unit:1/32M clk.prog en
    WaitRTCCount(2); // 2*32us
    reg += block << 1;
    buf[0] = *((uint32_t *)reg);
    reg++;
    buf[1] = *((uint32_t *)reg);
    AP_PCRM->efuse_cfg = 0x00;//disable o_sclk_prog_hcyc and clear prog data
    return AP_PCRM->SECURTY_STATE;
}

int efuse_write_p(int block, uint32_t* buf)
{
    uint32_t temp_rd[2];
    block &= 3;
    AP_PCRM->SECURTY_STATE = 0x0f;
    AP_PCRM->EFUSE_PROG[0] = buf[0];
    AP_PCRM->EFUSE_PROG[1] = buf[1];
    uint32_t temp = (BIT((28 + block)) | 0x8000);//enable o_sclk_prog_hcyc, sclk high duty during time, unit:1/32M clk.prog en
    AP_PCRM->efuse_cfg = temp;
    WaitRTCCount(512);//512*32us
    AP_PCRM->efuse_cfg = 0x00;//disable o_sclk_prog_hcyc and clear prog data
    AP_PCRM->EFUSE_PROG[0] = 0;
    AP_PCRM->EFUSE_PROG[1] = 0;
    efuse_read_p(block, temp_rd);
    return (temp_rd[1] != buf[1]) || (temp_rd[0] != buf[0]);
}
#endif

#if (OTA_TYPE == OTA_TYPE_BOOT)

#define FLASH_ADDR_RINFO  FLASH_BASE_ADDR
#define OFFSET_ADDR_RMAC  0x900
#define FLASH_ADDR_RMAC  (FLASH_BASE_ADDR + OFFSET_ADDR_RMAC)

static void write_fix_mac(uint32_t *pdata) {
	uint32_t idx, tmp;
	uint8_t buf_sector[FLASH_SECTOR_SIZE];
	hal_flash_read(FLASH_ADDR_RINFO, buf_sector, FLASH_SECTOR_SIZE);
	memcpy(&buf_sector[OFFSET_ADDR_RMAC], pdata, CHIP_MADDR_LEN*4);
	tmp = phy_flash.Capacity;
	phy_flash.Capacity |= FLASH_MAX_SIZE;
	*(volatile int*) 0x1fff0898 = phy_flash.Capacity;
	hal_flash_unlock();
	hal_flash_erase_sector(FLASH_ADDR_RINFO + phy_flash.Capacity);
	for(idx = 0; idx < FLASH_SECTOR_SIZE; idx += 256)
		hal_flash_write(FLASH_ADDR_RINFO + phy_flash.Capacity + idx, &buf_sector[idx], 256);
	phy_flash.Capacity = tmp;
	*(volatile int*) 0x1fff0898 = phy_flash.Capacity;
}

void fix_mac(int write) {
	int i;
	uint32_t data[CHIP_MADDR_LEN];
	if(write) {
		if(read_chip_mAddr((uint8_t *)data) == CHIP_ID_VALID) {
			if(memcmp(ownPublicAddr, data, CHIP_MADDR_LEN) == 0)
				return;
		}
		for (i = 0; i < CHIP_MADDR_LEN; i++) {
			data[CHIP_MADDR_LEN - 1 - i] = (1 << (((ownPublicAddr[i] & 0xf0) >> 4) + 16))
					| (1 << (ownPublicAddr[i] & 0x0f));
		}
		write_fix_mac(data);

	} else { // clear
		hal_flash_read(FLASH_ADDR_RMAC, (uint8_t *)data, sizeof(data));
		for(i = 0; i < CHIP_MADDR_LEN; i++) {
			if(data[i] != 0xffffffff) {
				memset(&data, 0xff, sizeof(data));
				write_fix_mac(data);
				break;
			}
		}
	}
}

#endif

#if (DEV_SERVICES & SERVICE_SCREEN) && (OTA_TYPE == OTA_TYPE_APP)
static void update_display_mode(uint32_t changed_flags) {
	if (changed_flags & FLG_DISPLAY_OFF) {
		init_lcd(); // Enable/disable display according to config
	} else if (changed_flags & FLG_DISPLAY_SLEEP) {
		if ((cfg.flg & (FLG_DISPLAY_OFF | FLG_DISPLAY_SLEEP)) == 0) {
			init_lcd(); // Switch from "sleep mode" to "always on"
		}
	} else {
		return; // nothing relevant changed
	}

	wrk.lcd_sleeping = 0;
#if (DEV_SERVICES & SERVICE_KEY)
	wrk.long_press_state = LONG_PRESS_NONE;
#endif
	start_display_sleep_timer(); // Does nothing unless display and sleep mode are on
}
#endif

int cmd_parser(uint8_t * obuf, uint8_t * ibuf, uint32_t len) {
	int olen = 0;
	if (len) {
		uint8_t cmd = ibuf[0];
		uint32_t tmp = ibuf[1] | (ibuf[2]<<8) | (ibuf[3]<<16) | (ibuf[4]<<24);
		obuf[0] = cmd;
		obuf[1] = 0; // no err
		if (cmd == CMD_ID_DEVID) { // Get DEV_ID
			memcpy(obuf, &dev_id, sizeof(dev_id));
#if (DEV_SERVICES & SERVICE_THS)
			dev_id_t * p = (dev_id_t *)&obuf;
			p->dev_spec_data = thsensor_cfg.sensor_type;
#endif
			olen = sizeof(dev_id);
		} else if (cmd == CMD_ID_CFG) {		// Get/Set device config
			if (--len > sizeof(cfg))
				len = sizeof(cfg);
			if (len) {
#if (DEV_SERVICES & SERVICE_SCREEN) && (OTA_TYPE == OTA_TYPE_APP)
				tmp ^= cfg.flg;
#endif
				memcpy(&cfg, &ibuf[1], len);
#if (DEV_SERVICES & SERVICE_SCREEN) && (OTA_TYPE == OTA_TYPE_APP)
				update_display_mode(tmp);
#endif
				test_config();
				save_config();
			}
			memcpy(&obuf[1], &cfg, sizeof(cfg));
			olen = sizeof(cfg) + 1;
		} else if (cmd == CMD_ID_CFG_DEF) { // Set default device config
#if (DEV_SERVICES & SERVICE_SCREEN) && (OTA_TYPE == OTA_TYPE_APP)
			tmp = cfg.flg ^ def_cfg.flg;
#endif
			memcpy(&cfg, &def_cfg, sizeof(cfg));
#if (DEV_SERVICES & SERVICE_SCREEN) && (OTA_TYPE == OTA_TYPE_APP)
			update_display_mode(tmp);
#endif
			test_config();
			save_config();
			memcpy(&obuf[1], &cfg, sizeof(cfg));
			olen = sizeof(cfg) + 1;
#if (DEV_SERVICES & SERVICE_THS)
		} else if (cmd == CMD_ID_CFS) {	// Get/Set sensor config
			if (--len > sizeof(thsensor_cfg.coef))
				len = sizeof(thsensor_cfg.coef);
			if (len) {
				memcpy(&thsensor_cfg.coef, &ibuf[1], len);
				flash_write_cfg(&thsensor_cfg.coef, EEP_ID_CFS, sizeof(thsensor_cfg.coef));
			}
			memcpy(&obuf[1], &thsensor_cfg, thsensor_cfg_send_size);
			olen = thsensor_cfg_send_size + 1;
		} else if (cmd == CMD_ID_CFS_DEF) {	// Get/Set default sensor config
			memset(&thsensor_cfg, 0, thsensor_cfg_send_size);
			init_sensor();
			memcpy(&obuf[1], &thsensor_cfg, thsensor_cfg_send_size);
			olen = thsensor_cfg_send_size + 1;
		} else if (cmd == CMD_ID_SEN_ID) {
			memcpy(&obuf[1], (uint8_t *)&thsensor_cfg.mid, 6);
			olen = 1 + 5;
#if (OTA_TYPE == OTA_TYPE_APP) && (DEV_SERVICES & SERVICE_TH_TRG)
		} else if (cmd == CMD_ID_TRG) {	// Get/Set trigger data config
			if (--len > trigger_send_size)
				len = trigger_send_size;
			if (len) {
				memcpy(&trg, &ibuf[1], len);
				flash_write_cfg(&trg, EEP_ID_TRG, trigger_send_size);
			}
			memcpy(&obuf[1], &trg, trigger_send_size);
			olen = trigger_send_size + 1;
#endif

#endif
#if (DEV_SERVICES & SERVICE_HISTORY)
		} else if (cmd == CMD_ID_LOGGER && len > 2) { // Read memory measures
			rd_memo.cnt = ibuf[1] | (ibuf[2] << 8);
			if (rd_memo.cnt) {
				rd_memo.saved = memo;
				if (len > 4)
					rd_memo.cur = ibuf[3] | (ibuf[4] << 8);
				else
					rd_memo.cur = 0;
			}
			wrk_notify();
//			osal_set_event(simpleBLEPeripheral_TaskID, WRK_NOTIFY_EVT);
		} else if (cmd == CMD_ID_CLRLOG && len > 2) { // Clear memory measures
			if (ibuf[1] == 0x12 && ibuf[2] == 0x34) {
				clear_memo();
				olen = 2;
			}
#endif
#if (DEV_SERVICES & SERVICE_BINDKEY)
		} else if (cmd == CMD_ID_BKEY) { // Get/set beacon bindkey
			if (len == sizeof(bindkey) + 1) {
				if(memcmp(bindkey, &ibuf[1], sizeof(bindkey))) {
					memcpy(bindkey, &ibuf[1], sizeof(bindkey));
					flash_write_cfg(&bindkey, EEP_ID_KEY, sizeof(bindkey));
					bthome_beacon_init();
				}
			}
			if (flash_read_cfg(&bindkey, EEP_ID_KEY, sizeof(bindkey)) == sizeof(bindkey)) {
				memcpy(&obuf[1], bindkey, sizeof(bindkey));
				olen = sizeof(bindkey) + 1;
			} else { // No bindkey in EEP!
				obuf[1] = 0xff;
				olen = 2;
			}
#endif
#if (DEV_SERVICES & SERVICE_FINDMY)
		} else if (cmd == CMD_ID_FDMKEY) { // Get/set findmy key
			if (len == sizeof(findmy_key)/2 + 2) {
				if(ibuf[1] == 3) {
					memcpy(findmy_key_new, &ibuf[2], SIZE_FINDMY_KEY/2);
					memcpy(&obuf[2], findmy_key_new, SIZE_FINDMY_KEY/2);
					obuf[1] = 3;
					olen = SIZE_FINDMY_KEY/2 + 2;
				} else if(ibuf[1] == 4) {
					memcpy(&findmy_key_new[SIZE_FINDMY_KEY/2], &ibuf[2], SIZE_FINDMY_KEY/2);
					memcpy(&obuf[2], &findmy_key_new[SIZE_FINDMY_KEY/2], SIZE_FINDMY_KEY/2);
					if(memcmp(findmy_key, findmy_key_new, SIZE_FINDMY_KEY)) {
						memcpy(findmy_key, findmy_key_new, SIZE_FINDMY_KEY);
						flash_write_cfg(findmy_key, EEP_ID_FDK, sizeof(findmy_key));
						findmy_key_new[0] |= 0xC0;
						swap_mac(findmy_key_new, findmy_key_new);
						if(memcmp(ownPublicAddr, findmy_key_new, MAC_LEN)) {
							memcpy(ownPublicAddr, findmy_key_new, MAC_LEN);
							flash_write_cfg(ownPublicAddr, EEP_ID_MAC, MAC_LEN);
							wrk.reboot |= 1;
						}
					}
					obuf[1] = 4;
					olen = SIZE_FINDMY_KEY/2 + 2;
				} else {
					obuf[1] = 0xff;
					olen = 2;
				}
			} else if (len == 2) {
				if(ibuf[1] == 1) {
					if (flash_read_cfg(findmy_key, EEP_ID_FDK, sizeof(findmy_key)) == sizeof(findmy_key)) {
						memcpy(&obuf[2], findmy_key, SIZE_FINDMY_KEY/2);
						obuf[1] = 1;
						olen = SIZE_FINDMY_KEY/2 + 2;
					} else { // No findmy key in EEP!
						obuf[1] = 0xfe;
						olen = 2;
					}
				} else if(ibuf[1] == 2) {
					if (flash_read_cfg(findmy_key, EEP_ID_FDK, sizeof(findmy_key)) == sizeof(findmy_key)) {
						memcpy(&obuf[2], &findmy_key[SIZE_FINDMY_KEY/2], SIZE_FINDMY_KEY/2);
						obuf[1] = 2;
						olen = SIZE_FINDMY_KEY/2 + 2;
					} else { // No findmy key in EEP!
						obuf[1] = 0xfe;
						olen = 2;
					}
				} else {
					obuf[1] = 0xff;
					olen = 2;
				}
			} else {
				obuf[1] = 0xff;
				olen = 2;
			}
#endif
#if defined(GPIO_BUZZER) && defined(PWM_CHL_BUZZER)
		} else if (cmd == CMD_ID_BUZZER) {
			if(len == 2 && ibuf[1] == 0)
				pwm_buzzer_stop();
			else
				pwm_buzzer_start();
			olen = 2;
#endif
		} else if (cmd == CMD_ID_SERIAL) {
			memcpy(&obuf[1], devInfoSerialNumber, sizeof(devInfoSerialNumber)-1);
			olen = 1 + sizeof(devInfoSerialNumber)-1;
		} else if (cmd == CMD_ID_FLASH_ID) {
			memcpy(&obuf[1], (uint8_t *)&phy_flash.IdentificationID, 8);
			olen = 1 + 8;
		} else if (cmd == CMD_ID_MTU) {
			if(len >= 2) {
				if (ibuf[1] <= MTU_SIZE) {
					ATT_UpdateMtuSize(gapRole_ConnectionHandle, ibuf[1]);
					obuf[1] = gAttMtuSize[gapRole_ConnectionHandle];
				} else
					obuf[1] = 0xff;
			} else
				obuf[1] = gAttMtuSize[gapRole_ConnectionHandle];
			olen = 2;
		} else if (cmd == CMD_ID_REBOOT) {
			if(len >= 2) {
				wrk.reboot = ibuf[1];
				obuf[1] = ibuf[1];
			} else
				obuf[1] = wrk.reboot;
			olen = 2;
		} else if (cmd == CMD_ID_MEASURE) {
			olen = make_measure_msg(obuf);
#if (DEV_SERVICES & SERVICE_SCREEN)
		} else if (cmd == CMD_ID_LCD_DUMP) { // Get/set lcd buf
			if (--len > sizeof(lcdd.display_buff))
				len = sizeof(lcdd.display_buff);
			if (len) {
				lcdd.chow_ext_ut = clkt.utc_time_sec + 600;
				memcpy(lcdd.display_buff, &ibuf[1], len);
				update_lcd();
			} else {
				lcdd.chow_ext_ut = 0;
				chow_lcd(1);
			}
			memcpy(&obuf[1], lcdd.display_buff, sizeof(lcdd.display_buff));
			olen = 1 + sizeof(lcdd.display_buff);
#endif
#if (DEV_SERVICES & SERVICE_SCREEN) && (OTA_TYPE == OTA_TYPE_APP)
		} else if (cmd == CMD_ID_EXTDATA) { // Show ext. small and big number
			if (--len > sizeof(lcdd.ext))
				len = sizeof(lcdd.ext);
			if (len) {
				memcpy(&lcdd.ext, &ibuf[1], len);
				if(lcdd.ext.vtime_sec == 0xffff)
					lcdd.chow_ext_ut = 0xffffffff;
				else
					lcdd.chow_ext_ut = clkt.utc_time_sec + lcdd.ext.vtime_sec;
				chow_ext_data();
			} else {
				lcdd.chow_ext_ut = 0;
				chow_lcd(1);
			}
			memcpy(&obuf[1], &lcdd.ext, sizeof(lcdd.ext));
			olen = 1 + sizeof(lcdd.ext);
#endif
		} else if (cmd == CMD_ID_UTC_TIME) { // Get/set utc time
			if (len > 4) {
				clkt.utc_time_sec = tmp;
#if (DEV_SERVICES & SERVICE_TIME_ADJUST)
				@TODO
#endif
				clkt.utc_time_tik = clock_time_rtc();
				//clkt.utc_time_add = 0;
			}
			tmp = get_utc_time_sec();
			memcpy(&obuf[1], &tmp, 4);
#if (DEV_SERVICES & SERVICE_TIME_ADJUST)
			memcpy(&obuf[4 + 1], &clkt.utc_set_time_sec, sizeof(clkt.utc_set_time_sec));
			olen = 4 + sizeof(clkt.utc_set_time_sec) + 1;
#else
			olen = 4 + 1;
#endif // SERVICE_TIME_ADJUST
#if (DEV_SERVICES & SERVICE_TIME_ADJUST)
		} else if (cmd == CMD_ID_TADJUST) { // Get/set adjust time clock delta (in 1/16 us for 1 sec)
			if (len > 4) {
				clkt.delta_time = tmp;
				flash_write_cfg(&clkt.delta_time, EEP_ID_TIM, sizeof(&clkt.delta_time));
			}
			memcpy(&send_buf[1], &clkt.delta_time, sizeof(clkt.delta_time));
			olen = sizeof(clkt.delta_time) + 1;
#endif
		} else if (cmd == CMD_ID_DEV_MAC) {
			if (len > MAC_LEN) {
				if(memcmp(ownPublicAddr, &ibuf[1], MAC_LEN)) {
					memcpy(ownPublicAddr, &ibuf[1], MAC_LEN);
					flash_write_cfg(ownPublicAddr, EEP_ID_MAC, MAC_LEN);
					wrk.reboot |= 1;
				}
			}
			memcpy(&obuf[1], ownPublicAddr, MAC_LEN);
			olen = MAC_LEN + 1;
#if (OTA_TYPE == OTA_TYPE_BOOT)
		} else if (cmd == CMD_ID_FIX_MAC && len > 1) {
			fix_mac(ibuf[1]);
			olen = 2;
#endif
		} else if (cmd == CMD_ID_DNAME) {
			if (len > 1 && len <= GAP_DEVICE_NAME_LEN) {
				if(ibuf[1] == 0)
					set_def_name();
				else {
					len--;
					memcpy(&gapRole_ScanRspData[2], &ibuf[1], len);
					flash_write_cfg(&gapRole_ScanRspData[2], EEP_ID_DVN, len);
				}
				set_dev_name();
			}
			olen = gapRole_ScanRspData[0];
			memcpy(&obuf[1], &gapRole_ScanRspData[2], olen - 1);
		} else if (cmd == CMD_ID_I2C_SCAN) {
#if DEVICE == DEVICE_IBSTH2P
			// V10: UART frame parser stats and debug
			uint8_t op = (len > 1) ? ibuf[1] : 0;
			obuf[0] = CMD_ID_I2C_SCAN;
			if (op == 0) {
				// Stats dump
				obuf[1] = 0x0C; // version: V12
				obuf[2] = ucap.uart_inited;
				obuf[3] = ucap.sensor_valid;
				obuf[4] = (ucap.total_bytes) & 0xFF;
				obuf[5] = (ucap.total_bytes >> 8) & 0xFF;
				obuf[6] = (ucap.total_bytes >> 16) & 0xFF;
				obuf[7] = (ucap.total_bytes >> 24) & 0xFF;
				obuf[8] = ucap.good_frames & 0xFF;
				obuf[9] = (ucap.good_frames >> 8) & 0xFF;
				obuf[10] = ucap.fr.crc_bad & 0xFF;
				obuf[11] = (ucap.fr.crc_bad >> 8) & 0xFF;
				obuf[12] = ucap.last_temp & 0xFF;
				obuf[13] = (ucap.last_temp >> 8) & 0xFF;
				obuf[14] = ucap.last_humi & 0xFF;
				obuf[15] = (ucap.last_humi >> 8) & 0xFF;
				olen = 16;
			} else if (op == 1) {
				// Latest parsed frame debug info
				obuf[1] = 0x0C; // V12
				obuf[2] = ucap.last_temp & 0xFF;
				obuf[3] = (ucap.last_temp >> 8) & 0xFF;
				obuf[4] = ucap.last_humi & 0xFF;
				obuf[5] = (ucap.last_humi >> 8) & 0xFF;
				obuf[6] = ucap.last_byte1;
				obuf[7] = ucap.last_byte2;
				obuf[8] = ucap.last_byte9;
				obuf[9] = ucap.last_flags7;
				obuf[10] = ucap.last_flags8;
				olen = 11;
			} else if (op == 3 && len > 2) {
				// Read raw captured bytes at offset ibuf[2]
				uint8_t offset = ibuf[2];
				obuf[1] = 0x0C;
				obuf[2] = rawcap_pos;
				obuf[3] = offset;
				if (offset < rawcap_pos) {
					uint8_t copylen = rawcap_pos - offset;
					if (copylen > 12) copylen = 12;
					memcpy(&obuf[4], &rawcap_buf[offset], copylen);
					olen = 4 + copylen;
				} else {
					olen = 4;
				}
#ifdef UCAP_PROBE
			} else if (op == 4) {
				// Frame-period probe log dump. ibuf[2..3] = start index
				// (monotonic frame number); returns up to 2 records of
				// 6 bytes each: tik u32 LE (24-bit RTC), type7, flags8.
				// Header: [1]=0x50, [2..3]=probe_cnt, [4..5]=start idx.
				// Indices older than the ring (probe_cnt - 64) or >= cnt
				// return no records — the client walks from
				// max(0, cnt-64) to cnt-1.
				uint16_t idx = (len > 3) ? (uint16_t)(ibuf[2] | (ibuf[3] << 8)) : 0;
				uint16_t cnt = probe_cnt;
				uint16_t first = (cnt > PROBE_LOG_SIZE) ? (uint16_t)(cnt - PROBE_LOG_SIZE) : 0;
				obuf[1] = 0x50;
				obuf[2] = cnt & 0xFF;
				obuf[3] = (cnt >> 8) & 0xFF;
				obuf[4] = idx & 0xFF;
				obuf[5] = (idx >> 8) & 0xFF;
				olen = 6;
				for (int k = 0; k < 2; k++, idx++) {
					if (idx < first || idx >= cnt)
						break;
					memcpy(&obuf[olen], &probe_log[idx % PROBE_LOG_SIZE], sizeof(probe_rec_t));
					olen += sizeof(probe_rec_t);
				}
#endif
			} else {
				obuf[1] = 0xFF;
				olen = 2;
			}
#else
			// V7 scan handler (non-IBSTH2P devices)
			{
				uint8_t op = (len > 1) ? ibuf[1] : SCAN_OP_START;
				uint8_t mode = (len > 2) ? ibuf[2] : 0;
				uint8_t scan_id = (len > 3) ? ibuf[3] : 0;
				uint8_t chunk_idx = (len > 4) ? ibuf[4] : 0;

				if (op == SCAN_OP_START) {
					scan_build_report(mode, scan_id);
					olen = scan_make_response(obuf, SCAN_STATUS_OK, op, g_scan_state.mode,
							g_scan_state.scan_id, g_scan_state.phase, g_scan_state.total_chunks,
							0, 0, 0, NULL, 0);
				} else if (op == SCAN_OP_GET_CHUNK) {
					if (!g_scan_state.active || chunk_idx >= g_scan_state.total_chunks) {
						olen = scan_make_response(obuf, SCAN_STATUS_BAD_ARG, op, g_scan_state.mode,
								g_scan_state.scan_id, g_scan_state.phase, g_scan_state.total_chunks,
								chunk_idx, 0, 0, NULL, 0);
					} else {
						scan_chunk_t *c = &g_scan_state.chunks[chunk_idx];
						olen = scan_make_response(obuf, SCAN_STATUS_OK, op, g_scan_state.mode,
								g_scan_state.scan_id, g_scan_state.phase, g_scan_state.total_chunks,
								chunk_idx, c->record_type, c->record_count,
								c->payload, c->payload_len);
					}
				} else if (op == SCAN_OP_GET_SUMMARY) {
					if (!g_scan_state.active || g_scan_state.total_chunks == 0) {
						olen = scan_make_response(obuf, SCAN_STATUS_BAD_ARG, op, g_scan_state.mode,
								g_scan_state.scan_id, g_scan_state.phase, g_scan_state.total_chunks,
								0, 0, 0, NULL, 0);
					} else {
						scan_chunk_t *c = &g_scan_state.chunks[g_scan_state.summary_chunk];
						olen = scan_make_response(obuf, SCAN_STATUS_OK, op, g_scan_state.mode,
								g_scan_state.scan_id, g_scan_state.phase, g_scan_state.total_chunks,
								g_scan_state.summary_chunk, c->record_type, c->record_count,
								c->payload, c->payload_len);
					}
				} else if (op == SCAN_OP_ABORT) {
					memset(&g_scan_state, 0, sizeof(g_scan_state));
					olen = scan_make_response(obuf, SCAN_STATUS_ABORTED, op, mode,
							scan_id, SCAN_PHASE_IDLE, 0, 0, 0, 0, NULL, 0);
				} else {
					olen = scan_make_response(obuf, SCAN_STATUS_BAD_ARG, op, mode,
							scan_id, SCAN_PHASE_IDLE, 0, 0, 0, 0, NULL, 0);
				}
			}
#endif // DEVICE == DEVICE_IBSTH2P

//---------- Debug commands (unsupported in different versions!):

		} else if (cmd == CMD_ID_EEP_RW && len > 2) {
			obuf[1] = ibuf[1];
			obuf[2] = ibuf[2];
			uint16_t id = (uint16_t)tmp;
			if(len > 3) {
				flash_write_cfg(&ibuf[3], id, len - 3);
			}
			int16_t i = flash_read_cfg(&obuf[3], id, SEND_DATA_SIZE);
			if(i < 0) {
				obuf[1] = (uint8_t)(i & 0xff); // Error
				olen = 2;
			} else
				olen = i + 3;
		} else if (cmd == CMD_ID_MEM_RW && len > 4) { // Read/Write memory
			uint8_t *p = (uint8_t *)tmp;
			if(len > 5) {
				len -= 5;
				memcpy(p, &ibuf[5], len);
			} else
				len = SEND_DATA_SIZE;
			memcpy(obuf, ibuf, 5);
			memcpy(&obuf[5], p, len);
			olen = len + 1 + 4;
		} else if (cmd == CMD_ID_REG_RW && len > 4) { // Read/Write 32 bits register (aligned)
			volatile uint32_t *p = (volatile uint32_t *)tmp;
			if(len > 8) {
				tmp = ibuf[5] | (ibuf[6]<<8) | (ibuf[7]<<16) | (ibuf[8]<<24);
				*p = tmp;
			} else {
				obuf[1] = 0xfe; // Error size
				olen = 2;
			}
			tmp = *p;
			memcpy(obuf, ibuf, 5);
			memcpy(&obuf[5], &tmp, 4);
			olen = 1 + 4 + 4;
#if (DEV_SERVICES & SERVICE_SCREEN)
		} else if (cmd == CMD_ID_DEBUG) { // debug - send to lcd
			if (--len) {
				send_to_lcd(&ibuf[1], len);
			}
#endif
#if EFUSE_TEST
		} else if (cmd == CMD_ID_EFUSE && len > 3 && ibuf[1] == 0xce) { // Read/Write 32 bits register (aligned)
			extern int phy_sec_efuse_lock(int block);
			uint32_t efuse[2];
			//efuse_init();
			if(ibuf[2] == 0) {
				obuf[1] = efuse_read_p(ibuf[3] & 3, efuse);
				memcpy(&obuf[2], efuse, sizeof(efuse));
				olen = 2 + sizeof(efuse);
			} else if(ibuf[2] == 1) {
				memcpy(efuse, &ibuf[4], sizeof(efuse));
				obuf[1] = efuse_write_p(ibuf[3] & 3, efuse);
				olen = 2;
			} else if(ibuf[2] == 2) {
				obuf[1] = phy_sec_efuse_lock(ibuf[3] & 3);
				olen = 2;
			}
#endif
		} else {
			obuf[1] = 0xff; // Error cmd
			olen = 2;
		}
	}
	return olen;
}
