/*
 * Host-side test for flash_eep.c bank compaction (pack_cfg_fmem).
 *
 * Audit finding: pack_cfg_fmem() never advances wraddr, so when a config
 * bank fills and gets repacked into the next bank, every surviving object
 * is written to the SAME address (fnewseg + 4). NOR-flash semantics mean
 * the writes AND together, corrupting every stored object (MAC, config,
 * device name...).
 *
 * Build & run:
 *   gcc -o t_flash_eep test_flash_eep.c && ./t_flash_eep        # current code
 *   gcc -DWITH_FIX -o t_flash_eep test_flash_eep.c && ./t_flash_eep  # candidate fix
 *
 * Emulates 512 KB NOR flash in RAM: erase = 0xFF fill, write = bitwise AND
 * (a NOR write can only clear bits), which is exactly why the overlapping
 * writes are destructive rather than "last one wins".
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---- minimal environment stubs so flash_eep.c compiles on the host ---- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int bool;
#define __ATTR_SECTION_XIP__

static uint8_t sim_flash[512 * 1024];
#define FLASH_BASE_ADDR ((uintptr_t)sim_flash)

static int err_cnt = 0;

static void hal_flash_erase_sector(uintptr_t addr) {
	memset((void *)addr, 0xFF, 4096);
}
static void flash_write_word(uintptr_t addr, uint32_t wd) {
	uint32_t cur;
	memcpy(&cur, (void *)addr, 4);
	cur &= wd; /* NOR: can only clear bits */
	memcpy((void *)addr, &cur, 4);
}
static void hal_flash_write(uintptr_t addr, unsigned char *p, unsigned int len) {
	unsigned char *d = (unsigned char *)addr;
	for (unsigned int i = 0; i < len; i++)
		d[i] &= p[i]; /* NOR AND semantics */
}

/* pull in the code under test (its own header defines the geometry) */
#define _GNU_SOURCE
#include "../source/flash_eep.h"
#include "../source/flash_eep.c"

/* ---------------------------------------------------------------------- */
static int check(int ok, const char *what) {
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) err_cnt++;
	return ok;
}

int main(void) {
	memset(sim_flash, 0xFF, sizeof(sim_flash));

	/* realistic object set for an IBSTH2P after first boot */
	uint8_t mac[8] = {0x39, 0xB3, 0x97, 0x8D, 0x1F, 0x38, 0, 0};
	uint32_t ver = 22;
	uint8_t cfg[12] = {0};
	char name[16] = "myfridge";

	check(flash_write_cfg(mac, EEP_ID_MAC, 8), "store MAC");
	check(flash_write_cfg(&ver, EEP_ID_VER, sizeof(ver)), "store version");
	check(flash_write_cfg(name, EEP_ID_DVN, sizeof(name)), "store device name");

	/* now simulate years of config changes until the bank fills and packs.
	 * Each changed cfg write appends align(4+12)=16 bytes; bank is 4096. */
	int packs_hit = 0;
	for (int i = 0; i < 600; i++) {
		cfg[0] = (uint8_t)i;
		cfg[1] = (uint8_t)(i >> 8);
		if (!flash_write_cfg(cfg, EEP_ID_CFG, sizeof(cfg))) {
			printf("write_cfg failed at iteration %d\n", i);
			err_cnt++;
			break;
		}
		/* detect that a pack happened: current bank counter changed base */
		static unsigned int last_base = 0;
		unsigned int base = get_addr_bscfg();
		if (last_base && base != last_base)
			packs_hit++;
		last_base = base;
	}
	printf("bank switches (packs) during test: %d\n", packs_hit);
	check(packs_hit >= 1, "compaction was exercised at least once");

	/* after packing, is everything still intact? */
	uint8_t rmac[8] = {0};
	uint32_t rver = 0;
	char rname[16] = {0};
	uint8_t rcfg[12] = {0};

	int mac_len = flash_read_cfg(rmac, EEP_ID_MAC, 8);
	int ver_len = flash_read_cfg(&rver, EEP_ID_VER, sizeof(rver));
	int name_len = flash_read_cfg(rname, EEP_ID_DVN, sizeof(rname));
	int cfg_len = flash_read_cfg(rcfg, EEP_ID_CFG, sizeof(rcfg));

	check(mac_len == 8 && memcmp(rmac, mac, 8) == 0, "MAC survives compaction");
	check(ver_len == 4 && rver == ver, "version record survives compaction");
	check(name_len == 16 && memcmp(rname, name, 16) == 0, "device name survives compaction");
	check(cfg_len == 12 && memcmp(rcfg, cfg, 12) == 0, "latest cfg readable after compaction");

	printf("\n%s\n", err_cnt ? "FAILURES PRESENT" : "ALL TESTS PASS");
	return err_cnt ? 1 : 0;
}
