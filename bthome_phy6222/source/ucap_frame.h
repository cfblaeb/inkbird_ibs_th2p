/******************************************************************************
 * ucap_frame.h — framing layer for the IBSTH2P inter-chip UART stream.
 *
 * Pure C with no SDK dependencies so it can be unit-tested on the host
 * (tests/test_ucap_frame.c). The byte-feed state machine collects 13-byte
 * 'R'...'E' frames and validates the end marker plus the stock protocol's
 * CRC-16/MODBUS before a frame is accepted.
 *
 * Design notes (fixes for the V15..V17 parser):
 *  - Payload bytes may legally equal the 'R' start marker (e.g. the main
 *    MCU battery byte [9] is 0x52 at 82%), so a mid-frame 'R' must NOT
 *    restart collection: with the old restart-on-'R' behavior, every frame
 *    failed while the battery reported 82%. Resync happens only after a
 *    full buffer fails validation, by sliding to the next 'R' inside it.
 *  - Frames carry CRC-16/MODBUS over bytes [1..9] at [10..11] (LE); the
 *    old parser ignored it and accepted corrupted temperature/humidity/
 *    button state. Now a frame is only accepted when marker and CRC match.
 ******************************************************************************/
#ifndef _UCAP_FRAME_H_
#define _UCAP_FRAME_H_

#include <stdint.h>
#include <string.h>

#define UCAP_FRAME_START 0x52  // 'R'
#define UCAP_FRAME_END   0x45  // 'E'
#define UCAP_FRAME_SIZE  13

// CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF); implemented in
// ble_ota.c on the target, by the test harness on the host.
extern unsigned short crc16(unsigned char *pD, int len);

typedef struct {
	uint8_t buf[UCAP_FRAME_SIZE];
	uint8_t pos;
	uint8_t in_frame;
	volatile uint16_t crc_bad;  // full buffers that failed validation
	volatile uint32_t noise;    // bytes received outside any frame
} ucap_frame_t;

// End marker + CRC check over a complete 13-byte buffer.
static int ucap_frame_valid(const uint8_t *f) {
	if (f[UCAP_FRAME_SIZE - 1] != UCAP_FRAME_END)
		return 0;
	return crc16((unsigned char *)&f[1], 9)
			== (uint16_t)(f[10] | (f[11] << 8));
}

// Feed one received byte. Returns 1 when s->buf holds a validated frame
// (consume it before feeding the next byte); 0 otherwise.
static int ucap_frame_feed(ucap_frame_t *s, uint8_t b) {
	if (!s->in_frame) {
		if (b == UCAP_FRAME_START) {
			s->buf[0] = b;
			s->pos = 1;
			s->in_frame = 1;
		} else {
			s->noise++;
		}
		return 0;
	}
	s->buf[s->pos++] = b;
	if (s->pos < UCAP_FRAME_SIZE)
		return 0;
	if (ucap_frame_valid(s->buf)) {
		s->in_frame = 0;
		s->pos = 0;
		return 1;
	}
	// Validation failed: resync to the next 'R' inside the buffer and keep
	// collecting. Each pass consumes at least one byte, so this converges.
	s->crc_bad++;
	{
		uint8_t i, k;
		for (i = 1; i < UCAP_FRAME_SIZE && s->buf[i] != UCAP_FRAME_START; i++)
			;
		if (i < UCAP_FRAME_SIZE) {
			// Left shift in place (forward copy is safe for overlap;
			// avoids memmove, which the ROM symbol table lacks).
			for (k = 0; k < UCAP_FRAME_SIZE - i; k++)
				s->buf[k] = s->buf[k + i];
			s->pos = UCAP_FRAME_SIZE - i;
		} else {
			s->in_frame = 0;
			s->pos = 0;
		}
	}
	return 0;
}

#endif // _UCAP_FRAME_H_
