/* Host unit test for the IBSTH2P UART framing layer (source/ucap_frame.h).
 *
 * Build & run:  gcc -Wall -Wextra -o test_ucap_frame test_ucap_frame.c && ./test_ucap_frame
 *
 * Regression focus: the V15..V17 parser restarted on any 0x52 ('R') byte,
 * so a frame whose payload contained 0x52 — notably main-MCU battery byte
 * [9] at 82% — broke framing for as long as that value persisted.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Host-side CRC-16/MODBUS, same algorithm as crc16() in ble_ota.c. */
unsigned short crc16(unsigned char *pD, int len) {
	static unsigned short poly[2] = { 0, 0xa001 };
	unsigned short crc = 0xffff;
	int i, j;
	for (j = len; j > 0; j--) {
		unsigned char ds = *pD++;
		for (i = 0; i < 8; i++) {
			crc = (crc >> 1) ^ poly[(crc ^ ds) & 1];
			ds = ds >> 1;
		}
	}
	return crc;
}

#include "../source/ucap_frame.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
	if (cond) { printf("PASS: " __VA_ARGS__); printf("\n"); } \
	else { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* Build a valid 13-byte frame with the given payload bytes [1..9]. */
static void mk_frame(uint8_t *out, const uint8_t *payload9) {
	uint16_t crc;
	out[0] = UCAP_FRAME_START;
	memcpy(&out[1], payload9, 9);
	crc = crc16(&out[1], 9);
	out[10] = crc & 0xFF;
	out[11] = (crc >> 8) & 0xFF;
	out[12] = UCAP_FRAME_END;
}

/* Feed a byte stream; count validated frames and remember the last one. */
static int run(ucap_frame_t *s, const uint8_t *stream, int len, uint8_t *last) {
	int got = 0;
	for (int i = 0; i < len; i++) {
		if (ucap_frame_feed(s, stream[i])) {
			got++;
			if (last)
				memcpy(last, s->buf, UCAP_FRAME_SIZE);
		}
	}
	return got;
}

int main(void) {
	/* CRC algorithm against an independently computed vector (python crcmod). */
	{
		uint8_t v[9] = {0x01,0x02,0x92,0x09,0xE6,0x15,0x01,0x01,0x52};
		CHECK(crc16(v, 9) == 0x9EE9, "CRC-16/MODBUS matches independent test vector");
	}

	/* Typical frame: type 1, temp 24.50, humi 56.06, batt 83%. */
	uint8_t pl_normal[9]  = {0x00,0x00,0x92,0x09,0xE6,0x15,0x01,0x01,0x53};
	/* The killer: battery byte [9] = 0x52 (82%) == 'R'. */
	uint8_t pl_batt82[9]  = {0x00,0x00,0x92,0x09,0xE6,0x15,0x01,0x01,0x52};
	/* 0x52 in temperature low byte (21.30 C = 0x0852). */
	uint8_t pl_temp52[9]  = {0x00,0x00,0x52,0x08,0xE6,0x15,0x01,0x01,0x53};

	/* 1. Clean stream of 3 normal frames. */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[39];
		for (int i = 0; i < 3; i++) mk_frame(&st[13*i], pl_normal);
		int n = run(&s, st, sizeof(st), NULL);
		CHECK(n == 3 && s.crc_bad == 0, "3 clean frames parse (got %d, crc_bad %d)", n, s.crc_bad);
	}

	/* 2. REGRESSION: battery = 82% (payload contains 'R'). Old parser: 0 frames forever. */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[13*5], last[13];
		for (int i = 0; i < 5; i++) mk_frame(&st[13*i], pl_batt82);
		int n = run(&s, st, sizeof(st), last);
		CHECK(n == 5 && s.crc_bad == 0, "5 batt=82%% frames parse (got %d, crc_bad %d)", n, s.crc_bad);
		CHECK(last[9] == 0x52, "battery byte preserved (0x%02x)", last[9]);
	}

	/* 3. 0x52 in temperature bytes. */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[13*4], last[13];
		for (int i = 0; i < 4; i++) mk_frame(&st[13*i], pl_temp52);
		int n = run(&s, st, sizeof(st), last);
		CHECK(n == 4, "4 temp-contains-'R' frames parse (got %d)", n);
		CHECK(last[3] == 0x52 && last[4] == 0x08, "temp bytes preserved");
	}

	/* 4. Corrupted byte -> frame rejected, next frame still parses. */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[26];
		mk_frame(&st[0], pl_normal);
		st[4] ^= 0x10;                       /* flip a temperature bit */
		mk_frame(&st[13], pl_normal);
		int n = run(&s, st, sizeof(st), NULL);
		CHECK(n == 1 && s.crc_bad >= 1, "corrupted frame rejected, next parses (got %d, crc_bad %d)", n, s.crc_bad);
	}

	/* 5. Corrupted button byte [8] must NOT be accepted (phantom press guard). */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[13];
		mk_frame(st, pl_normal);
		st[8] ^= 0x01;                       /* flip BLE-state byte, CRC now wrong */
		int n = run(&s, st, sizeof(st), NULL);
		CHECK(n == 0 && s.crc_bad >= 1, "frame with corrupted button byte rejected");
	}

	/* 6. Truncated frame followed by a full frame: resync recovers. */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[6 + 13], last[13];
		uint8_t full[13]; mk_frame(full, pl_normal);
		memcpy(st, full, 6);                 /* first 6 bytes then the MCU hiccups */
		mk_frame(&st[6], pl_batt82);         /* complete frame follows */
		int n = run(&s, st, sizeof(st), last);
		CHECK(n == 1 && last[9] == 0x52, "resync after truncated frame (got %d)", n);
	}

	/* 7. Noise between frames is counted, frames still parse. */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[3 + 13 + 2 + 13];
		st[0]=0x00; st[1]=0xFF; st[2]=0x17;
		mk_frame(&st[3], pl_normal);
		st[16]=0x99; st[17]=0x42;
		mk_frame(&st[18], pl_batt82);
		int n = run(&s, st, sizeof(st), NULL);
		CHECK(n == 2 && s.noise == 5, "noise skipped and counted (got %d, noise %d)", n, (int)s.noise);
	}

	/* 8. Random-ish garbage stream never produces a false frame. */
	{
		ucap_frame_t s; memset(&s, 0, sizeof(s));
		uint8_t st[512];
		uint32_t x = 12345;
		for (unsigned i = 0; i < sizeof(st); i++) { x = x*1103515245+12345; st[i] = (x>>16) & 0xFF; }
		int n = run(&s, st, sizeof(st), NULL);
		CHECK(n == 0, "512 pseudo-random bytes yield no false frames (got %d)", n);
	}

	printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASS\n", failures);
	return failures ? 1 : 0;
}
