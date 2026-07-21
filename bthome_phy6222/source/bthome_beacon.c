/*
 * bthome_beacon.c
 *
 *  Created on: 17.10.23
 *      Author: pvvx
 */
#include "rom_sym_def.h"
#include "types.h"
#include "config.h"
#include "bcomdef.h"
#include "gapbondmgr.h"
#include "sensors.h"
#include "bthome_beacon.h"
#include "findmy_beacon.h"

#if (DEV_SERVICES & SERVICE_BINDKEY)

#include "thb2_main.h"
#include "ccm.h"
#include "flash_eep.h"
#include "ll.h"
#include "thb2_peripheral.h"

/* Encrypted bthome nonce */
typedef struct __attribute__((packed)) _bthome_beacon_nonce_t{
    uint8_t  mac[6];
    uint16_t uuid16;	// = 0xfcd2
    uint8_t  info;		// = 0x41
	uint32_t cnt32;
} bthome_beacon_nonce_t, * pbthome_beacon_nonce_t;

bthome_beacon_nonce_t bthome_nonce;
uint8_t bindkey[16];

void bthome_beacon_init(void) {
	// SwapMacAddress(bthome_nonce.mac, ownPublicAddr);
	bthome_nonce.mac[0] = ownPublicAddr[5];
	bthome_nonce.mac[1] = ownPublicAddr[4];
	bthome_nonce.mac[2] = ownPublicAddr[3];
	bthome_nonce.mac[3] = ownPublicAddr[2];
	bthome_nonce.mac[4] = ownPublicAddr[1];
	bthome_nonce.mac[5] = ownPublicAddr[0];
	bthome_nonce.uuid16 = ADV_BTHOME_UUID16;
	bthome_nonce.info = BtHomeID_Info_Encrypt;
	if (flash_read_cfg(bindkey, EEP_ID_KEY, sizeof(bindkey))
			!= sizeof(bindkey)) {
		LL_Rand(bindkey, sizeof(bindkey));
		flash_write_cfg(bindkey, EEP_ID_KEY, sizeof(bindkey));
	}
}

uint8_t adv_encrypt(uint8_t * p, uint8_t data_size) {
	uint8_t *pmic = &p[data_size];
	bthome_nonce.cnt32 = measured_data.count;
	*pmic++ = (uint8_t)measured_data.count;
	*pmic++ = (uint8_t)(measured_data.count>>8);
	*pmic++ = (uint8_t)(measured_data.count>>16);
	*pmic++ = (uint8_t)(measured_data.count>>24);
	ccm_auth_crypt(CCM_ENCRYPT, (const unsigned char *)&bindkey,
					   (uint8_t*)&bthome_nonce, sizeof(bthome_nonce),
					   (const unsigned char *)p, data_size,
					   p,
					   pmic, 4);
	return data_size + 4 + 4; // + mic + count
}
#endif // (DEV_SERVICES & SERVICE_BINDKEY)

#if (DEV_SERVICES & SERVICE_THS)

uint8_t adv_set_data(void * pd) {
	padv_bthome_data1_t p = (padv_bthome_data1_t)pd;
	p->b_id = BtHomeID_battery;
	p->battery_level = measured_data.battery;
	p->t_id = BtHomeID_temperature;
	p->temperature = measured_data.temp; // x0.01 C
	p->h_id = BtHomeID_humidity;
	p->humidity = measured_data.humi; // x0.01 %
	p->v_id = BtHomeID_voltage;
	p->battery_mv = measured_data.battery_mv; // x mV
	uint8_t len = sizeof(adv_bthome_data1_t);
#if DEVICE == DEVICE_IBSTH2P
	// Append a BTHome button object during a press burst so Home Assistant
	// fires a per-device button trigger. Object id 0x3a > 0x0c (voltage),
	// so it keeps the required ascending object order. adv_button_press is
	// held for a few broadcasts with a frozen packet id (see adv_measure),
	// which HA de-duplicates into a single press event.
	extern volatile uint8_t adv_button_press;
	if (adv_button_press) {
		uint8_t *b = (uint8_t *)pd + len;
		b[0] = BtHomeID_button; // 0x3a
		b[1] = 0x01;            // 0x01 = press
		len += 2;
	} else {
		// Append the BTHome firmware version object (0xf2, uint24
		// little-endian: patch, minor, major), readable by any scanner
		// implementing the full BTHome v2 spec (e.g. bthome_monitor.py).
		// Note: Home Assistant's bthome-ble parser (verified at 3.23.5)
		// does not know 0xf2 yet; because it is the last object it is
		// ignored without affecting the sensor entities, and it will
		// surface automatically once bthome-ble learns the device-info
		// objects. Object id 0xf2 is the highest we send, keeping the
		// required ascending object order. Mutually exclusive with the button object above:
		// that bounds the worst-case advertisement (encrypted build,
		// 8 bytes counter+MIC overhead) at exactly the 31-byte legacy
		// limit; the plain build stays at 25 bytes. The version returns
		// with the first post-burst packet, so it is only absent for the
		// ~60 s button hold.
		uint8_t *b = (uint8_t *)pd + len;
		b[0] = BtHomeID_fw_version24; // 0xf2
		b[1] = 0;                     // patch
		b[2] = 0;                     // minor
		b[3] = IBS_FW_VERSION;        // major ("VNN" -> NN.0.0)
		len += 4;
	}
#endif
	return len;
}

#else

uint8_t adv_set_data(void * pd) {
	padv_bthome_data2_t p = (padv_bthome_data2_t)pd;
	p->b_id = BtHomeID_battery;
	p->battery_level = measured_data.battery;
	p->v_id = BtHomeID_voltage;
	p->battery_mv = measured_data.battery_mv; // x mV
#if (DEV_SERVICES & SERVICE_BUTTON)
	p->u_id = BtHomeID_button; // or BtHomeID_opened ?
	p->button = measured_data.button;
	p->c_id = BtHomeID_count32;
	p->counter = adv_wrk.rds_count;
#elif (DEV_SERVICES & SERVICE_RDS)
	p->o_id = BtHomeID_opened;
	p->opened = measured_data.flg.pin_input;
	p->c_id = BtHomeID_count32;
	p->counter = adv_wrk.rds_count;
#endif
	return sizeof(adv_bthome_data2_t);
}

#endif

#if (DEV_SERVICES & SERVICE_RDS)
uint8_t adv_set_event(void * ped) {
	padv_bthome_event1_t p = (padv_bthome_event1_t)ped;
	p->o_id = BtHomeID_opened;
	p->opened = measured_data.flg.pin_input;
	p->c_id = BtHomeID_count32;
	p->counter = adv_wrk.rds_count;
	return sizeof(adv_bthome_event1_t);
}
#elif (DEV_SERVICES & SERVICE_BUTTON)
uint8_t adv_set_event(void * ped) {
	padv_bthome_event1_t p = (padv_bthome_event1_t)ped;
	p->b_id = BtHomeID_button; // or BtHomeID_opened ?
	p->button = measured_data.button;
	p->c_id = BtHomeID_count32;
	p->counter = adv_wrk.rds_count;
	return sizeof(adv_bthome_event1_t);
}
#endif

uint8_t bthome_data_beacon(void * padbuf) {
#if (DEV_SERVICES & SERVICE_FINDMY)
	if (adv_wrk.adv_event == 0 && (cfg.flg & FLG_FINDMY)) {
		gapRole_AdvEventType = LL_ADV_NONCONNECTABLE_UNDIRECTED_EVT;
		return  findmy_beacon(padbuf);
	} else
		gapRole_AdvEventType = LL_ADV_CONNECTABLE_UNDIRECTED_EVT;
#endif
	padv_bthome_noencrypt_t p = (padv_bthome_noencrypt_t)padbuf;
	p->flag[0] = 0x02; // size
	p->flag[1] = GAP_ADTYPE_FLAGS; // type
	/*	Flags:
	 	bit0: LE Limited Discoverable Mode
		bit1: LE General Discoverable Mode
		bit2: BR/EDR Not Supported
		bit3: Simultaneous LE and BR/EDR to Same Device Capable (Controller)
		bit4: Simultaneous LE and BR/EDR to Same Device Capable (Host)
		bit5..7: Reserved
	*/
	p->flag[2] = GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED | GAP_ADTYPE_FLAGS_GENERAL; // Flags
	p->head.type = GAP_ADTYPE_SERVICE_DATA; // 16-bit UUID
	p->head.UUID = ADV_BTHOME_UUID16;
#if (DEV_SERVICES & SERVICE_BUTTON)
#if (DEV_SERVICES & SERVICE_BINDKEY)
	if (cfg.flg & FLG_ADV_CRYPT) {
		padv_bthome_encrypt_t pe = (padv_bthome_encrypt_t)p;
		pe->info = BtHomeID_Info_Encrypt;
		p->head.size = adv_encrypt(pe->data, adv_set_data(pe->data)) + sizeof(pe->head) - sizeof(pe->head.size) + sizeof(pe->info);
	} else
#endif	// (DEV_SERVICES & SERVICE_BINDKEY)
	{
		p->info = BtHomeID_Info;
		p->p_id = BtHomeID_PacketId;
		p->pid = (uint8)measured_data.count;
		p->head.size = adv_set_data(p->data) + sizeof(p->head) - sizeof(p->head.size) + sizeof(p->info) + sizeof(p->p_id) + sizeof(p->pid);
#else // !(DEV_SERVICES & SERVICE_BUTTON)

#if (DEV_SERVICES & SERVICE_BINDKEY)
	if (cfg.flg & FLG_ADV_CRYPT) {
		padv_bthome_encrypt_t pe = (padv_bthome_encrypt_t)p;
		pe->info = BtHomeID_Info_Encrypt;
#if (DEV_SERVICES & (SERVICE_RDS | SERVICE_BUTTON))
		if(adv_wrk.adv_event) {
			p->head.size = adv_encrypt(pe->data, adv_set_event(pe->data)) + sizeof(pe->head) - sizeof(pe->head.size) + sizeof(pe->info);
		} else
#endif
		{
			p->head.size = adv_encrypt(pe->data, adv_set_data(pe->data)) + sizeof(pe->head) - sizeof(pe->head.size) + sizeof(pe->info);
		}
	} else
#endif	// (DEV_SERVICES & SERVICE_BINDKEY)
	{
		p->info = BtHomeID_Info;
		p->p_id = BtHomeID_PacketId;
		p->pid = (uint8)measured_data.count;
#if (DEV_SERVICES & (SERVICE_RDS | SERVICE_BUTTON))
		if(adv_wrk.adv_event) {
			p->head.size = adv_set_event(p->data) + sizeof(p->head) - sizeof(p->head.size) + sizeof(p->info) + sizeof(p->p_id) + sizeof(p->pid);
		} else
#endif
		{
			p->head.size = adv_set_data(p->data) + sizeof(p->head) - sizeof(p->head.size) + sizeof(p->info) + sizeof(p->p_id) + sizeof(p->pid);
		}
#endif
	}
	return p->head.size + sizeof(p->flag) + 1;
}

