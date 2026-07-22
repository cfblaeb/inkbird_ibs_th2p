# Code Audit Findings — 2026-07-22

Output of a 9-lens multi-agent audit of the custom firmware (master @ V22,
`d043da3`) plus the fleet tooling. **Every entry below is a raw finder claim,
unverified unless its Status line says otherwise.** Verification protocol per
finding: (1) technical re-read of the actual code, (2) cross-module check —
does anything depend on the "buggy" behavior (the V20 WAITING_AFTER_TIMEOUT
lesson), (3) reachability in the shipped IBSTH2P BOOT_OTA image and realistic
operation. Statuses: unverified / CONFIRMED / REFUTED (with reason) /
FIXED (with commit).

Duplicate clusters (same underlying issue reported by several finders):
- #3, #7, #14 — battery.c low-vbat 60-min sleep vs 24-bit RTC comparator
- #8, #25, #36 — placeholder GPIO_KEY P07 armed as IRQ + wake source
- #15, #16, #22, #27, #28, #33, #39 — UART grab windows opened outside the
  V21 sync engine, no close timer (several distinct call sites — verify each)
- #2, #4, #5 (+#44 not-in-build) — V21 adv_restart_pending / END_DISCOVERABLE
  bounce races
- #12, #20 — fix_mac in-place sector-0 erase
- #11, #21 — OTA bounds checked against 2 MB constant on 512 KB part

### 1. [HIGH] `bthome_phy6222/source/flash_eep.c:273` — EEP compaction never advances wraddr: every repack overlap-corrupts all stored objects

*Status: **CONFIRMED — WON'T FIX** · Verified 2026-07-22*

Verification: host-side repro `bthome_phy6222/tests/test_flash_eep.c` —
`pack_cfg_fmem()` never advances `wraddr`, so bank compaction writes every
surviving object to the same address (NOR AND-semantics), destroying
MAC/version/name; a two-line fix (`wraddr += cplen`) passes the same test.
The test stays in the tree as documentation of the analysis.

Won't-fix rationale (this device, this repo): the EEP area stores only
identity + settings, appended only on *change*. IBSTH2P write census:
~3 records at first boot (MAC 12 B, name stub 4 B, version 8 B) plus one
8-byte version record per firmware upgrade. Settings are hard-pinned in
`test_config()` and never changed over BLE in this deployment, so the
4092-byte bank fills after ~500 firmware upgrades — compaction is
effectively unreachable for the life of the hardware. If a future feature
ever starts writing config records at runtime, revisit this first.*changed* config writes (~250 cfg saves or a mix of upgrades/name changes) — long fuse, dev/bench units at highest risk. Not yet fixed in source · Reachability: shipped build*

In pack_cfg_fmem() the destination pointer is set once (`wraddr = fnewseg + 4;`, line 257) and never incremented after `_flash_write(wraddr, len, pbuf)` (line 273). Every surviving object (EEP_ID_VER, EEP_ID_MAC, EEP_ID_DVN, EEP_ID_CFS, ...) is written to the SAME flash address in the new bank; NOR writes AND bits together, so the second copy corrupts the first's header and data, and the merged garbage header derails all subsequent traversal (get_addr_fobj / get_addr_fobj_save walk AND'd data as headers). This bug was introduced in the THB2 port: pvvx's earlier ATC_MiThermometer flash_eep.c advances the write pointer (`wraddr += ...` after each copy, verified against upstream); pvvx/THB2 master carries the same regression, so it is inherited, not local. Compaction triggers when the 4 KB bank fills — roughly 250 config/name writes (each changed CMD_ID_CFG save is a 16-byte record) — so it is guaranteed to fire eventually on any unit whose settings are ever changed, e.g. via CMD_ID_DNAME fleet naming or CMD_ID_EEP_RW debugging.

**Failure scenario:** Device accumulates ~250 EEP writes (config changes, name sets, EEP_RW debug writes) -> bank full -> next flash_write_cfg() calls pack_cfg_fmem() -> VER, MAC, DVN, CFS records are all written on top of each other at fnewseg+4 and destroyed. On next boot flash_supported_eep_ver() cannot read EEP_ID_VER, erases all 4 banks and loads defaults: custom device name, sensor calibration, and any MAC set via CMD_ID_DEV_MAC are silently lost; a unit whose chip ID is invalid (set_mac fallback) comes back with a NEW random MAC, permanently breaking its Home Assistant / fleet identity.

### 2. [MEDIUM] `bthome_phy6222/SDK/components/driver/uart/uart.c:278` — UART init with tx_pin=GPIO_DUMMY writes OOB garbage into AON PMCTL0 on every wakeup

*Status: **CONFIRMED** (code trace, V22) — one defect complex with #4 and #5.*

Verified trace: `set_new_adv_interval()` (thb2_main.c:220) is a non-atomic
"bounce": it queues `GAP_EndDiscoverable`, then directly fakes
`gapRole_state = GAPROLE_WAITING_AFTER_TIMEOUT` and counts
`adv_restart_pending++`, relying on the *next* `GAP_END_DISCOVERABLE_DONE`
event to consume the marker. Any real GAP event interleaving with that
in-flight bounce breaks the protocol:

1. **Connect wins the race** (this finding + #4): CONNECT_IND accepted
   before the adv-disable completes -> `GAP_LINK_ESTABLISHED` processed
   first: state=CONNECTED, callback zeroes `adv_restart_pending`, locks
   MOD_USR0, starts the 10 s grab timer. The late `END_DISCOVERABLE_DONE`
   (status SUCCESS) then falls into the else-branch (thb2_peripheral.c:1143):
   state := GAPROLE_WAITING + notify -> the app runs the FULL disconnect
   cleanup **while the connection is live**: MOD_USR0 unlocked (chip sleeps
   mid-connection — the exact instability the lock exists to prevent),
   TIMER_BATT_EVT stopped (no measurements during the connection), advert
   rebuilt, and `wrk.reboot` honored if set. State is left WAITING while
   actually connected.
2. **Bounce issued while not advertising** (#5, via KEY_CHANGE ->
   `increase_advertising_frequency()` during a connection — reachable in the
   shipped build only through the floating-P07 glitch, finding #8):
   `GAP_EndDiscoverable` fails -> END_DONE arrives with status != SUCCESS ->
   handler sets GAPROLE_ERROR *without consuming the pending marker* ->
   `adv_restart_pending` strands at 1 -> the NEXT real supervision timeout is
   mistaken for a bounce (thb2_main.c:1180 decrements and skips cleanup) ->
   the V20 MOD_USR0 sleep-lock leak is back for exactly that disconnect.

Trigger frequency for case 1: the bounce fires one advertising event
(<=10 s) after EVERY disconnect (`adv_reload_count = 1` in the WAITING
cleanup -> interval restore at thb2_main.c:412/426) and 60 s after every
boot (V18 fast window expiry). A client connecting inside that window races
it — realistic during fleet flashing (connect/disconnect/reconnect cycles)
and battery-pull + connect workflows. Ordering caveat: LINK_ESTABLISHED
processing before END_DISCOVERABLE_DONE requires the connection event to
be queued first; the window is one advertising event, so the race is rare
per attempt but the attempt count is high over a fleet's life.

Proposed fix (design-level, not yet applied): stop borrowing GAP states.
`thb2_peripheral.c` is app-owned source — add a dedicated
`gapRole_AdvRestartReq` flag consumed inside the END_DISCOVERABLE_DONE
handler (re-enable adv, no fake state, no app notification), and have
`set_new_adv_interval()` set that flag instead of touching
`gapRole_state`. This removes the fake-state protocol entirely:
CONNECTED no longer needs to zero a marker, WAITING_AFTER_TIMEOUT means
only real supervision timeouts again, and both race arms above become
impossible by construction. Must also make the END_DONE error path clear
the flag (case 2).*

The app configures UART0 RX-only with cfg.tx_pin = GPIO_DUMMY (0xFF) in ucap_init (bthome_phy6222/source/cmd_parser.c:263). The driver guards GPIO_DUMMY in hal_gpio_fmux_set (gpio.c:196) but NOT in the hal_gpio_pull_set(pcfg->tx_pin, GPIO_PULL_UP) call at uart.c:278. hal_gpio_pull_set (gpio.c:324-339) has its bounds check compiled out (TEST_PIN_NUM=0) and indexes the 23-entry c_gpio_pull table at [255], reading 3 garbage bytes ~700 bytes past the table in flash rodata; since 255 >= P31 it then executes subWriteReg(&AP_AON->PMCTL0, h, l, 2) with those garbage bit positions. PMCTL0 holds the pull/wakeup-polarity configuration for P31-P34. This runs on every hal_uart_init: at boot, on every listen-window open (ucap_start_grab does deinit+init), and on EVERY wakeup from sleep via the driver's own uart_wakeup_process_0 wakeup handler (uart.c:403-405) — thousands of times per day. The written bits are deterministic per build (the current V22 binary is evidently benign — hardware validated at 10-15 uA), but any relink that moves rodata changes which PMCTL0 bits get smashed.

**Failure scenario:** Rebuild/relink (e.g. V23) shifts the rodata after c_gpio_pull so c_gpio_pull[255] decodes to h/l covering P33/P34 pull bits -> every wakeup clears the pulldowns on those unused pins -> floating inputs leak tens of uA continuously (or wake-polarity bits flip, causing spurious/failed GPIO wakes). Nothing in testing flags it because the write is silent and per-build constant.

### 3. [MEDIUM] `bthome_phy6222/source/battery.c:183` — low_vbat 60-min sleep tick overflows 24-bit RTC comparator (wakes every 16 s)

*Status: **CONFIRMED** (arithmetic + SDK trace) — duplicates: #7, #14.*

Verified: `low_vbat()` (battery.c:183) requests
`hal_pwrmgr_enter_sleep_rtc_reset((60*60)<<15)` = 117,964,800 ticks. The SDK
(`config_RTC1`, SDK/lib/rf/patch.c:5610) programs the wake comparator as
`AP_AON->RTCCC0 = RTCCNT + time` with no range check. The PHY62xx AON RTC
counter/comparator is 24-bit (SDK's own mesh code masks `RTCCNT & 0xffffff`,
appl_prov.c:22; config.h reference comment "max 512 sec"), so only the low
24 bits of the sum matter: 117,964,800 mod 2^24 = 524,288 ticks = exactly
16.0 s. The predicted symptom falls out of the arithmetic precisely: the
"60-minute" hibernate wakes after 16 seconds.

Aggravators found during verification:
- The wake is a warm RESET with SRAM retention cleared, and V18 makes every
  reset start the 60 s fast-advertising window (1.56 s interval). Net: a
  device that should hibernate on a dying battery instead reboots every
  16 s and spends its entire remaining life in the most radio-active mode
  the firmware has. End-of-life behavior is the opposite of the design
  intent, actively finishing off the battery.
- `check_battery()` (battery.c:191) trips `low_vbat()` on a SINGLE raw ADC
  reading < 2000 mV, before any averaging. A transient dip — cold battery in
  a freezer (this device's canonical use), or a radio TX burst — can fire
  it spuriously; each spurious trip costs a 16 s reboot cycle, and a cold
  aging battery can oscillate in and out of the loop.

Fix candidate (not yet applied): the wake-by-reset design cannot count
multiple comparator periods (RAM is cleared), so the honest fix is to clamp
the request to the hardware: sleep `510<<15` ticks (~8.5 min) per cycle.
Battery is re-checked at each boot anyway; 8.5-minute checks are 7x more
frequent than the intended 60 min but 32x less than the current 16 s, and
the constant stays within the 24-bit comparator by construction. Optionally
also gate `low_vbat()` on two consecutive low readings to kill the
transient-dip trigger.*

low_vbat() calls hal_pwrmgr_enter_sleep_rtc_reset((60*60)<<15) = 117,964,800 ticks. The AON RTC counter/comparator is 24-bit (wraps every 512 s at 32768 Hz; see the ucap_rtc() comment in cmd_parser.c:88-89 and the SDK's config_RTC1 in SDK/lib/rf/patch.c:5609, which writes sleep_tick + time straight into AP_AON->RTCCC0 with no range check). 117,964,800 mod 2^24 = 524,288 ticks = 16.0 s, so the intended 60-minute low-battery hibernation actually wakes (via warm reset, since hal_pwrmgr_enter_sleep_rtc_reset clears SRAM retention and reboots) after only 16 seconds — or, if the comparator latched the upper bits, never fires and the unit is dead until a battery pull. Every wake is a full boot: 60 s fast-advertising window at ~1.56 s interval, boot UART grab window holding MOD_UART0 for up to 15 s, ADC measure, then check_battery() -> low_vbat() again.

**Failure scenario:** Battery drops below 2000 mV (the fleet deliberately runs one V22 unit at 5% as a drain experiment). check_battery() calls low_vbat() intending one wake per hour at 1.67 uA. Instead the device cycles: ~5-20 s fully awake booting/fast-advertising/measuring, then 16 s sleep, forever — milliamp-level average draw plus boot activity below the 2 V threshold the code itself says is unsafe for flash writes (battery.c:191). The dying battery is flattened in hours-days instead of surviving weeks in protective hibernation, and the device spams fast advertisements the whole time.

### 4. [MEDIUM] `bthome_phy6222/source/battery.c:183` — low_vbat 60-minute sleep wraps 24-bit RTC: wakes after ~16 s, not 1 hour

*Status: **CONFIRMED** (code trace, V22) — same complex — see #2 for the verified trace and proposed fix (case 1).*

low_vbat() calls hal_pwrmgr_enter_sleep_rtc_reset((60*60)<<15) = 117,964,800 ticks, but the PHY6222 RTC counter/comparator is 24-bit (wraps at 16,777,216 ticks = 512 s). config_RTC1 (SDK/lib/rf/patch.c:5609) programs AP_AON->RTCCC0 = sleep_tick + time; only the low 24 bits are meaningful, and 117,964,800 mod 2^24 = 524,288 ticks = 16.0 s. It also enables the counter-overflow wake event (RTCCTL BIT(18)), so even under the alternative 32-bit-compare interpretation the chip wakes at the first RTC wrap (<=512 s). Either way the intended 1-hour low-battery standby is impossible.

**Failure scenario:** Battery drains below 2000 mV (the OTA_TYPE_BOOT threshold in check_battery); low_vbat() puts the chip into what is meant to be a 1.67 uA one-hour sleep-reset. Instead the RTC comparator matches after ~16 s, the chip warm-resets (SRAM retention cleared -> full reboot), runs full init including RF cal, the 60 s fast-advertising window, and an ADC battery measurement, sees <2 V again and re-sleeps: a permanent ~16 s reboot loop. Average draw is milliamp-level bursts every 16 s instead of one wake per hour, killing the already-critical battery in hours-days instead of the intended graceful standby, while spamming BLE boot advertisements. Directly relevant to the fleet unit deliberately running its battery at 5%.

### 5. [MEDIUM] `bthome_phy6222/source/battery.c:183` — low_vbat 60-min hibernate truncates to ~16 s in 24-bit RTC comparator: brownout boot-loop

*Status: **CONFIRMED** (code trace, V22) — same complex — see #2 for the verified trace and proposed fix (case 2, stranded marker).*

low_vbat() calls hal_pwrmgr_enter_sleep_rtc_reset((60*60)<<15) = 117,964,800 ticks intending a 60-minute sleep. hal_pwrmgr_enter_sleep_rtc_reset (SDK pwrmgr.c:471-487) passes this to config_RTC1, which writes AP_AON->RTCCC0 = sleep_tick + time (SDK/lib/rf/patch.c:5609). The RTC counter/comparator is 24-bit (wraps every 512 s), so the effective delta is 117,964,800 mod 2^24 = 524,288 ticks = 16 s. check_battery() (battery.c:190-192, the OTA_TYPE_BOOT branch in the shipped image) invokes low_vbat whenever a measurement reads < 2000 mV.

**Failure scenario:** A unit's 2xAAA pack sags below 2.0 V (the fleet deliberately runs one unit at 5% battery): instead of parking at 1.67 uA and re-checking hourly, the device warm-resets every ~16 s, and each reset runs the full radio bring-up plus the V18 60-s fast-advertising window (~1.5 s adv interval) and an ADC measurement before low_vbat fires again. The intended low-battery protection becomes a mA-scale reboot loop that flattens the dying cells in hours and keeps executing flash/config reads at brownout voltage.

### 6. [MEDIUM] `bthome_phy6222/source/ble_ota.c:157` — OTA bound uses 2 MB FLASH_MAX_SIZE on 512 KB part; wraps over EEP banks and firmware

*Status: unverified · Reachability: shipped build*

CMD_OTA_SET validates `program_offset + (pkt_total << 4) <= FADDR_START_ADDR + FLASH_MAX_SIZE` where FLASH_MAX_SIZE is 0x200000 (ble_ota.h:13) but the IBS-TH2P flash is 512 KB (FLASH_SIZE 0x80000). The default start path (msg_size==2 at line 162) applies NO size bound at all: pkt_total is taken from the image's own size field in packet 0 (line 202). hal_flash_write/hal_flash_erase_sector (SDK flash.c) perform no capacity check, and the SPI die ignores high address bits, so addresses past 0x80000 wrap to physical offset 0. The per-packet erase (line 232) and write (line 235) therefore march through the EEP config banks at 0x7C000-0x7FFFF at transfer offset ~432 KB, then wrap at 512 KB and erase/overwrite physical sector 0 (flash boot info) and the running firmware — all before the final CRC32 check (which only gates the boot-flag write) can reject the image.

**Failure scenario:** Operator builds an ota.bin from the wrong input (e.g. a fullflash-sized hex run through phy62x2_ota.py) producing a PHY6-headered image claiming > ~432 KB, and fleet-flashes it: at packet ~27648 the transfer erases the EEP banks (stored MAC/name/config gone), at packet 28672 it erases physical sector 0 and starts overwriting the live firmware -> device dead mid-transfer, recoverable only by UART/pogo-pin reflash.

### 7. [MEDIUM] `bthome_phy6222/source/cmd_parser.c:961` — CMD_ID_FIX_MAC erases physical flash sector 0 in place; power loss in window bricks device

*Status: **CONFIRMED** — duplicate of #3, see there for the verified analysis and fix candidate.*

write_fix_mac() (reachable via CMD_ID_FIX_MAC, compiled in the shipped OTA_TYPE_BOOT build, cmd_parser.c:1283) reads physical sector 0 into a 4 KB stack buffer, then erases it via the address-wrap trick (`hal_flash_erase_sector(FLASH_ADDR_RINFO + phy_flash.Capacity)` with Capacity |= 2 MB wraps to physical 0x00000 on the 512 KB die) and rewrites it in sixteen 256-byte chunks (lines 961-963). Sector 0 holds the PHY6222 ROM boot information (partition table, chip MAC area at 0x900). There is no staging copy: between the erase and the last chunk write the only copy of the boot sector is in RAM.

**Failure scenario:** User issues CMD_ID_FIX_MAC 1 over BLE to persist the MAC into the chip-info sector and the battery fails / is pulled during the ~50-100 ms erase+rewrite window -> sector 0 is blank or partial -> the ROM bootloader cannot find the boot record on next reset -> device does not boot at all; only recovery is direct UART flashing with the pogo-pin rig.

### 8. [MEDIUM] `bthome_phy6222/source/cmd_parser.c:1026` — CMD_ID_DEVID uses (dev_id_t*)&obuf: writes past pointer var, not into reply buffer

*Status: unverified · Reachability: shipped build*

In the CMD_ID_DEVID handler, after `memcpy(obuf, &dev_id, sizeof(dev_id))` the code does `dev_id_t *p = (dev_id_t *)&obuf; p->dev_spec_data = thsensor_cfg.sensor_type;`. `&obuf` is the address of the 4-byte `uint8_t *obuf` parameter on the stack, NOT the buffer it points to. `dev_spec_data` sits at byte offset 6 in dev_id_t (pid[0], revision[1], hw_version[2..3], sw_version[4..5], dev_spec_data[6..7]), so this stores a 16-bit value 2-3 bytes PAST the 4-byte pointer object, into whatever local the compiler placed adjacent to `obuf`. Two consequences: (1) the actual output buffer's dev_spec_data field is never patched, so the DEVID reply always carries the const-initializer value 0 instead of the real sensor type; (2) it is an out-of-bounds stack write. This is compiled in (SERVICE_THS is in the shipped THS|KEY|OTA set). Correct form is `(dev_id_t *)obuf`.

**Failure scenario:** A client (pvvx/custom app or nRF Connect script) connects and writes CMD_ID_DEVID (0x00) to the CMD characteristic 0xFFF4 - the standard device-identification handshake. cmd_parser runs line 1026: dev_spec_data (sensor type) is written to a stack slot at &obuf+6 rather than obuf[6..7]. The 12-byte DEVID notification returned reports dev_spec_data=0 (sensor type unknown) on every call, and a 2-byte stray write lands in an adjacent stack local of cmd_parser.

### 9. [MEDIUM] `bthome_phy6222/source/flash_eep.c:340` — Object header committed before data, no CRC: torn write reads back as valid record

*Status: unverified · Reachability: shipped build*

_flash_write_cfg() writes the 4-byte header {size,id} first (line 340) and the payload afterwards (line 344), and records carry no CRC or completion marker. Power loss between the two writes (or mid-payload) leaves a record whose header is fully valid but whose data is 0xFF/partial. flash_read_cfg() has no way to detect this and returns the garbage as the stored object. Because the record matches on id and size, it also shadows the previous good copy of the same object (get_addr_fobj returns the LAST record for an id).

**Failure scenario:** User sends CMD_ID_DEV_MAC (or first boot writes EEP_ID_MAC at thb2_main.c set_mac) and the battery is pulled/dies in the window between header and data write -> from then on every boot reads MAC = FF:FF:FF:FF:FF:FF from a structurally valid record and advertises with it; the corruption persists across reboots until another MAC write or full EEP wipe. Same pattern applies to EEP_ID_CFG (cfg.flg = 0xFFFFFFFF etc.) written during a battery brown-out.

### 10. [MEDIUM] `bthome_phy6222/source/thb2_main.c:224` — Bounce/connect race lets END_DISCOVERABLE_DONE unlock MOD_USR0 mid-connection

*Status: unverified · Reachability: shipped build*

set_new_adv_interval() ignores the result of GAP_EndDiscoverable() and force-fakes gapRole_state = GAPROLE_WAITING_AFTER_TIMEOUT. If a central's CONNECT_IND lands in the window between the bounce issuing GAP_EndDiscoverable and the LL actually disabling advertising (the interval-restore bounce runs exactly once per advertising restart, e.g. at fast-window expiry when users are connecting for OTA), GAP_LINK_ESTABLISHED is processed first: state becomes GAPROLE_CONNECTED, the app locks MOD_USR0 and starts TIMER_BATT. The still-pending GAP_END_DISCOVERABLE_DONE then arrives with state==GAPROLE_CONNECTED, and thb2_peripheral.c:1140-1143 falls into the else branch: it sets gapRole_state = GAPROLE_WAITING and gapRole_AdvEnabled = FALSE and notifies the app. The app's GAPROLE_WAITING cleanup (thb2_main.c:1190-1213) runs while the connection is live: it stops TIMER_BATT_EVT, calls hal_pwrmgr_unlock(MOD_USR0) (line 1195), rebuilds the advert, and would honor wrk.reboot — the same double-purpose-state aliasing family that produced the V20 storm.

**Failure scenario:** User connects (nRF Connect / OTA flasher) at the advertising event where adv_reload_count expires. The connection comes up, then milliseconds later the stale end-discoverable completion runs the disconnect cleanup: MOD_USR0 is released so the chip sleeps between connection events — the lock exists precisely because sleep during a connection is unstable on this device (comment at line 1145) — sensor reads stop (TIMER_BATT killed), and gapRole_state is corrupted to WAITING while connected. The connection/GATT session (e.g. an OTA transfer) stalls or drops; recovers only after the link dies and advertising restarts.

### 11. [MEDIUM] `bthome_phy6222/source/thb2_main.c:338` — adv_measure still opens unmanaged UART grab windows under UCAP_SYNC (no close timer)

*Status: unverified · Reachability: shipped build*

With UCAP_SYNC, cfg.measure_interval is pinned to 1 (config.c:173) and the design says the sync engine owns all listen windows. But adv_measure's SERVICE_THS branch still calls start_measure() when meas_count == measure_interval-1 == 0 (thb2_main.c:337-338), and GAPROLE_ADVERTISING resets meas_count = 0 on every advertising (re)start (thb2_main.c:1132). start_measure() -> ucap_start_grab() (sensors.c:150-156, cmd_parser.c:297-317) does hal_uart_deinit+init, hal_pwrmgr_lock(MOD_UART0) and sets grab_active=1 — but no SBP_UCAP_CLOSE_EVT is armed (only ucap_sync_open_evt arms one, cmd_parser.c:366-370). The lock is only released by the next good frame ISR (up to one full ~10.4 s main-MCU period away) or when the sync engine's next OPEN event adopts the window. The same unmanaged open happens from GAPROLE_CONNECTED (thb2_main.c:1153) and every 10 s from TIMER_BATT_EVT during a connection (thb2_main.c:860), where the deinit+init can also chop a frame mid-reception.

**Failure scenario:** After every disconnect or fast-window expiry (boot ~60 s mark, every supervision timeout, every clean disconnect), the first advertising event opens a rogue window: chip held fully awake with UART RX powered at 1-2 mA for up to ~10.4 s waiting for the next frame — the sleep the sync engine was built to avoid. Also, if the deinit lands while an off-schedule frame is mid-air, that frame is dropped and the sync engine logs a spurious miss (guard widens). Bounded and self-healing, but repeats on every advertising restart and silently costs ~10 s of awake time each.

### 12. [MEDIUM] `bthome_phy6222/source/thb2_main.c:521` — SERVICE_KEY edge-IRQ + sleep-wake armed on placeholder pin P07 in shipped image

*Status: unverified · Reachability: shipped build*

The shipped BOOT_OTA build has SERVICE_KEY in DEV_SERVICES (config.h:182), and config.h:219 defines GPIO_KEY as GPIO_P07 with the comment 'Not really. Just a placeholder.' — the IBS-TH2 Plus button is wired to the main MCU, not the PHY. init_app_gpio() (thb2_main.c:521) nevertheless calls hal_gpioin_register(GPIO_P07, ...) with both-edge handlers; hal_gpioin_enable leaves the pin at default pull (the pull_set call in the SDK is commented out, SDK gpio.c:639) and hal_gpio_sleep_handler arms it as a sleep wakeup source. Every edge on P07 wakes the chip and fires KEY_CHANGE_EVT; the key-up branch (thb2_main.c:1038) calls increase_advertising_frequency(), which drops the advertising interval to DEF_CON_ADV_INTERVAL (1.56 s) for 60 s and runs a stop/restart bounce.

**Failure scenario:** On any fleet unit where P07 is floating (drifting with the very humidity/temperature this device measures) or tied to an active net on the Inkbird PCB, spurious edges continuously wake the chip and re-trigger 60 s fast-advertising windows: the device advertises at ~6.4x the configured rate indefinitely plus takes a wakeup per edge — a silent battery-drain mode that looks healthy on air. Bench units are quiet, but the ~20-device fleet rollout has never verified P07's net.

### 13. [MEDIUM] `bthome_phy6222/source/thb2_main.c:861` — TIMER_BATT_EVT self-rearm survives disconnect: perpetual 10 s grab loop while advertising

*Status: unverified · Reachability: shipped build*

During a connection the IBSTH2P TIMER_BATT_EVT handler unconditionally rearms itself with osal_start_timerEx(..., 10000) (thb2_main.c:861). The disconnect cleanup (GAPROLE_WAITING/_AFTER_TIMEOUT, thb2_main.c:1192) and GAPROLE_ADVERTISING (thb2_main.c:1131) call osal_stop_timerEx, but stop_timerEx cannot clear an event the timer already posted to the task. If the link drops in the window between the timer expiring (event bit set) and the app task dispatching it, the handler runs once after the disconnect and rearms — and nothing stops it again until the next GAPROLE_CONNECTED/ADVERTISING transition. Each iteration runs read_sensors + start_measure -> the unmanaged ucap_start_grab of the previous finding, locking MOD_UART0 until the next main-MCU frame (mean ~5 s of every 10 s).

**Failure scenario:** Client disconnects (cleanly or by supervision timeout) at the wrong millisecond -> the device resumes normal-looking 10 s advertising but runs a hidden 10 s timer loop that keeps the UART RX powered ~50% duty (~0.5-1 mA average) indefinitely. Only the next connect/disconnect cycle or a battery pull clears it. Low probability per disconnect (~expiry-to-dispatch latency / 10 s), but across a 20-unit fleet with recurring connections it will eventually strand a unit in a drain state indistinguishable from the V15-V19 lock leak.

### 14. [MEDIUM] `bthome_phy6222/source/thb2_main.c:1180` — Stranded adv_restart_pending eats a real supervision timeout, re-leaking MOD_USR0

*Status: **CONFIRMED** — duplicate of #3, see there for the verified analysis and fix candidate.*

adv_restart_pending is incremented unconditionally in set_new_adv_interval() (line 223) but is only decremented when the fake GAPROLE_WAITING_AFTER_TIMEOUT notification actually arrives, and only reset on GAPROLE_CONNECTED. If the bounce's GAP_EndDiscoverable fails (return value ignored, no DONE event ever posted) or END_DISCOVERABLE_DONE completes with status != SUCCESS (thb2_peripheral.c:1147-1152 then notifies GAPROLE_ERROR, which the app callback merely logs), the counter is stranded at 1 with no connection to clear it. The next REAL supervision timeout is then consumed by the `if (adv_restart_pending) { adv_restart_pending--; break; }` guard, skipping the entire disconnect cleanup including hal_pwrmgr_unlock(MOD_USR0) at line 1195.

**Failure scenario:** A bounce's end-discoverable operation fails once (LL busy/error, or the connect race in the adjacent finding when no LINK_ESTABLISHED follows); adv_restart_pending stays 1. Weeks later a phone connects and walks out of range: the supervision-timeout notification is misclassified as a bounce, MOD_USR0 stays locked, and the device advertises normally while never sleeping again (~1-2 mA, 2xAAA dead in weeks) — the exact V15-V19 leak the V20/V21 work was meant to fix — until the next clean connect/disconnect cycle or a battery pull. There is no watchdog or lock-age fallback to recover it (main.c:588 excludes IBSTH2P from watchdog_config).

### 15. [MEDIUM] `bthome_phy6222/source/thb2_peripheral.c:815` — One failed GAP_MakeDiscoverable permanently kills advertising (GAPROLE_ERROR dead-end)

*Status: unverified · Reachability: shipped build*

START_ADVERTISING_EVT (thb2_peripheral.c:811-822) sets gapRole_state = GAPROLE_ERROR if GAP_MakeDiscoverable() returns non-SUCCESS, and the app callback's GAPROLE_ERROR case (thb2_main.c:1216-1218) only logs. Nothing ever re-posts START_ADVERTISING_EVT after ERROR: adv_measure() stops running (no more ADV_BROADCAST_EVT), gatrole_advert_enable(TRUE) is only reachable from GAPROLE_STARTED, and the 32 s watchdog (main.c:589) never fires because the OSAL loop still runs. GAP_MakeDiscoverable does an osal_mem_alloc(sizeof(gapAdvertState_t)) (SDK gap_peridevmgr.c:138) and issues HCI adv-param/enable commands; any single transient failure (heap pressure right after a disconnect while ATT/L2CAP buffers are in flight, or an HCI error) is permanent. The same dead-end exists via GAP_END_DISCOVERABLE_DONE with failure status (thb2_peripheral.c:1147-1150). MakeDiscoverable runs at least twice per connection cycle (post-disconnect restart + the reload-count interval bounce) and once per boot-window expiry, so the failure point is exercised constantly over fleet lifetime.

**Failure scenario:** Client disconnects; GAP_LINK_TERMINATED posts START_ADVERTISING_EVT; osal_mem_alloc for the advert state block fails once under transient heap pressure; gapRole_state = GAPROLE_ERROR; device vanishes from the air (no BTHome data, no connectability) until a battery pull, while sensors and the rest of the firmware keep running normally.

### 16. [MEDIUM] `bthome_phy6222/source/thb2_peripheral.c:1143` — END_DISCOVERABLE_DONE racing an incoming connection runs disconnect cleanup mid-connection

*Status: unverified · Reachability: shipped build*

set_new_adv_interval() (thb2_main.c:220) issues GAP_EndDiscoverable and fakes gapRole_state=GAPROLE_WAITING_AFTER_TIMEOUT. If a CONNECT_IND lands in the same advertising event (exactly the V18 fast-window use case: user connects as the 60 s window expires and the interval-restore bounce fires), GAP_LINK_ESTABLISHED_EVENT can be processed before the END_DISCOVERABLE_DONE_EVENT. The connected callback clears adv_restart_pending (thb2_main.c:1143) and locks MOD_USR0; then the late END_DONE handler sees state==GAPROLE_CONNECTED, falls into the else branch at thb2_peripheral.c:1143 setting gapRole_state=GAPROLE_WAITING and notifies the app, whose GAPROLE_WAITING case (thb2_main.c:1190-1213) stops TIMER_BATT_EVT, unlocks MOD_USR0, and honors wrk.reboot — all during a live connection. The V21 adv_restart_pending counter cannot guard this because the notification arrives as GAPROLE_WAITING, not GAPROLE_WAITING_AFTER_TIMEOUT.

**Failure scenario:** Client starts connecting during the last fast-window advertising event; connection establishes while the EndDiscoverable bounce is in flight. Result: sensor/battery notifications stop for the whole connection (TIMER_BATT_EVT killed), MOD_USR0 sleep lock released so the chip sleeps between connection events (the lock exists because connections were unstable without it), and gapRole_state is left GAPROLE_WAITING while connected. If wrk.reboot was set (OTA flow) the device resets mid-connection. Recovers on the next disconnect/reconnect cycle.

### 17. [MEDIUM] `fleet_flash_stock.py:129` — Stock flasher targets any 'PPlusOTA' advertiser, not the device it mode-switched

*Status: unverified · Reachability: shipped build*

After sending mode-switch 0x0102 to the given stock address, mode_switch() scans 30 s for the first advertiser whose name starts with 'PPlusOTA' and uploads the STAGE3 bundle to it. The documented address relationship (stock-address+1) is never checked, and the write exception before the reboot is swallowed unconditionally (line 117-118), so the scan can latch a different unit: one left in PPlusOTA mode by an earlier aborted run, or a second unit being flashed concurrently in the same rooms. The intended device may not even have rebooted. The wrong unit gets reflashed and the subsequent stock->new mapping line pairs the CLI stock address with an unrelated new 38:1F:8D address.

**Failure scenario:** Run 1 against unit A aborts after the mode switch (A stays advertising as PPlusOTA). Operator starts run 2 against unit B: B reboots, but find_device() returns A's PPlusOTA advertisement first. The bundle is uploaded to A, watch_new_custom() sees A's new custom identity, and fleet_flash_mapping.jsonl records stock-B -> new-A. B is left sitting in OTA mode unflashed while the tool prints SUCCESS for it.

### 18. [MEDIUM] `fleet_flash_stock.py:243` — 10 s pre-flash snapshot can miss existing custom units, corrupting the identity mapping

*Status: unverified · Reachability: shipped build*

watch_new_custom() attributes the first 38:1F:8D:* BTHome advertiser not in `known` to the just-flashed device. `known` is built from a single 10 s passive scan (line 242-243) — exactly one advertising interval of the V19+/V22 fleet (10 s cadence). BLE advertisement reception is lossy, so with ~11 other custom units already deployed in adjacent rooms, missing at least one unit's single advert in that window is realistic. A missed unit advertising during the watch phase (its next beacon is <=10 s away, while the flashed device still has to run the SRAM installer and reboot) is then recorded as the flashed device's new identity.

**Failure scenario:** Unit X (already V22, room 219) is not captured during the 10 s snapshot due to a lost advert. fleet_flash_stock.py flashes stock unit S; during watch_new_custom() X beacons first, passes the 38:1F:8D + BTHome filter, its revision reads IBS-V22, and the tool logs SUCCESS and appends {stock_addr: S, new_addr: X} to fleet_flash_mapping.jsonl. Downstream identity carry-over renames X's sensor to S's location; S's real new address is never recorded.

### 19. [MEDIUM] `bthome_phy6222/source/ble_ota.c:156` — CMD_OTA_SET bounds check uses FLASH_MAX_SIZE (2MB) not the 512KB part

*Status: unverified · Reachability: **not in shipped build***

The CMD_OTA_SET handler validates program_offset with `ota.program_offset + (ota.pkt_total<<4) <= FADDR_START_ADDR + FLASH_MAX_SIZE` (FLASH_MAX_SIZE=0x200000, 2MB) while the device flash is 512KB (FLASH_SIZE=0x80000). hal_flash_read masks addresses with &0x7ffff, so writes/erases beyond 512KB alias back into low flash. With a client-chosen program_offset (only lower-bounded at FADDR_APP_SEC) and pkt_total, the forward erase/write loop (lines 228-235) can be steered onto the running low-flash boot image.

**Failure scenario:** A client (not the fixed InkbirdOTA web page) sends CMD_OTA_SET with a program_offset/pkt_total that passes the 2MB check but whose aliased addresses map onto the resident boot image; the subsequent hal_flash_erase_sector/hal_flash_write erase code executing from XIP flash, bricking the unit with no BLE recovery. The shipped flasher always sends the correct 0x10000 offset, so normal field flashing does not hit it.

### 20. [MEDIUM] `bthome_phy6222/source/ble_ota.c:277` — start_app() reads uninitialized info_seg.waddr when image start_addr is 0xFFFFFFFF

*Status: unverified · Reachability: **not in shipped build***

The first loop in start_app() (lines 273-278) runs `while(i--) if(info_app.start_addr == 0xffffffff) info_app.start_addr = info_seg.waddr;` BEFORE any spif_read of info_seg — info_seg is uninitialized stack garbage at that point. The image-header format explicitly supports start_addr = -1 ('taken from the first segment != -1' per the struct comment, and the second loop at line 285 contains the correct late assignment, which the garbage from the first loop pre-empts). On IBSTH2P this code runs on EVERY boot (main.c:557 always-boot path) whenever a START_UP_FLAG image is staged at 0x10000. The project's own phy62x2_ota.py always writes an explicit start address, so fleet images do not hit it, but any pvvx-format image using the documented -1 convention does. (Inherited unchanged from pvvx/THB2 upstream.)

**Failure scenario:** An OTA image with header start_addr = 0xFFFFFFFF is staged and the device reboots: start_addr becomes stack garbage; the != 0xFFFFFFFF gate then passes, RAM segments are memcpy'd over live SRAM, and if the garbage (often an old stack pointer in 0x1fffxxxx, exactly the 0x1fff0000-0x20020000 jump window main.c checks) falls in range, the boot jumps to a garbage address. Either way the crash recurs on every reset because the staged flash image is never cleared -> permanent boot loop until UART reflash.

### 21. [MEDIUM] `bthome_phy6222/source/cmd_parser.c:961` — fix_mac() erases info sector before rewrite; power cut corrupts chip MAC/trim

*Status: unverified · Reachability: **not in shipped build***

write_fix_mac() (reached via CMD_ID_FIX_MAC in cmd_parser at line 1283) reads the RINFO sector into RAM, then at line 961 calls hal_flash_erase_sector(FLASH_ADDR_RINFO + (phy_flash.Capacity|FLASH_MAX_SIZE)) and only afterwards (lines 962-963) rewrites the 4KB via a 256-byte loop. Between the erase and completion of the write loop the manufacturer/security info page (holding the fixed chip MAC and, on this part, RF calibration/trim accessed through the high remap) is blank. It also temporarily forces phy_flash.Capacity |= 0x200000 and pokes 0x1fff0898, so a fault before the restore at line 964 also leaves the flash-size shadow wrong. No verify-before-erase or journaling.

**Failure scenario:** Operator issues CMD_ID_FIX_MAC over BLE (write=1) and the coin cell is pulled or browns out during the erase+write window. The info sector is left erased (0xFF); on next boot the chip cannot recover its MAC/trim, RF init misbehaves, and the device is not fixable over the BLE/web path — only a UART fullflash restore can recover it.

### 22. [LOW] `bthome_phy6222/SDK/components/driver/uart/uart.c:129` — RX FIFO overrun during flash operations is silent: frames lost while IRQs masked

*Status: unverified · Reachability: shipped build*

irq_rx_handler reads RFL and drains the 16-byte RX FIFO but never checks or clears the LSR overrun flag, so overflow loses bytes silently. Flash writes/erases run under spif_lock (SDK flash.c:76-85), which masks ALL interrupts except LL/TIM1 — including UART0 — for the full operation. A sector erase takes tens of ms; at 9600 baud (~1 byte/ms) a 13-byte frame arriving during save_config (any BLE CMD_ID_CFG write, cmd_parser.c:1041-1042) or during OTA block writes overflows the FIFO or is truncated. The V19 CRC framing recovers by resync, and the V21 sync engine records the lost frame as a window miss.

**Failure scenario:** User writes a config over BLE (or runs a pvvx OTA) at the moment a listen window is open: the erase masks the UART IRQ long enough to overflow the 16-byte FIFO, the frame is corrupted/lost, ucap_sync counts a miss, widens the guard and (on a second miss) burns a full-period ~13 s reacquire window at 1-2 mA. Recoverable and rare, but the driver gives the app no overrun indication at all — the app's noise/crc_bad counters are the only trace.

### 23. [LOW] `bthome_phy6222/source/battery.c:66` — ADC completion ISR calls non-IRQ-safe clock-gate RMWs, racing task-context UART re-init

*Status: unverified · Reachability: shipped build*

hal_ADC_IRQHandler runs in IRQ context and calls hal_clk_reset(MOD_ADCC) and hal_clk_gate_disable(MOD_ADCC) (battery.c:66-67), which are plain read-modify-writes on the shared AP_PCR->SW_CLK / SW_RESET0 registers with no critical section (SDK clock.c:15-45, 71-84). Task-context code RMWs the same registers: uart_hw_init/deinit do hal_clk_gate_enable/hal_clk_reset(MOD_UART0) (uart.c:230-232, 272-273) on every listen-window open. batt_start_measure (60 s cadence, adv_measure thb2_main.c:312) and window opens are both driven from the same adv cycle, so an ADC conversion (~ms) can complete exactly while ucap_start_grab is re-initializing the UART. The ISR also RMWs AP_PCRM->CLKHF_CTL1/ANA_CTL (battery.c:57-64), shared with the RF driver's task-context subWriteReg calls.

**Failure scenario:** batt_start_measure fires on the same advertising event that opens a grab window; the ADC IRQ preempts uart_hw_init between the load and store of its SW_CLK |= BIT(MOD_UART0) -> the UART0 clock-enable bit is lost -> the peripheral is dead for that window, no frame received, the sync engine records a miss and widens/reacquires (extra ~13 s awake in the reacquire window). Self-heals at the next window's re-init; rare (few-cycle race window per 60 s), but a standing violation of the clock driver's task-context-only contract.

### 24. [LOW] `bthome_phy6222/source/cmd_parser.c:297` — ucap_start_grab still opens untracked UART lock windows outside the V21 sync engine

*Status: unverified · Reachability: shipped build*

Under UCAP_SYNC the listen windows are supposed to be owned by ucap_sync_open/close_evt (config.c:169-173 even claims 'Grab windows are no longer opened from this cycle'), but ucap_start_grab() is still invoked from three legacy paths that lock MOD_UART0 and set grab_active WITHOUT arming SBP_UCAP_CLOSE_EVT: (1) adv_measure() -> start_measure() (thb2_main.c:338, sensors.c:154) on the first advertising event after every advertising restart, because GAPROLE_ADVERTISING resets meas_count=0 and cfg.measure_interval is pinned to 1 so `meas_count == measure_interval-1` matches; (2) GAPROLE_CONNECTED (thb2_main.c:1153); (3) TIMER_BATT_EVT every 10 s during a connection (thb2_main.c:860). Such a rogue window is only closed by the next good frame ISR or by the sync engine's next OPEN->CLOSE cycle, and the hal_uart_deinit()+init() it performs can destroy a frame that is mid-reception while the chip is awake in a connection.

**Failure scenario:** After every disconnect or interval bounce, the first advertising event locks MOD_UART0 and the chip stays fully awake up to a full frame period (~10.4 s) waiting for the main MCU. If the main MCU is silent (crashed/browned-out sensor board) and the sync engine is in its 5-minute backoff, a connect/disconnect leaves the rogue window locked for up to ~5 minutes at ~1-2 mA before the next backoff OPEN arms a CLOSE and releases it. During long connections the 10 s reinit cadence beats against the ~10.4 s frame cadence and periodically deinits the UART mid-frame, producing spurious CRC misses that widen the sync guard. All occurrences self-heal, so the cost is bounded extra awake time and scheduling noise, not a permanent leak.

### 25. [LOW] `bthome_phy6222/source/cmd_parser.c:310` — Grab windows opened outside the V21 sync engine hold MOD_UART0 with no close timeout

*Status: unverified · Reachability: shipped build*

ucap_start_grab() is still called from three non-sync paths in the shipped UCAP_SYNC build: GAPROLE_CONNECTED (thb2_main.c:1153), the connected TIMER_BATT_EVT 10 s cycle (thb2_main.c:860 via start_measure), and adv_measure's meas_count==(measure_interval-1) branch (thb2_main.c:338, hit once after every GAPROLE_ADVERTISING reset because the callback zeroes meas_count and measure_interval is pinned to 1). Each of these sets grab_active=1 and locks MOD_UART0 but arms no SBP_UCAP_CLOSE_EVT — the sync engine's close handler is armed only by ucap_sync_open_evt. The lock is normally released by the next good frame (<=10.4 s), but if the main MCU stream is silent it stays held until the sync engine's own OPEN->CLOSE cycle happens to sweep it, which in the 6-miss backoff state is up to 300 s + a full-period window later. These paths also hal_uart_deinit/init (lines 310-311) at times uncorrelated with the frame phase, killing any frame that is mid-reception (13.5 ms of every ~10.4 s), which the sync engine counts as a miss and answers by widening its guard.

**Failure scenario:** Main MCU silent (brownout, bench unit with inter-chip UART disconnected): user connects then disconnects over BLE. The TIMER_BATT_EVT grab opened just before disconnect leaves MOD_UART0 locked; the chip stays fully awake at ~1-2 mA for up to ~5 minutes until the backed-off sync window closes it. With a healthy main MCU: during any connection the 10.0 s TIMER_BATT grab beats against the 10.4 s frame period and roughly every 4-5 minutes the deinit lands inside a frame, dropping it and spuriously widening the listen guard.

### 26. [LOW] `bthome_phy6222/source/cmd_parser.c:322` — sensor_valid latches forever: dead main MCU keeps advertising stale temp/humi as fresh

*Status: unverified · Reachability: shipped build*

ucap.sensor_valid is set on the first good frame and never cleared or aged. Under V21/V22, cfg.measure_interval=1 makes read_sensors() -> ucap_update_measured_data() copy last_temp/last_humi and increment measured_data.count on every 10 s advertising event, regardless of whether a new frame has arrived. There is no staleness check against good_frames or last_tik even though the sync engine already tracks miss_streak and knows when the stream is dead (6+ misses, 5-minute backoff).

**Failure scenario:** Main MCU stops sending frames (its side brownouts first at low battery, or the inter-chip UART fails) while the PHY6222 keeps running: the BTHome advertisement continues with the last captured temperature/humidity and an advancing packet id indefinitely. Home Assistant shows a live, updating sensor with frozen-but-plausible values — e.g. a freezer alarm never fires because the last good reading was in range. Contrast: pre-first-frame the code deliberately reports negative debug values, so the failure mode is inconsistent too.

### 27. [LOW] `bthome_phy6222/source/cmd_parser.c:376` — ucap_sync_close_evt check-then-act race with frame completion in UART IRQ

*Status: unverified · Reachability: shipped build*

ucap_sync_close_evt() tests ucap.grab_active (line 376) and then clears it, unlocks, and sets ucap.have_prev=0 (lines 378-381). A frame that completes in the UART IRQ between the test and the stores (most likely precisely when the frame arrives at the very edge of the window, i.e. when the phase estimate has drifted) has already cleared grab_active, unlocked, computed frame_dt_ms, set have_prev=1 and queued SBP_UCAP_FRAME_EVT. The task then re-clears the flag, re-unlocks, destroys have_prev, and books a miss (guard doubled, miss_streak++) for a frame that was actually caught; the queued FRAME_EVT afterwards reschedules the window off the real frame and resets miss_streak.

**Failure scenario:** Frame completes in the ~instruction-level window inside the close handler: the scheduler records a phantom miss — the listen guard doubles (e.g. 60 ms -> 120 ms, longer awake time per window until 8 consecutive hits shrink it back) and the next inter-frame delta is discarded as UCAP_SYNC_DT_UNKNOWN, losing one EMA training sample. Self-recovering; no lock is leaked because the double unlock is idempotent.

### 28. [LOW] `bthome_phy6222/source/cmd_parser.c:1310` — Debug stats dump reads ISR-written multi-byte counters non-atomically

*Status: unverified · Reachability: shipped build*

CMD_ID_I2C_SCAN op 0/1 serialize ucap.total_bytes (4 separate volatile byte reads, lines 1310-1313), good_frames, last_temp and last_humi (two reads each, lines 1314-1321 and 1326-1329) while the UART RX IRQ concurrently increments/rewrites them — these commands only run over a live GATT connection, during which the UART stays powered and frames arrive every ~10.4 s. An IRQ between the component reads yields a torn value, e.g. total_bytes 0x0000FFFF->0x00010000 read as 0x000100FF, or last_temp mixing the low byte of frame N with the high byte of frame N+1.

**Failure scenario:** Operator runs the stats/debug dump while a frame happens to arrive between the byte reads: the reported byte counter jumps by ~256 or the debug temperature is wildly wrong for that one query (e.g. 0x18FF instead of 0x1900 region), potentially sending someone chasing a nonexistent UART corruption. Advertised sensor data is unaffected (ucap_update_measured_data uses single atomic 16-bit loads).

### 29. [LOW] `bthome_phy6222/source/config.c:80` — get_utc_time_sec uses raw single RTCCNT read and off-by-one wrap arithmetic

*Status: unverified · Reachability: shipped build*

The compiled variant of get_utc_time_sec reads AP_AON->RTCCNT once (config.c:80, TEST_RTC_DELTA off), although this RTC register needs the double-read pattern — the V21 code itself documents and uses it in ucap_rtc (cmd_parser.c:92-98, 'same double-read pattern as the reference'). A metastable read near a multi-bit carry returns a bogus value; the subsequent delta is clamped only to 64 s (delta &= 0x1fffff, config.c:87), so one bad read can inject up to ~64 s into clkt.utc_time_sec and desynchronize clkt.utc_time_tik for a cycle. Additionally the wrap branch 'delta = 0xffffffff - clkt.utc_time_tik + new_time_tik' (config.c:85) loses one tick per call (24-bit wrap should be 0x1000000-based), a small systematic skew at the 10 s call cadence.

**Failure scenario:** An RTC read glitch during an advertising event adds up to 64 s to UTC time in one step -> the battery-measure tick comparison (thb2_main.c:310, batt_interval<<15) fires early/late by up to a minute and the SLEEP_R-persisted clock jumps; the systematic 1-tick loss slowly skews UTC. No user-visible damage in the shipped BTHome-only build (no history timestamps), but this clock is the time base for all periodic maintenance.

### 30. [LOW] `bthome_phy6222/source/config.c:82` — get_utc_time_sec wrap branch inverted: normal path computes delta-1 tick

*Status: unverified · Reachability: shipped build*

The code takes `delta = new - old` when new <= old (the wrap case) and `delta = 0xffffffff - old + new` when new > old (the normal case). Modulo-2^32 arithmetic makes both branches nearly equivalent, but the normal-path expression equals (new - old - 1) mod 2^32, so every call undercounts elapsed time by exactly one 32768 Hz tick (30.5 us). The subsequent `delta &= 0x1fffff` hides the mistake for the wrap case (2^24 is a multiple of 2^21) but not the systematic -1.

**Failure scenario:** get_utc_time_sec() is called once per advertising event (every 10 s steady state) and every 10 s while connected: the settable UTC clock (CMD_ID_UTC_TIME) permanently loses ~30.5 us per call, ~0.26 s/day of pure software drift on top of crystal tolerance. A client that sets the time and reads it back weeks later sees the clock consistently slow by several seconds beyond hardware error.

### 31. [LOW] `bthome_phy6222/source/config.c:139` — test_config() never clamps batt_interval: 0 makes battery ADC run every adv event

*Status: unverified · Reachability: shipped build*

test_config() clamps rf_tx_power, advertising_interval, measure_interval and adv_event_cnt but never batt_interval. CMD_ID_CFG (cmd_parser.c:1030-1043) memcpys client-supplied bytes into cfg, runs test_config(), and persists via save_config(). With cfg.batt_interval = 0 the threshold `(uint32_t)cfg.batt_interval << 15` is 0, so `clkt.utc_time_tik - adv_wrk.measure_batt_tik >= 0` at thb2_main.c:310 (and :852 while connected) is always true.

**Failure scenario:** A BLE client writes a config blob with batt_interval = 0 (accidentally or via a truncated/garbage CMD_ID_CFG write). From then on -- persisted across reboots in flash -- batt_start_measure() runs on every 10 s advertising event (and every 10 s TIMER_BATT_EVT while connected) instead of every 60 s: 6x the ADC duty (pwrmgr lock, ADC LDO power-up, IRQ) forever, with no clamp restoring sanity until the config is rewritten or defaulted.

### 32. [LOW] `bthome_phy6222/source/sensors.c:154` — Legacy grab call sites bypass the V21 sync engine, opening windows with no close timer

*Status: unverified · Reachability: shipped build*

V21 makes ucap_sync_open_evt() the owner of listen windows (it arms SBP_UCAP_CLOSE_EVT for every window it opens), but three pre-V21 call sites still call ucap_start_grab() directly without arming any close: start_measure() (reached from adv_measure()'s meas_count==0 branch on the first advertising event after every advertising restart, because peripheralStateNotificationCB GAPROLE_ADVERTISING resets meas_count and cfg.measure_interval is pinned to 1), the TIMER_BATT_EVT handler during connections (thb2_main.c:861), and GAPROLE_CONNECTED (thb2_main.c:1153). A window opened this way holds the MOD_UART0 sleep lock until the next good frame arrives (~<=10.4 s) or until the sync engine's next OPEN timer fires and arms a CLOSE — in the 5-minute backoff state (silent main MCU) that is up to ~300 s fully awake at ~1-2 mA.

**Failure scenario:** Client disconnects -> state passes through GAPROLE_ADVERTISING -> meas_count=0 -> next 10 s advertising event calls start_measure() -> ucap_start_grab() opens an unmanaged window mid-cycle. Device stays awake up to a full frame period (~10 s at ~1-2 mA) per disconnect instead of sleeping; if the main MCU UART is dead and the scheduler is in 300 s backoff, the lock is held up to ~5 min per occurrence. Recoverable automatically, but contradicts the 'sync engine owns window close' design and adds unbudgeted awake time.

### 33. [LOW] `bthome_phy6222/source/thb2_main.c:310` — Battery-interval delta lacks 24-bit RTC mask: early fire at every 512 s wrap

*Status: unverified · Reachability: shipped build*

clkt.utc_time_tik is a raw 24-bit RTCCNT value (updated in get_utc_time_sec), but `clkt.utc_time_tik - adv_wrk.measure_batt_tik >= ((uint32_t)cfg.batt_interval << 15)` does 32-bit subtraction without masking to 24 bits (same pattern at thb2_main.c:852). When the counter wraps between the snapshot and the check, the delta reads ~4.28e9 instead of the true elapsed ticks, so the comparison fires regardless of the configured interval. The correct thresholds (max 255<<15 = 8.36M ticks) all fit in 24 bits, so `& 0xFFFFFF` on the delta would fix it (as the ucap_rtc/frame-delta code in cmd_parser.c already does).

**Failure scenario:** Once every 512 s RTC wrap, one battery-measurement interval is cut short: batt_start_measure() runs early (e.g. after 10-50 s instead of the configured 60 s), permanently raising the ADC measurement rate by roughly batt_interval/512. Behavior is only ever early-fire (never a missed measurement), so the impact is a small constant battery-cost increase rather than a functional failure.

### 34. [LOW] `bthome_phy6222/source/thb2_main.c:338` — UCAP_SYNC still opens un-timed UART grab windows from adv_measure

*Status: unverified · Reachability: shipped build*

With UCAP_SYNC, test_config() pins cfg.measure_interval = 1 and the config.c:171 comment claims 'Grab windows are no longer opened from this cycle'. But adv_measure()'s check `meas_count == (uint8_t)(cfg.measure_interval - 1)` compares against 0, and peripheralStateNotificationCB resets meas_count = 0 on every GAPROLE_ADVERTISING transition (thb2_main.c:1132). So the first ADV_BROADCAST_EVT after every advertising (re)start calls start_measure() -> ucap_start_grab(), which deinits/reinits UART0 and takes the MOD_UART0 sleep lock with no SBP_UCAP_CLOSE_EVT timeout armed (unlike sync-engine windows). The lock is only released by the next good frame (ISR) or by the next sync-engine window close.

**Failure scenario:** After every advertising restart -- fast-window expiry ~60 s after boot, and twice per connect/disconnect cycle (set_new_adv_interval restore + reconnect restore) -- an unscheduled listen window opens mid-period and holds the chip awake at ~1-2 mA for up to one ~10.4 s frame period before the next frame releases it, defeating the V21 duty-cycle design for that interval. Worst case: if the main MCU is silent (sync engine in 300 s backoff), the rogue lock is held until the next backoff open/close cycle -- up to ~5 minutes fully awake per advertising restart.

### 35. [LOW] `bthome_phy6222/source/thb2_main.c:521` — Placeholder GPIO_KEY P07 registered for edge interrupts and armed as sleep wake source

*Status: unverified · Reachability: shipped build*

config.h:219 defines GPIO_KEY as GPIO_P07 with the comment 'Not really. Just a placeholder.', but SERVICE_KEY is in the shipped DEV_SERVICES, so init_app_gpio (thb2_main.c:521) registers real edge interrupts on it and the SDK sleep handler (gpio.c hal_gpio_sleep_handler) arms every GPIO_PIN_ASSI_IN pin — including P07 — as a wake source with polarity opposite its current level on every sleep. No pull is explicitly configured (hal_gpioin_enable leaves the reset default). Any edge on this pad wakes the chip and posts KEY_CHANGE_EVT; the key-up branch (thb2_main.c:1038) calls increase_advertising_frequency(), switching to ~1.56 s advertising for 60 s.

**Failure scenario:** On a fleet unit where the unknown P07 pad picks up noise (coupling from the main MCU's lines, humidity/condensation in a fridge deployment, ESD), each glitch pair costs a wakeup plus 60 s of ~6x-rate advertising; sustained noise keeps the device permanently in the fast-advertising state, multiplying radio battery drain. Recoverable (no state corruption), and bench units show it quiet, but it is live latent behavior on all 12+ deployed units.

### 36. [LOW] `bthome_phy6222/source/thb2_main.c:521` — SERVICE_KEY arms placeholder pin P07 as IRQ+wake source; glitch triggers 60 s fast adv

*Status: unverified · Reachability: shipped build*

The shipped BOOT build keeps SERVICE_KEY (config.h:182) with GPIO_KEY = GPIO_P07 explicitly marked 'Not really. Just a placeholder.' (config.h:219). init_app_gpio (thb2_main.c:521) calls hal_gpioin_register(GPIO_P07, ...), making P07 an edge-interrupt input, and the SDK's hal_gpio_sleep_handler (gpio.c:524-559) then automatically configures every GPIO_PIN_ASSI_IN pin — including P07 — as an AON wake source with polarity opposite its current level. P07 is only weak-pulled-down (main.c:396). With KEY_PRESSED=0, the idle level 0 reads as 'pressed'; the KEY_CHANGE_EVT key-up path (thb2_main.c:1038, no SERVICE_SCREEN) calls increase_advertising_frequency() -> 60 s reload count and set_new_adv_interval(DEF_CON_ADV_INTERVAL).

**Failure scenario:** EMI/ESD or board-level coupling produces a positive excursion on the unconnected P07 pad that persists to task time: the chip wakes from sleep, fires KEY_CHANGE_EVT, reads 1 -> 'key released' -> 60 seconds of ~1.5 s-interval advertising per glitch (plus a full advertising teardown/restart). Repeated noise becomes a chronic battery tax; every single edge is at minimum a spurious sleep-wake cycle. All of this serves a button that does not exist on this pin.

### 37. [LOW] `bthome_phy6222/source/thb2_main.c:678` — OTA-boot fast window: reload count computed for 1 s interval but 1.56 s interval set

*Status: unverified · Reachability: shipped build*

For BOOT_FLG_OTA boots, adv_wrk.adv_reload_count = 60000/DEF_OTA_ADV_INERVAL_MS (= 60 events, comment '60 sec') but the very next line sets the interval to DEF_CON_ADV_INTERVAL (2500 x 625 us = 1.56 s), not DEF_OTA_ADV_INERVAL (1 s). The window before the adv_measure() expiry path executes the BOOT_FLG_OTA hal_system_soft_reset (thb2_main.c:371-375) is therefore 60 x 1.56 s ~= 94 s, matching neither the code comment (60 s) nor config.h:712's '80 sec'.

**Failure scenario:** A device rebooted into OTA mode whose OTA session never starts sits connectable at 1.56 s advertising for ~94 s instead of the designed 60 s before auto-resetting back to the app — an extra ~34 s of fast advertising per aborted OTA attempt, and tooling calibrated to the documented window misjudges when the device returns. Benign in effect but a concrete arithmetic mismatch in the audited reload logic.

### 38. [LOW] `bthome_phy6222/source/thb2_main.c:1153` — Connected-grab opens UART window with no close timer; up to ~5 min awake post-disconnect

*Status: unverified · Reachability: shipped build*

GAPROLE_CONNECTED calls ucap_start_grab() directly (and again via start_measure() on every 10 s TIMER_BATT_EVT while connected). Unlike windows opened by ucap_sync_open_evt(), no SBP_UCAP_CLOSE_EVT is armed for these; ucap_update_measured_data() deliberately does not close windows under UCAP_SYNC (cmd_parser.c:330-341), and the disconnect cleanup (thb2_main.c:1190-1214) stops TIMER_BATT_EVT but never closes an open grab. The MOD_UART0 pwrmgr lock is then released only by the next good frame (ISR, cmd_parser.c:213-216) or by the sync chain's next OPEN→CLOSE cycle — which, in the 6-miss backoff state (UCAP_SYNC_BACKOFF_MS, ucap_sync.h:64), can be up to 300 s away.

**Failure scenario:** Main MCU has stopped streaming (fault, brown-out, disconnected flex) so the sync engine is in 5-minute backoff; an operator connects over BLE to diagnose the unit and disconnects: GAPROLE_CONNECTED opened a grab that nothing closes, so after MOD_USR0 is released the chip still cannot sleep (MOD_UART0 held, ~1-2 mA) for up to ~5 minutes until the backoff OPEN→CLOSE cycle finally unlocks it. Recoverable but contradicts the post-disconnect sleep guarantee the V20/V21 fixes were validated for.

### 39. [LOW] `bthome_phy6222/source/thb2_main.c:1197` — Disconnect cleanup rebuilds advert buffer but never pushes it to the controller

*Status: unverified · Reachability: shipped build*

The GAPROLE_WAITING / real-timeout cleanup calls bthome_data_beacon((void*)gapRole_AdvertData) without LL_SetAdvData() (contrast every other call site, e.g. thb2_main.c:281, 293, 328). GAP_MakeDiscoverable only sets adv params and enables — it never rewrites adv data — so after every disconnect the controller re-broadcasts the last payload pushed BEFORE the connection. The rebuilt buffer is dead work; the on-air payload refreshes only at the next read_sensors() advertising event, ~2-3 events (~20-30 s at the 10 s interval) later because the ADVERTISING notification resets meas_count. This also defeats the CONNECTED handler's explicit button-hold cancellation (thb2_main.c:1150-1151), whose comment claims the disconnect path gives a fresh packet id: if a button-press burst (0x3a object, frozen packet id) was on air when the client connected, that exact payload goes back on air after disconnect.

**Failure scenario:** User presses the device button (BTHome button object goes on air with frozen packet id N), then a client connects within the 60 s hold and later disconnects: the button-object payload with id N is re-broadcast for ~20-30 s. Any Home Assistant instance that did not receive the pre-connection burst (restarted, or out of range then) sees id N as new and fires a phantom button event minutes after the physical press; in all cases sensor entities are served stale pre-connection values for ~30 s after every disconnect.

### 40. [LOW] `fleet_flash_custom_any.py:46` — GATT read before try/finally leaks the BLE connection on failure

*Status: unverified · Reachability: shipped build*

In flash_one(), the initial read_gatt_char(SW_REV_CHAR) at line 46 sits outside the try/finally that guarantees client.disconnect(). If the read raises while the link is still up (ATT error, timeout with connection intact), the exception propagates to main()'s except handler which just prints FAILED and continues the round-robin — the BleakClient is never disconnected. The zombie connection keeps the target device connected (its fast window consumed), so subsequent try_connect() attempts to it fail until the script is restarted or BlueZ drops the link. Contrast fleet_flash_custom.py, which correctly wraps the same read in try/finally.

**Failure scenario:** Operator battery-pulls a device; try_connect succeeds; read_gatt_char times out at the ATT layer while the ACL link stays up. flash_one raises, main() prints 'FAILED ... battery-pull to retry' and loops, but the leaked connection holds the device, so every retry fails silently and the operator repeatedly pulls the battery to no effect.

### 41. [LOW] `bthome_phy6222/source/buzzer.c:184` — Buzzer melody loops forever holding the MOD_PWM sleep lock until an explicit stop

*Status: unverified · Reachability: **not in shipped build***

pwm_buzzer_event() wraps pwm_buzzer_note_idx back to 0 at the end of the melody and always re-arms BUZZER_TONE_EVT, so a single CMD_ID_BUZZER start (cmd_parser.c:1181-1186) plays the tune in an infinite loop. Every non-REST note holds hal_pwrmgr_lock(MOD_PWM) (line 163; released only during rests at line 158), keeping the chip awake nearly continuously. Nothing stops it on disconnect: only another CMD_ID_BUZZER 0 command or, on SERVICE_BUTTON devices, a key-release PIN_INPUT_EVT calls pwm_buzzer_stop().

**Failure scenario:** On a buzzer-equipped build (DEVICE_KEY2/HDP16/TN6ATAG3 with PWM_CHL_BUZZER), a client sends CMD_ID_BUZZER start and then disconnects or goes out of range: the device beeps forever with MOD_PWM locked for the majority of every melody cycle, draining the battery in days until someone reconnects to send stop or presses the button. Not in the shipped IBSTH2P BOOT_OTA image — DEVICE_IBSTH2P defines no GPIO_BUZZER/PWM_CHL_BUZZER, so buzzer.c and the CMD_ID_BUZZER handler compile out.

### 42. [LOW] `bthome_phy6222/source/flash_eep.c:330` — pack_cfg_fmem error codes used as flash write addresses by _flash_write_cfg

*Status: unverified · Reachability: **not in shipped build***

pack_cfg_fmem() returns unsigned int but signals errors as -(FMEM_OVR_ERR) = 0xFFFFFFFC (line 269) and can return small values via `return rdaddr` (line 266). _flash_write_cfg() checks only `faddr == 0` after the pack call (line 330); the `faddr < FMEM_ERROR_MAX` guard at line 335 is on the else-branch of the pre-pack lookup and 0xFFFFFFFC does not satisfy it either. The error value then flows into `_flash_write_dword(faddr, fobj.x)` (line 340): 0x11000000 + 0xFFFFFFFC wraps to a write at physical 0x7FFFC (last EEP bank dword), and the following data write at faddr+4 = 0 lands on physical sector 0 (flash boot info) without erase, clearing bits in the ROM boot record. In practice the overflow branch is currently dead because the missing wraddr advance (finding 1) keeps wraddr pinned at fnewseg+4 so the overflow guard never fires — but any fix of finding 1 makes this latent wild-write live on genuine pack overflow (e.g. after a torn header inflates record sizes).

**Failure scenario:** EEP bank contents corrupted (torn header with large size fields) -> pack copies overflow the new bank -> pack returns 0xFFFFFFFC -> _flash_write_cfg writes the object header at physical 0x7FFFC and the object payload over physical sector 0x00000 (ROM boot info) without erase -> corrupted boot record; device may fail to boot on next reset.

### 43. [LOW] `bthome_phy6222/source/thb2_main.c:438` — KEY event during a connection leaks adv_restart_pending, re-enabling the sleep-lock leak

*Status: unverified · Reachability: **not in shipped build***

The shipped build includes SERVICE_KEY with GPIO_KEY = P07, a self-described placeholder pin ('Not really. Just a placeholder.', config.h:219) whose edge interrupts are registered at init. A key-up while a connection is live (gapRole_AdvEnabled stays TRUE across connections) calls increase_advertising_frequency() -> set_new_adv_interval(), which increments the new V21 adv_restart_pending counter and calls GAP_EndDiscoverable while not advertising — so no END_DISCOVERABLE_DONE bounce notification ever arrives to decrement it — and also overwrites gapRole_state from GAPROLE_CONNECTED to GAPROLE_WAITING_AFTER_TIMEOUT mid-connection (pre-existing). With pending stuck >0, the next real supervision timeout takes the 'bounce' early-break in peripheralStateNotificationCB: MOD_USR0 is never unlocked (the V15-V19 ~1-2 mA leak returns), the advertisement is not rebuilt, and wrk.reboot is not honored, until a later clean connect/disconnect cycle. Note adv_restart_pending is cleared on GAPROLE_CONNECTED, but that cannot help once the leak happens inside the same connection.

**Failure scenario:** Electrical noise/coupling produces an edge on the unconnected P07 while a phone is connected. Later the phone walks out of range: supervision timeout fires, the callback sees adv_restart_pending==1, decrements and breaks — sleep lock leaked, device draws ~1-2 mA continuously (weeks to a dead 2xAAA) while appearing to advertise normally. Trigger depends on the placeholder pin actually toggling, which has not been observed on bench units.

### 44. [LOW] `bthome_phy6222/source/thb2_main.c:1195` — Disconnect frees MOD_USR0 but never closes an open UART grab (MOD_UART0)

*Status: unverified · Reachability: **not in shipped build***

GAPROLE_CONNECTED (line 1153) calls ucap_start_grab(), which locks MOD_UART0 and sets grab_active=1, with no CLOSE timer armed by the connect path — closure relies on a later frame ISR or the UCAP_SYNC OPEN/CLOSE cycle. The disconnect cleanup (GAPROLE_WAITING / _AFTER_TIMEOUT, lines 1190-1214) unlocks MOD_USR0 (line 1195) and stops TIMER_BATT_EVT but does nothing to grab_active or MOD_UART0. If no frame arrives to release it, the only release is the sync engine's next OPEN->CLOSE, which after miss backoff can be up to UCAP_SYNC_BACKOFF_MS (300s) away.

**Failure scenario:** Main MCU is silent (dead/reset), the sync engine has escalated to backoff, then a phone connects (opens a grab, locks MOD_UART0) and disconnects. After disconnect MOD_USR0 is freed but MOD_UART0 stays locked, so the chip cannot sleep (~1-2 mA) until the backoff OPEN/CLOSE fires up to ~5 min later. Bounded and self-healing, and requires an abnormal silent main MCU, so not seen in normal fleet operation.

### 45. [LOW] `bthome_phy6222/source/thb2_peripheral.c:1200` — Failed GAP_LINK_ESTABLISHED statuses strand advertising off with no re-enable

*Status: unverified · Reachability: **not in shipped build***

In gapRole_ProcessGAPMsg, GAP_LINK_ESTABLISHED_EVENT with status bleGAPConnNotAcceptable sets gapRole_AdvEnabled = FALSE and gapRole_state = GAPROLE_WAITING (thb2_peripheral.c:1200-1207) with a comment saying the device becomes discoverable again 'when this value gets set to TRUE' — but no code in this app ever sets it TRUE again (the TI-style GAPRole_SetParameter path is compiled out under #if 0, and the app's WAITING cleanup does not re-enable advertising). Any other failure status falls to gapRole_state = GAPROLE_ERROR (lines 1208-1211), the same unrecoverable dead-end as the GAPROLE_ERROR finding. Either way advertising is permanently off with no link established to recover through.

**Failure scenario:** An LL connection-complete arrives with a non-SUCCESS status (controller-level connection-setup failure): the WAITING notification runs the app's disconnect cleanup but leaves gapRole_AdvEnabled FALSE with no START_ADVERTISING_EVT pending — the device never advertises again until battery pull. Marked unreachable because no code in this SDK actually generates bleGAPConnNotAcceptable and legacy-advertising slaves essentially never receive failed connection-complete events; it is a latent trap on the audited path rather than a field-triggerable bug.
