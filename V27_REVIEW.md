# V27 firmware review (IBS-W27, commit 49aa20c)

**Date:** 2026-09-09
**Scope:** V27 P03 build (shipped fleet image, ~16 units, alkaline, in fridges/freezers since 2026-09-08); V25 P10 and legacy V21-V24 SYNC paths still in tree and still shipped in `inkbird_fw/` / `fleet_flash_custom.py`; the Python flashing/diagnostic tools; the host test suite.
**Method:** 12 independent static-review passes, 61 deduplicated findings, each checked by one strict verifier for code truth and reachability in the shipped builds; 61 confirmed, 0 refuted. Overlapping findings from different passes are merged below into single entries.

> **Verification caveat (added by the orchestrating session).** The single-pass verifier refuted none of the 61 deduplicated findings, which means it acted as a fact-checker of citations rather than as a filter; treat the low-severity list as unfiltered. The orchestrator independently re-checked the following against source at HEAD 49aa20c and they hold as written: S1 (`cmd_parser.c:511`, `:749`, `sensors.c:120-125`), S3 (`cmd_parser.c:441-447`), S4 (`sbp_profile.c:367`, `thb2_main.c:809` are the only feeds besides the ROM wake path), S9 (`fleet_flash_stock.py:47-48`), S15 (`fleet_flash_custom.py:31` defaults to v24), S22 (TX-power no-op via `version.h` QFN32 default). S8 (`ble_ota.h:13` FLASH_MAX_SIZE 0x200000 vs FLASH_SIZE 512 KB in `flash_eep.h:35`). Earlier 3-lens verification, before it was cut for cost, refuted 4 of the first 24 verdicts, so expect roughly 10-20 % of the medium/low entries to be overstated.

## Verdict

V27 is safe to leave running on the fleet for now: no confirmed defect produces an *immediate* silent unit or a drain larger than tens of µA above the ~10-14 µA design budget, and the two critical findings below either require a fault condition (main-MCU/P03 failure) that is not known to have occurred, or apply only to the V25_P10 image, which is not on any fielded unit. The single most important gap is structural, not a bug: **there is no frame-age check anywhere in the shared sensor path** (S1), so the exact failure this fleet cares most about — a freezer that dies while HA keeps showing a live reading — is currently undetectable by firmware and would only be caught if HA's own staleness/absence alarms are configured on top of a suspiciously flat trace. Fix that first; it is a small, self-contained change (age-stamp the last good frame, gate `sensor_valid`) and it is the one change that directly serves the "never silently go dark" requirement.

Second priority: the connection/watchdog cluster (S4, S7, S10) — an idle or read-only BLE client, or a rare GAP failure, can put a unit into a reset-reconnect loop or a permanently-connectable state; low probability today (no phone app routinely connects to these), but it becomes relevant the next time someone does an OTA or a bench inspection. Third: the tooling defaults (S9, S15) are a live foot-gun — reflashing any unit today without setting `IBS_OTA_IMAGE` regresses it to the legacy SYNC scheduler, which has a real, quantified drain mode (S6).

Energy budget: the V27 steady-state design point is genuinely good, ~10-14 µA average, and the V27 TX-power change contributes exactly 0 to it (confirmed no-op, S22) — any battery-slope difference from V26 comes solely from the non-connectable steady-state change, not from TX power. Nothing in this review points to a code-level explanation for a drain anomaly larger than ~50 µA; if a fielded unit shows that, look at the fault paths in the table in section 5, not steady state.

## How the V27 build works

**Build.** `PROJECT_DEF="-DDEVICE=DEVICE_IBSTH2P -DUCAP_P03=1" BOOT_OTA=1`. `config.h` forces `cfg.advertising_interval = 160` (10.0 s) and `cfg.measure_interval = 1` whenever `UCAP_P03` is set (config.c:168-176). The shipped binary carries the string "IBS-W27", which `config.h` only emits under `UCAP_P03`, confirming the fleet is on this path (not SYNC or P10) despite no in-tree v27 build directory existing to check flags directly.

**P03 receiver (UCAP_P03).** P03 (pulled down, rise+fall GPIO IRQ) and the UART RX line (P10, made an AON wake source as a side effect of `uart_hw_init`'s `fmux_set`) are both armed as wake sources every sleep. A P03 rise, or the first UART byte, takes `hal_pwrmgr_lock(MOD_UART0)` in ISR/wake-hook context and posts an OSAL event; the task-side state machine (`ucap_p03.h`: IDLE → ARMED → SUSPENDED) re-locks, arms a 250 ms timeout, and releases the lock on a CRC-good `FRAME` event or on `TIMEOUT`. A CRC-bad completion in `ARMED` keeps the lock and re-arms the full 250 ms (S51: the file's own header comment says otherwise). `adv_measure()` sweeps every advertising event and forces `RECOVER` if `ARMED` is older than 2 s (bounds a lost timer to one 10 s period). Bench: 2.2 ms edge-to-first-byte lead, 100% edge coverage.

**Advertising / connectable policy.** Steady state is reached only through one choke point, `thb2_main.c:382-447`: when `adv_reload_count` hits 0, `gapRole_AdvEventType = LL_ADV_NONCONNECTABLE_UNDIRECTED_EVT` and the interval is set to 10 s. Connectable windows open from the boot default (`adv_reload_count = 38`, ~60 s at 1.56 s interval) and from a button click seen as a byte[8] change in a UART frame (also `adv_reload_count = 38`, but the six held-open events themselves run at 1.56 s, so the *button-object* airtime is only ~9.4 s, not ~60 s — S53). A disconnect restarts advertising connectable for exactly one event, then falls through the choke point. The only way a unit stays connectable forever in the field is a `GAP_EndDiscoverable`/`END_DISCOVERABLE_DONE` failure at the choke point, which has no retry (S10).

**Watchdog.** `WDG_64S`, hardware reset (polling mode, not IRQ). Fed from `ADV_BROADCAST_EVT` (steady state, every 10 s) and from the ROM wake path's `__wdt_init()` (every sleep/wake cycle) and from GATT writes. During a connection, `MOD_USR0` blocks sleep and advertising is disabled, so both of those feeds stop; only a GATT *write* feeds it (S4).

**Battery gauge.** ADC every `cfg.batt_interval` = 60 s, `hal_pwrmgr_lock(MOD_ADCC)` released only in the ADC ISR with no timeout (S18). `measured_data.battery_mv` is a 512-sample running mean, effectively an α=1/512 EMA once full (~8.5 h time constant at 60 s cadence), reset from scratch on every reboot — voltage slopes fitted from HA are only trustworthy well after a flash/reboot settles. Mapping is linear 2000-3000 mV → 0-100%. There is no low-battery action (removed in 414ec0b); the unit browns out unmanaged.

**Telemetry / BTHome.** 27-byte steady payload (25 during the button hold): flags, service-data header, info/PID, a 13-byte `data1` block (temperature sint16×0.01, humidity uint16×0.01, battery uint8%, `0x09` count8), and a 4-byte `0xF2` firmware-version tail. The `0x09` object means different things depending on which variant a fleet unit is running: P03-edge-to-first-byte lead in ms on V27, listen-window health % on V25_P10 (S40) — both variants are live in `inkbird_fw/` today.

## Confirmed findings

### Critical

#### S1. Stale sensor values advertised indefinitely with fresh packet ids when frames stop arriving [critical] [UCAP_P03 (V26/V27), UCAP_P10 (V25), UCAP_SYNC (legacy)]

`bthome_phy6222/source/cmd_parser.c:745-750`, consumed by `sensors.c:120-126` and `bthome_beacon.c:74-77`.

There is no frame-age check anywhere. `ucap.sensor_valid` is set once (`cmd_parser.c:511`) and never cleared. Every advertising cycle (`measure_interval` pinned to 1):

```c
if (ucap.sensor_valid) { measured_data.temp = ucap.last_temp; measured_data.humi = ucap.last_humi; }
```

`sensors.c` then does `measured_data.count++` and returns, so the BTHome packet id keeps incrementing with stale data.

**Failure scenario:** main MCU keeps running but its P03/UART driver fails, or the line is held stuck, or the main MCU itself hangs (field-observed hang history is firmware-agnostic on that side). Every subsequent 10 s advertisement carries the last known temperature/humidity with a fresh packet id. A thawing freezer is never alarmed by anything in this firmware; HA sees a live, updating sensor.

**Fix:** timestamp `ucap_rtc()` at each CRC-good frame; if now minus that timestamp exceeds ~60-120 s, stop incrementing `measured_data.count`, clear `sensor_valid` (or publish a BTHome "problem" object), and surface the age via GATT telemetry.

*Verifier note:* one-line citation drift only (sensors.c:120-126, not 121-127); otherwise fully confirmed.

#### S2. Health % and sensor_valid never decay when frames stop (V25_P10) [critical] [V25_P10]

`bthome_phy6222/source/ucap_p10.h:171-178`.

`p10_health` is written only from `p10_hist_push()`, called only when a listen window closes (hit or miss); `WAIT_EDGE` (the "nothing is happening" state) never touches it. `ucap.sensor_valid` (shared code, see S1) is likewise set once and never cleared.

**Failure scenario:** a V25_P10 unit running healthily (health 100) whose main MCU hangs, or whose P10 trace opens, stops producing edges entirely — no windows, no `hist_push`. Adverts continue every 10 s with frozen temperature *and* health = 100, indefinitely. A freezer warming up is invisible for as long as the batteries last.

**Fix:** in `ucap_p10_sanity()` (already called every 10 s), age the data — if `WAIT_EDGE` and elapsed time exceeds ~3× the estimated period, push a synthetic miss / decay health, and clear `sensor_valid` after a bounded number of missed periods.

*Verifier note:* not live on the current 16-unit fleet (all on V27/UCAP_P03) — matters only if a unit is (re)flashed with the still-shipped `v25_p10` OTA image. The underlying stale-data mechanism (S1) is shared code and already covers V27's temp/humidity exposure; this finding is specifically about the P10 health counter, which V27 does not have.

### High

#### S3. A frame received without a P03 edge (or with P03 held high) costs a full 250 ms awake for zero data [high] [V27_P03]

`bthome_phy6222/source/cmd_parser.c:441-446`, `ucap_p03.h:189-197`.

When the first thing the chip sees on wake is the UART start bit rather than a P03 edge, the wake hook still locks and arms the full `P03_TIMEOUT_MS = 250`. If the wake-triggering byte is lost to `uart_hw_init`'s FIFO reset (see S33), the frame can no longer validate, and the timeout is always spent in full.

**Failure scenario (systematic):** a P03 wiring/level fault, or a main-MCU firmware variant that stops pulsing P03: every ~10.4 s frame costs 250 ms at ~1.5-2 mA instead of ~17 ms, i.e. 40-50 µA average — 2-3× the entire intended sleep budget — with zero sensor data recovered (compounds with S1).

**Fix:** use a short timeout (~40-60 ms) when the arm came from an unanchored RX-start in IDLE; keep 250 ms only for the edge-anchored case.

*Verifier note:* the "stuck high" half of the original title is imprecise — a P03-high wake is classified `P03_SRC_WAKE`, a valid rise anchor, not `SRC_UNKNOWN`; the 250 ms cost still holds either way via the same unconditional lock/timeout action, and the premise that the waking byte is necessarily lost is a plausible but code-unconfirmed inference about SDK wake latency.

#### S4. Connection watchdog starvation: an idle or write-free BLE connection is reset by the 64 s watchdog, reopening a connectable window [high] [V27_P03, V25_P10, legacy_SYNC]

`bthome_phy6222/source/thb2_main.c:804-811, 1193-1219`; `sbp_profile.c:364-368`; `main.c:590-598`.

While connected, `MOD_USR0` blocks sleep (no wake-path `__wdt_init` feed) and advertising is disabled (`gapRole_AdvEnabled = FALSE`, so no `ADV_BROADCAST_EVT` feed). The *only* remaining feed is a write to the 0xFCD2 GATT service (`sbp_profile.c:364-368`). No application timer bounds connection duration.

```c
// sbp_profile.c
#if (DEVICE == DEVICE_IBSTH2P)
hal_watchdog_feed();
#endif
```
is inside the write callback only; reads, CCCD subscribes, and the 10 s connected `TIMER_BATT_EVT` loop feed nothing.

**Failure scenario:** a phone app, nRF Connect, or any client that connects, subscribes to notifications, and then only reads/idles for >64 s gets a hard reset. The unit reboots into a fresh ~60 s connectable fast window; an auto-reconnecting client repeats the cycle indefinitely at full-awake current (~1-2 mA) with a reboot every ~70 s, and telemetry/UTC/packet counters reset each time. A legitimate multi-minute inspection or OTA-pause session is silently cut off.

**Fix:** feed the watchdog from the connected `TIMER_BATT_EVT` handler (fires every 10 s while connected) in addition to GATT writes; separately, add an explicit connection-age timer (e.g. 2-5 min) that calls `GAPRole_TerminateConnection()` so idle dwell ends cleanly instead of via reset.

*Verifier note:* this entry merges four overlapping submissions (main-adv, power, x-arith, x-drain-budget, ble-gatt) rated high/medium/medium/low for the same underlying mechanism; the high rating is carried forward because the worst case (a genuinely idle client causing a repeating reset loop) is a real, unbounded drain, not merely a nuisance.

#### S5. RC32K correction is discarded exactly when |RC error| exceeds 5%, turning the documented "G3 unavailable" case into a permanent all-miss [high] [V25_P10]

`bthome_phy6222/source/ucap_p10.h:44-74`.

```c
#define P10_RC_CT_MIN 7421u  /* -5% */
#define P10_RC_CT_MAX 8203u  /* +5% */
...
if (ct < P10_RC_CT_MIN || ct > P10_RC_CT_MAX) return ms; /* nominal, uncorrected */
```

The SDK's own sleep conversion (`ll_sleep.c`, `patch.c`) keeps correcting past this band via a soft limit around a running average; P10's clock-domain conversion does not, so any RC32K excursion beyond ±5% (the header itself: "beyond that the unit stays all-miss", confirmed by `test_ucap_p10.c` T30) produces a fixed prediction bias exceeding the 400 ms guard cap.

**Failure scenario:** a freezer unit at -20°C with RC32K 5.5% slow: predicted period is always ~570 ms late relative to the guard-widened window; steady state settles into ~20 ms VERIFY + ~830 ms UART-on per 10.39 s period (~8% duty, order 100 µA average) with zero sensor data, until temperature moves the RC error back inside the band.

**Fix:** clamp instead of discard (`if (ct < MIN) ct = MIN; else if (ct > MAX) ct = MAX;`), mirroring the SDK's own acceptance of the value; add a host test forcing `ct` to 7300/8300 expecting ≥95% hits.

*Verifier note:* none material; not reachable on the current fleet (V27/P03), relevant only if V25_P10 is ever redeployed.

#### S6. Legacy UCAP_SYNC never trains outside a 9.88-10.89 s period band [high] [legacy_SYNC]

`bthome_phy6222/source/ucap_sync.h:93-154`.

Period training requires a delta between two *consecutively caught* frames, but a miss clears `have_prev` and the guard caps at 500 ms, so any true period outside `t_est ± ~500 ms` around the 10390 ms seed can never produce a trainable delta. Host simulation of the actual glue: P=10900 ms → 50% catch, 11.8% duty; P=12000 ms → 19.8% duty; P=9850 ms → 34.9% duty.

**Failure scenario:** a main-MCU clock (temperature-dependent, alkaline cell, freezer) drifting to ≥10.9 s or ≤9.88 s: every third window becomes a ~13 s full-period listen at 1-2 mA, roughly 120-400 µA average instead of the ~10-20 µA design point — while data still advertises every 10 s (stale on alternate frames), so HA does not alarm.

**Fix:** if this image is kept at all, do not clear `have_prev` on a single miss (integrate dt over the known missed-period count), and widen the host simulation past 8-13 s.

*Verifier note:* the evidence's stated causal chain (T_MIN/T_MAX gate) is imprecise — the real gate is the guard-bounded catch window, not the training-band check — but the numeric conclusion (9.88-10.89 s) is independently reproducible and correct. The supporting field data point (T_est drift +126 ms/hr) was measured on a P10 unit, not a legacy SYNC unit — corroborating, not direct, evidence.

#### S7. Advertising-dead-but-sleeping escapes the watchdog: a GAP failure has no retry, and every sleep/wake feeds the WDT regardless [high] [V27_P03, V25_P10, legacy_SYNC, PROBE]

`bthome_phy6222/source/thb2_peripheral.c:1187-1195`.

```c
else { gapRole_AdvRestartReq = FALSE; gapRole_state = GAPROLE_ERROR; }
```
has no `osal_start_timerEx` retry, unlike the synchronous-failure path at `:825-835`. If `GAP_MAKE_DISCOVERABLE_DONE` or an `END_DISCOVERABLE_DONE` restart ever completes with a failure status, nothing ever re-posts `START_ADVERTISING_EVT`. The chip keeps sleeping/waking on P03/RTC (feeding the watchdog via the ROM wake path's `__wdt_init`), so the watchdog cannot rescue it.

**Failure scenario:** a transient HCI/LL rejection during an advertising restart leaves the unit in `GAPROLE_ERROR` with `gapRole_AdvEnabled` still `TRUE` but no more `ADV_BROADCAST_EVT` ever firing. Sleep current stays normal, the watchdog is fed forever by wake cycles, HA marks the unit unavailable, and only a battery pull recovers it.

**Fix:** mirror the synchronous path's 1 s retry timer in the `END_DISCOVERABLE_DONE`/`GAPROLE_ERROR` branches; add an application-level dead-man check independent of the wake re-arm (e.g. "no adv-done in 5 min → soft reset").

*Verifier note:* the triggering LL rejection's real-world probability is unquantified (no concrete non-self-healing trigger found in visible code); the code-level dead end and watchdog blind spot are confirmed, which is why this sits at high rather than critical for a probability-dependent failure.

#### S8. OTA bounds still use 2 MB FLASH_MAX_SIZE on a 512 KB part [high] [all]

`bthome_phy6222/source/ble_ota.c:155-157`.

```c
if(ota.program_offset >= FADDR_APP_SEC &&
   ota.program_offset + (ota.pkt_total << 4) <= FADDR_START_ADDR + FLASH_MAX_SIZE)
```
`FLASH_MAX_SIZE` is `0x200000` (2 MB); the actual part is `FLASH_SIZE = 512*1024`. The no-SET path derives `pkt_total` straight from the image's own header with no bound at all.

**Failure scenario:** during a connectable window, an oversized or malformed OTA image (`pkt_total` up to `0x8000`, which passes the 2 MB check) erases sectors well past 512 KB, including the EEP config banks at `0x7C000-0x7FFFF` (MAC address, calibration, name), before the final CRC32 can reject the transfer.

**Fix:** bound `program_offset + (pkt_total<<4)` against the real flash capacity minus the EEP banks in both the SET and no-SET paths; refuse `pkt_total == 0`.

*Verifier note:* the further claim that the address then "wraps and overwrites the boot image" depends on precompiled `spif_write()` behavior not visible in source — flag as unconfirmed SDK/ROM inference. The confirmed, source-verifiable damage (EEP bank erasure) stands on its own. Only reachable through a deliberate OTA session, not spontaneous field failure — a process-safety hazard for the *next* OTA, not a live fleet risk today.

#### S9. `fleet_flash_stock.py` default bundle is the V25 P10 experiment image, not V27 [high] [tools]

`fleet_flash_stock.py:47-48, 242, 285-291`.

```python
DEFAULT_BUNDLE = Path(__file__).parent / "inkbird_fw" / \
    "STAGE3_IBSTH2P_v25_p10_stock_bundle_installer.hex16"
```
set before V27 existed and never moved; the success check derives its "expected" revision from the very bundle chosen, so it cannot catch this.

**Failure scenario:** `python3 fleet_flash_stock.py <MAC>` on a new stock unit lands it on `IBS-P25` — the experimental P10 scheduler (half the frame rate by design, connectable steady state, `RF_PHY_TX_POWER_MAX`) — and the tool prints SUCCESS.

**Fix:** point `DEFAULT_BUNDLE` at the v27_p03 STAGE3 installer, or require the bundle argument explicitly; print the bundle's own revision string and require operator confirmation.

### Medium

#### S10. GAP_EndDiscoverable failure leaves the unit connectable at 1.56 s indefinitely [medium] [V27_P03, V25_P10, legacy_SYNC]

`bthome_phy6222/source/thb2_main.c:219-230, 382-447`.

The steady-state choke point sets `gapRole_AdvEventType = NONCONNECTABLE` and calls `set_new_adv_interval()` exactly once; if `GAP_EndDiscoverable()` returns non-`SUCCESS`, the comment says "the new parameters still take effect on the next advertising start" — but in steady state there is no next start, and nothing re-arms `adv_reload_count`.

**Failure scenario:** boot or button window ends; `GAP_EndDiscoverable()` fails once (e.g. `gapConnectedCleanUpAdvertising()` frees `pGapAdvertState` on `GAP_LINK_ESTABLISHED`, and `GAP_EndDiscoverable()` returns `bleIncorrectMode` when it is `NULL` — a concrete in-tree trigger, not purely ROM-dependent). The unit advertises `ADV_IND` at 1.56 s (≈19 µA adv term vs ≈2.9 µA) and stays connectable until a button press or connection — for V27 this also reopens the connection attack surface V27 was meant to close.

**Fix:** on failure, set `adv_wrk.adv_reload_count = 1` so the next `ADV_BROADCAST_EVT` retries the transition.

*Verifier note:* merges two submissions (main-adv/x-silence and x-drain-budget) targeting the call site and the definition of the same defect. For V25_P10/legacy_SYNC, steady state was already always-connectable in this build, so an equivalent failure there just sticks at the *fast* interval rather than adding new connectable exposure.

#### S11. One failed connection attempt collapses the 60 s connectable window to a single event [medium] [V27_P03]

`bthome_phy6222/source/thb2_main.c:1243-1267`; `thb2_peripheral.c:1242-1283`.

`GAPROLE_WAITING` is reached both on a clean disconnect *and* on a rejected/failed connection establishment; its handler unconditionally forces `adv_wrk.adv_reload_count = 1`.

**Failure scenario:** operator presses the button; a central's connect attempt fails on the first try (e.g. a short host-side connect timeout). The unit advertises connectable for one more 1.56 s event, then reverts to non-connectable — the remaining ~58 s of the intended window is gone, and the operator has to walk back and press again.

**Fix:** only force `adv_reload_count = 1` when the preceding state was a real established connection (track a flag), not on a failed/rejected establishment.

#### S12. No bias learner: a constant offset >100 ms produces a permanent M,M,H (or worse) oscillation [medium] [V25_P10]

`bthome_phy6222/source/ucap_p10.h:306-323`.

Hit position relative to the window (ideal = `guard + 18`) is recorded in `last_hit_pos_ms` but never fed back into `t_est`; a hit unconditionally resets `guard_ms` to base. The project's own test documents the resulting cycle: "Bias 208 ms... M,M,H cycles... → 33% hits."

**Failure scenario:** any persistent offset >~100 ms (chiefly from S5's RC-correction failure) costs 2-4 extra 20 ms VERIFY + 230-830 ms UART-on cycles per captured frame — roughly a 5-10× increase in receiver energy, cadence dropping to one sample per 30-50 s.

**Fix:** after a hit, compute the error against the ideal position and apply a damped correction to `t_est_ms` before resetting the guard.

*Verifier note:* the 208 ms bias demonstrated in-repo (T26) is specifically from an RC-correction failure (S5), not general wake-latency asymmetry as the failure scenario also speculates — the RC-error cause is the one actually demonstrated.

#### S13. Frame-as-burst verdict needs ≥3 IRQ-counted edges; margin depends on unverified wake latency [medium] [V25_P10]

`bthome_phy6222/source/ucap_p10.h:36`.

`P10_VERIFY_MIN_EDGES = 4`, of which the wake hook contributes 1; the remaining ≥3 must arrive as GPIO IRQs within the ~13.5 ms frame. If AON-wake-to-IRQ-live latency at cold temperature exceeds ~10 ms, every wake-frame classifies as a glitch and no window is ever armed — complete, silent data loss for that unit.

**Fix:** accept a lower edge count for the wake-stamped case, or confirm the latency bound with the V26/V27 `wake_lat` telemetry before re-shipping this image.

*Verifier note:* raised from low to medium — a genuine complete-silence mode with solid code-path support, but not higher since it targets a variant not on any fielded unit and the sibling P03 path's bench-confirmed 2.2 ms wake latency suggests the shared ROM path is plausibly well inside margin.

#### S14. Legacy UCAP_SYNC treats a CRC-bad completion as silence, escalating to a 13 s reacquire window [medium] [legacy_SYNC]

`bthome_phy6222/source/cmd_parser.c:624-627`.

Unlike the P03/P10 branches, the SYNC branch of the UART callback has no CRC-bad signal path — a corrupted frame inside a window simply times out as a plain miss, doubling the guard and, on a second consecutive miss, opening a ~13 s full-period reacquire window.

**Failure scenario:** a persistently CRC-bad stream (e.g. a UART FIFO overrun during a flash-erase-masked IRQ) leaves a SYNC unit in backoff, ~13 s awake at 1-2 mA every 5 minutes (~65 µA average), with stale sentinel data advertised throughout.

**Fix:** if kept, mirror the P03 branch's CRC-bad signaling so it is treated as a timing hit, not a miss.

#### S15. `fleet_flash_custom.py` still defaults to the v24 legacy-SYNC image [medium] [tools]

`fleet_flash_custom.py:26-31`.

```python
"v24": "BOOT_IBSTH2P_v24_ota.bin",   # default, fleet release
IMAGE = os.environ.get("IBS_OTA_IMAGE", "v24")
```
The V27 commit added a `"v27"` key but left the default and the docstring unchanged.

**Failure scenario:** any reflash without setting `IBS_OTA_IMAGE=v27` silently regresses a unit to the legacy SYNC scheduler with the period-lock-in drain (S6); the tool's own post-flash check reports `IBS-V24 CONFIRMED` as success.

**Fix:** change the default to `v27` and update the docstring, or refuse to run without an explicit image choice.

#### S16. Unauthenticated debug/config GATT commands (MEM_RW, REG_RW, EEP_RW, CFG) compiled into the shipped image [medium] [all; exposure bounded to two ~60 s windows on V27, effectively continuous on V24/V25]

`bthome_phy6222/source/cmd_parser.c:1461-1474, 1972-1999`.

The 0xFFF4 command characteristic accepts arbitrary RAM/register writes, EEP writes, and full config replacement from any central, with no pairing/encryption (`GATT_PERMIT_WRITE` plain, bond manager disabled). `test_config()` only clamps `rf_tx_power` to `RF_PHY_TX_POWER_EXTRA_MAX` (0x3f) and does not bound `batt_interval` at all.

**Failure scenario:** any BLE central in range during a connectable window writes a config with `rf_tx_power=0x3f` (persistent higher-current PA/DCDC trims) or `batt_interval=0` (ADC every advertising event), or pokes AON/PMCTL registers directly — persists across reboots until another config write or a battery pull.

**Fix:** compile `MEM_RW`/`REG_RW`/`EEP_RW` only under a debug flag; clamp `rf_tx_power` to the fleet ceiling and `batt_interval` to a sane minimum in `test_config()`.

*Verifier note:* exposure differs sharply by variant — V27 bounds this to the two ~60 s connectable windows; V24/V25 are connectable in steady state, making exposure effectively continuous.

#### S17. Default UCAP path for an IBSTH2P build without `-DUCAP_P03` is the known-bad legacy SYNC scheduler [medium] [docs/build-recipe, legacy_SYNC]

`bthome_phy6222/source/config.h:119-121`.

```c
#if !defined(UCAP_PROBE) && !defined(UCAP_P10) && !defined(UCAP_P03)
#define UCAP_SYNC 1
#endif
```
`IBS_FW_VERSION` (27) is shared by all UCAP variants; only the revision letter differs. The project's own documented "reproducible build" recipe (`PROJECT_DEF="-DDEVICE=DEVICE_IBSTH2P" BOOT_OTA=1`, no `UCAP_*` define) hits this default and would produce an `IBS-V27` image reporting firmware 27.0.0 over BTHome while actually running the legacy SYNC scheduler (S6's drain mode).

**Fix:** make `UCAP_P03` the default for `DEVICE_IBSTH2P`, or `#error` when no `UCAP_*` define is given for a release build; have the flashing tools assert the DIS revision letter before flashing.

*Verifier note:* the originally-cited `mk_all.py` trigger is factually wrong — `mk_all.py`'s device list does not include IBSTH2P, so it cannot produce this build at all (see refuted claims, item 1). The real trap is the documented manual build recipe, and it is latent (does not affect the already-shipped V27 image), not a current field defect.

#### S18. MOD_ADCC sleep lock is released only inside the ADC IRQ, with no timeout [medium] [all]

`bthome_phy6222/source/battery.c:90-95` (lock), `:79` (unlock).

`batt_start_measure()` takes `hal_pwrmgr_lock(MOD_ADCC)`; the only unlock is in `hal_ADC_IRQHandler()`. There is no fallback if that IRQ is ever lost (e.g. a re-init racing a completing conversion).

**Failure scenario:** one lost ADC completion IRQ → `MOD_ADCC` stays locked → chip never sleeps again → ~3 mA continuous, while advertising (and thus the watchdog feed) continues normally, so HA sees a healthy unit with a collapsing battery slope. Same signature as an unexplained field drain anomaly, at higher magnitude.

**Fix:** in `adv_measure()`, detect `MOD_ADCC` locked with no new `battery_mv` since the last `batt_start_measure()` call and force the stop sequence (disable ADC, unlock) with a counter exposed via telemetry.

*Verifier note:* raised from low — the consequence, if triggered, is severe (sustained high current in a freezer unit) — but no code-reachable path actually drops the IRQ under normal operation; all call sites are safely interval-gated, so this is a single-point-of-failure with an unconfirmed trigger, not a proven recurring defect.

### Low

#### S19. Stale TIMEOUT event bit can tear down a freshly re-armed period [low] [V27_P03]

`ucap_p03.h:198-201`. `osal_stop_timerEx` cannot retract an already-posted `SBP_P03_TIMEOUT_EVT`; a timeout expiring just before a CRC-bad re-arm can unlock while bytes are still in flight. Cost per rare occurrence: one lost frame plus one extra 250 ms window. Self-heals via the next RX/EDGE.

#### S20. BURST_RST clears the frame parser while bytes may be in flight [low] [V27_P03]

`cmd_parser.c:468-473`; `ucap_p03.h:205`. In `SUSPENDED`, a late-processed `RISE` (task-latency race during a connection/OTA session) wipes `in_frame`/`pos` while the ISR is mid-frame, discarding the leading bytes — can drop the button-state byte for that cycle.

#### S21. P03 pull-down costs ~20 µA continuously if the main MCU idles P03 high [low] [V27_P03]

`cmd_parser.c:681`. `GPIO_PULL_DOWN` on P03; the high-dwell time is measured (`high_last_100us`, GATT op 10) but unbounded by design. If the main MCU drives P03 as a level rather than a pulse, this alone could exceed the entire sleep budget. *Verifier note:* a supporting citation to a non-existent doc file was removed; recommend simply reading `high_last_100us`/`falls_wake` from a few fielded units.

#### S22. V27 rf_tx_power change is a no-op [low] [all IBSTH2P builds using this config.c field]

`config.c:53`. `RF_PHY_TX_POWER_0DBM == RF_PHY_TX_POWER_MAX == 0x1f` under the `__DEF_CHIP_QFN32__` branch that `DEVICE_IBSTH2P` defaults to (nothing in `config.h` overrides `SDK_VER_CHIP` for this device). `rf_phy_set_txPower()` writes the identical register value either way; confirmed against both shipped binaries (`def_cfg` byte `0x1f` at the expected offset in both v26 and v27 images). Any battery-slope comparison attributing improvement to this change is measuring the advertising-type change (S-none, the real V27 change) instead.

**Fix:** either drop the change and correct the comment, or pick a code that actually differs (`RF_PHY_TX_POWER_N2DBM`/`N5DBM`), after confirming reception margin.

*Verifier note:* merges two identical submissions (config.c:53, one initially mis-cited as :245); severity downgraded to low across the board since the register write is bit-identical to pre-V27 — zero device-behavior consequence, an analysis-attribution risk only.

#### S23. `SBP_RESET_ADV_EVT` meas_count seed is dead code [low] [all]

`thb2_main.c:834-839`. Computes `cfg.measure_interval - 2` = `0xFF` for these builds, immediately overwritten by `GAPROLE_ADVERTISING`'s `meas_count = 0`. No current effect; would matter if `measure_interval` were ever raised.

#### S24. Connectable-window lengths do not match their comments [low] [V27_P03 both sub-issues; V25_P10/legacy_SYNC OTA-window sub-issue only]

`thb2_main.c:695-699`. The OTA-boot window runs 60 events at the *connectable* interval (~94 s), not the ~60-80 s the comments claim; the button window's hold branch skips `adv_reload_count` decrements for 6 events (~69 s actual vs "~60 s" commented). Negligible energy; wrong operator-facing numbers. *Verifier note:* the button-hold sub-issue is new in V27 (confirmed via `git show` against the pre-V27 commit); the OTA-window sub-issue pre-dates V21 and applies to all three variants.

#### S25. MOD_USR1 registration failure silently disables the VERIFY sleep lock [low] [V25_P10]

`cmd_parser.c:660-661`. If a future build fills the 10-slot pwrmgr table, `hal_pwrmgr_lock(MOD_USR1)` becomes a no-op and the receiver loops wake→VERIFY→sleep→RECOVER on every frame, never opening a window. Latent (table is not full today).

#### S26. Timer-start return values ignored in `p10_exec` [low] [V25_P10]

`cmd_parser.c:318-320`. A failed `osal_start_timerEx` (uncheckable ROM behavior) leaves a lock held until the ~12 s sanity sweep instead of the intended timer duration — bounded, self-recovering.

#### S27. CRC-bad 13-byte buffer treated as a timing hit in P10 [low] [V25_P10]

`cmd_parser.c:603-607`. A CRC-bad completion still closes the window and re-learns `t_est`/anchor from garbage timing, costing one wasted window and inflating `hits_bad` telemetry before edge-learning self-repairs.

#### S28. Frame type byte[7] ignored across all variants [low] [all]

`cmd_parser.c:507-524, 748-751`. Temperature/humidity are always read from a fixed offset regardless of the documented frame-type byte; a type 0/2/3 frame (layout unconfirmed, never observed in this repo) would be mis-mapped and could produce a spurious HA spike for one cycle.

#### S29. Legacy SYNC scheduler stops permanently if boot UART init failed [low] [legacy_SYNC]

`cmd_parser.c:799-814`. `ucap_sync_close_evt` returns early on `!grab_active` without re-arming the open event — latent, requires a future change that initializes UART before `ucap_init`; not known to occur today.

#### S30. Legacy SYNC re-anchors phase to a button-inserted frame [low] [legacy_SYNC]

`ucap_sync.h:109-121`. A press frame inside a listen window is kept as the new phase anchor even though the next *periodic* frame still arrives on the original schedule, occasionally triggering the miss chain. Rare (1-5% of presses), bounded cost.

#### S31. SDK UART RX ISR copies FIFO bytes into a fixed 16-byte array without clamping length [low] [all]

`bthome_phy6222/SDK/components/driver/uart/uart.c:115-133`. `len = cur_uart->RFL` is used unbounded against a 16-byte stack array; correctness depends on unverifiable hardware FIFO depth. If ever exceeded, a Cortex-M0 stack overwrite → HardFault → watchdog reset (one lost frame, not permanent).

#### S32. Unreachable legacy grab-close branch and inert `grab_active` plumbing [low] [all]

`cmd_parser.c:757-768`. Dead code path (config.h's `#if` logic makes the negative condition always false); confirmed harmless today but misleads readers about MOD_UART0 ownership in the P03 build.

#### S33. Per-wake `uart_hw_init` resets the RX FIFO, losing a P10-start-bit-only wake [low] [V26_P03, V27_P03]

`bthome_phy6222/SDK/components/driver/uart/uart.c:405-408`. The wake handler unconditionally re-runs full `uart_hw_init` (including an RX FIFO reset), discarding any byte that arrived before the hook ran. Underpins S3's mechanism. *Verifier note:* present since V26_P03 (unchanged in the V27 diff), so attribute to both, not V27 alone.

#### S34. `hal_pwrmgr_unregister` compacts the context table with an overlapping `memcpy` [low] [V25_P10, legacy_SYNC]

`bthome_phy6222/SDK/components/driver/pwrmgr/pwrmgr.c:261`. Undefined behavior if the libc `memcpy` is ever non-forward-copying; exercised every listen window in P10/SYNC (UART0 is deinitialized per-window there), not in P03 (UART0 stays registered).

#### S35. Sleep-entry race: P03/P10 rising during the ROM sleep sequence arms the wrong wake polarity [low] [V27_P03, V25_P10]

`bthome_phy6222/SDK/components/driver/gpio/gpio.c:552-556`. If the pin rises after the ROM commits to sleep but before polarity is latched, the AON wake is armed FALLING instead of RISING, losing that edge; caught (partially, via the redundant P10 wake) or missed once, self-heals. Estimated ~1 event per 10 days per unit.

#### S36. P09 pulled down at boot, possibly holding the main MCU's UART RX in a break condition [low] [all]

`bthome_phy6222/source/main.c:397, 420-422`. Stock protocol uses P09 as PHY→MCU TX (idle high); this firmware never drives it and instead applies a weak internal pull-down. Effect on the (unreviewable) main MCU is unconfirmed — plausibly nil, since the main MCU's UART RX is documented as normally asleep between wake pulses.

#### S37. `CMD_ID_MTU` indexes `gAttMtuSize[1]` with a possibly-`INVALID_CONNHANDLE` index [low] [all]

`cmd_parser.c:1625-1634`. Deferred command processing can run after a disconnect sets `gapRole_ConnectionHandle = 0xFFFF`; `ATT_UpdateMtuSize`/array read then index far out of bounds (array size 1, `MAX_NUM_LL_CONN=1`). No tooling in this repo sends `CMD_ID_MTU`; a third-party client would be needed to trigger it. Outcome (fault vs silent corruption) is not determinable from source.

#### S38. CCCD hygiene: OTA/battery notify subscriptions leak into the next connection [low] [all]

`bthome_phy6222/source/battservice.c:317-330`. `Batt_HandleConnStatusCB` is never registered via `linkDB_Register`, and the OTA characteristic's client config is never reset on disconnect. With one connection slot, the next central can inherit stale notification enables. GATT hygiene only.

#### S39. Non-connectable steady state suppresses scan responses, hiding the device name [low] [V27_P03]

`thb2_main.c:444-445`. `LL_ADV_NONCONNECTABLE_UNDIRECTED_EVT` ignores `SCAN_REQ`, so the local name (carried only in the scan response) is invisible outside the boot/button windows. Not a battery or silence issue; affects initial HA discovery UX only.

#### S40. BTHome object 0x09 means different things on P10 vs P03 units in the same fleet [low] [V27_P03, V25_P10]

`bthome_beacon.c:78-90`. Same entity ID carries listen-window health % on P10 units and P03-edge-to-first-byte lead (ms) on P03 units; a fleet mixing both images (both are currently shipped) makes any dashboard keyed on this field ambiguous unless joined with the 0xF2 firmware version.

#### S41. rf_tx_power upper clamp allows EXTRA_MAX (0x3f), a persistent high-current PA/DCDC trim [low] [all]

`config.c:141-144`. `test_config()` clamps to `RF_PHY_TX_POWER_EXTRA_MAX` rather than the intended fleet ceiling; a config write of 0x3f (e.g. copied from another pvvx-derived device's defaults) persists across reboots at higher current until another config write or battery pull.

#### S42. Diagnostic tools give no button-press guidance for V27's non-connectable steady state [low] [tools, V27_P03]

`ucap_stats.py:13-17`. Still tells operators to use the fast window after a battery reinsert, which zeroes the very RAM counters (`uptime`, `good_frames`, `crc_bad`) the tool exists to read; V27's actual non-destructive path (click the button) is undocumented in the tool.

#### S43. `bthome_catch_log.py` filters and labels by advertised name, which V27 no longer sends in steady state [low] [tools, V27_P03]

The local name lives only in the scan response, which non-connectable undirected advertising does not send; the documented `IBSTH2P` name-filter usage example captures nothing under V27.

#### S44. P03 host test harness models only the pure state machine, not the ISR-side glue [low] [V27_P03]

`bthome_phy6222/tests/test_ucap_p03.c:13-222`. 48/48 checks pass but none observe `burst_open`/`rise_pending` latches, the pre-`p03_step` lock taken in ISR/hook context, or event-bit coalescing — the P10 harness models its equivalent glue, this one does not. Concretely: after a CRC-bad completion, the following good frame is attributed as "no edge" in telemetry (`frames_no_edge` inflated), untested by any case here.

*Verifier note:* merges two identical submissions from separate passes (tests-tools and x-isr-race).

#### S45. `fleet_flash_custom.py` exits 0 when the post-flash revision is UNEXPECTED or unverifiable [low] [tools]

`fleet_flash_custom.py:85-102`. Both the "could not reconnect to verify" and the "UNEXPECTED!" paths `return 0`; a scripted loop over multiple MACs would not notice a wrong-image flash.

#### S46. Host tests not wired into any build target [low] [V27_P03, V25_P10, tools]

`bthome_phy6222/tests/`. No Makefile/`mk_all.py` test target; README documents only `test_ucap_frame.c`. `test_ucap_sync.c` and `test_flash_eep.c` share this gap.

#### S47. First-byte offset documented as 1.04 ms but implemented as 1.0 ms [low] [V27_P03]

`ucap_p03.h:30-31`. `P03_FIRST_BYTE_100US = 10` (1.0 ms) vs the comment's/test's/`ucap_stats.py`'s stated 1.04 ms — a 0.04 ms systematic bias in every reported lead, harmless except within 0.04 ms of a bin edge.

#### S48. FRAME hand-off has no latch: a CRC verdict can be overwritten before the task runs [low] [V27_P03]

`cmd_parser.c:575-581`. OSAL event bits coalesce; a good frame immediately followed by a bad completion (needs ≥13 extra garbage bytes within the task-scheduling gap) leaves only the bad verdict visible, costing an extra 250 ms hold with no data loss.

#### S49. `osal_start_timerEx` failure ignored; the only backstop for an unarmed ARMED lock is the advertising sweep [low] [V27_P03, V25_P10]

`cmd_parser.c:467`. If timer allocation fails, `ARMED` holds `MOD_UART0` with no timeout until the ~2 s (P03) sanity sweep — or indefinitely if advertising itself has stopped (which is itself a silence condition HA would catch).

#### S50. SDK re-arms GPIO edge polarity from the reported edge, not the current pin level [low] [V27_P03]

`bthome_phy6222/SDK/components/driver/gpio/gpio.c:507-511`. With both rise and fall handlers registered (P03 only — P10 registers falling-only), a missed second edge under a longer-priority IRQ can leave the pin armed for the wrong polarity until the next sleep/wake; self-heals, costs one lead sample and a slightly later lock via the RX-start fallback.

#### S51. `ucap_p03.h` header describes CRC-bad completion as a release condition; the code keeps the lock [low] [V27_P03]

`ucap_p03.h:9-12`. Documentation/code mismatch left over from a prior review fix (CRC-bad in `ARMED` now keeps the lock and re-arms the full 250 ms timeout). Risk is a future maintainer "fixing" the code back to match the stale comment.

#### S52. Battery-interval delta not masked to 24 bits (audit #33, still present) [low] [all]

`thb2_main.c:324-326, 909`. `clkt.utc_time_tik - adv_wrk.measure_batt_tik` is a 32-bit subtraction of 24-bit RTC values; at each 512 s wrap the delta reads ~4.29e9 and the ADC fires one cycle early — ~6% extra ADC duty, self-correcting cadence, no missed measurements.

#### S53. V27 button object airtime shrank from ~60 s to ~9.4 s [low] [V27_P03]

`thb2_main.c:250-262, 287-310`. `BTN_ADV_HOLD = 6` was sized for the 10 s steady interval; V27's click branch also switches to the 1.56 s connectable interval, so the six repeats of the button object (0x3a) span only ~9.4 s, not the ~60 s both code comments claim. The surrounding connectable *window* itself is unaffected (~69 s, since `adv_reload_count` is not decremented during the hold) — only the button-payload repeats are compressed.

**Fix:** scale the hold to time (`adv_button_hold = 60000 / DEF_CON_ADV_INTERVAL_MS`) or update the comments to state the ~9 s intent.

*Verifier note:* merges two identical submissions (x-arith and x-silence, same code, adjacent line ranges).

#### S54. `cmd_parser` assembles a command word from stale buffer bytes with a signed left-shift [low] [all]

`cmd_parser.c:1447`. `ibuf[4] << 24` is UB for `ibuf[4] >= 0x80` on a signed `int`; every consumer of the resulting `tmp` is guarded by a `len` check, so this is hygiene only (would be flagged by `-fsanitize=undefined` if this file were host-tested).

## Per-frame energy budget of V27

10 s period, V27 P03, steady state, no connection. Current classes: sleep 2-5 µA, MCU awake @16 MHz ~3 mA, TX ~12 mA.

| # | Wake / interval | Trigger → ends by | Duration | Charge / avg | Avoidable? |
|---|---|---|---|---|---|
| 1 | RTC wake for advertising | LL RTC comparator → sleep entry | ~5 ms @3 mA + ~1.2 ms @12 mA | ~29 µC → ~2.9 µA | No (required); non-connectable already removes scan-response/RX-window cost vs V26 |
| 2 | P03 rise wake | P03 rise → CRC-good FRAME | ~16.5 ms (2.2 ms lead + 13×1.04 ms bytes + hooks/sleep) | ~50 µC → ~5 µA | **Largest avoidable-adjacent term** — every frame is received even though data changes slowly; see S1/S3 |
| 3 | P03 fall wake (conditional) | AON fall wake, only if main MCU holds P03 high past frame end | ~2-3 ms | 6-9 µC → 0.6-0.9 µA | **Avoidable if S21's high-dwell time is confirmed low**; scales with main-MCU hold time |
| 4 | Battery ADC | every 60 s, MOD_ADCC lock → ISR unlock | ~1-3 ms | ≤12 µC/60s → ~0.2 µA | No |
| 5 | 250 ms P03 timeout (fault path) | rise/first-byte without a good frame (S3, S19, S48) | 0 in the good path; 250 ms per fault | 0 normal; 750 µC per fault (=15 good frames) | **Fully avoidable** — scales with CRC-bad rate / edge-miss rate; see S3 fix |
| 6 | Watchdog | re-init + feed on every wake, plus adv completions | no wake of its own | 0 | n/a |
| 7 | Other OSAL timers | none in steady state | — | ~0 | n/a |
| 8 | Sleep | RET_SRAM0\|SRAM1, RC32K, low-current LDO | ~99.7% of period | 2-5 µA | n/a (baseline) |

**Total steady state: ~10-14 µA.** The V27 TX-power change (S22) contributes 0. The single largest lever available without a protocol change is reducing how often term #2 fires when data is not changing (S1's fix, done right, could also gate reception cadence); the next largest is closing term #5's fault-path cost (S3's fix). Term #3 needs a bench measurement of `high_last_100us`/`falls_wake` (S21) before deciding whether it is worth acting on. A field anomaly on the order of 100 µA cannot come from this steady-state table — it requires one of the fault paths in this review (a leaked lock, a connection dwell, or sustained CRC-bad/timeout activity).

## Legacy paths still in the tree (V25_P10, V21-V24 SYNC, PROBE)

Both legacy images are still shipped in `inkbird_fw/` and selectable via `fleet_flash_custom.py`/`fleet_flash_stock.py`, and both tools currently default to them (S15, S9) — the single biggest operational risk this review found.

- **V25_P10**: a well-engineered scheduler undermined by one real defect (RC32K correction discarded outside ±5%, S5) that can turn into permanent silent-but-advertising failure (S2, S12, S13) for any unit with enough clock drift or wake-latency margin loss. Not present on any fielded unit today.
- **V21-V24 SYNC**: a fundamentally narrower-margin design — locks into a 9.88-10.89 s period band (S6) and mishandles CRC-bad completions as plain misses (S14) with no equivalent of P03/P10's edge-based recovery. This is the design V27's P03 approach was built to replace.
- **PROBE**: shares the sensor-staleness code path (S1) but was not described as a shipped fleet image in any area summary; not separately assessed here.

**Recommendation:** delete rather than keep. The legacy SYNC scheduler has a real, quantified drain mode with no cheap fix that doesn't amount to re-deriving P03/P10; keeping it "just in case" is what let both flashing tools default to it. If V25_P10 must be kept as a fallback (e.g. hardware without a usable P03 line), fix S5 first — it is a five-line clamp — before any unit ships with it again. At minimum, before deleting anything: flip both tools' defaults to v27 (S15, S9) immediately regardless of the delete decision.

## Refuted or downgraded claims

- **"mk_all.py can build a DEVICE_IBSTH2P image with no UCAP define, defaulting to legacy SYNC."** Refuted: `mk_all.py`'s device list does not include IBSTH2P at all; it cannot produce this build. The real, still-live trap is the project's documented manual build recipe (S17), not this script.
- **"P03's pull-down current is bounded because stock firmware does the same."** The cited supporting file (`v25_p10_design/STOCK_FIRMWARE_WAKE_MECHANISM.md`) does not exist in this repository — drop that citation; the underlying question (how long does the main MCU hold P03 high) is still open and needs a bench measurement (S21).
- **"An unanchored P03 wake is classified SRC_UNKNOWN."** Downgraded: when the wake hook reads P03 HIGH it assigns `P03_SRC_WAKE` (a valid rise anchor), not `SRC_UNKNOWN` — the 250 ms cost conclusion in S3 still holds, but via a different code path than originally described.
- **"Back-to-back frames within one wake burst can be lost to BURST_RST."** Downgraded to unconfirmed: no evidence in the documented 13-byte single-frame protocol that the main MCU ever sends two frames per burst; only the SUSPENDED/connected-window race in S20 is verified.
- **"The T_est=10704ms/+126ms-per-hour field data point proves the legacy SYNC period-lock bug occurs in the field."** Downgraded: that measurement was taken on a P10 experimental unit, not a legacy SYNC unit — it corroborates that real periods drift beyond the assumed band, but is not itself a legacy_SYNC field observation.
- **"V27's TX-power no-op is a medium-severity issue via battery-slope misattribution."** Downgraded to low: the register write is bit-identical to pre-V27 firmware, so there is zero device-behavior consequence — this is an analysis-process risk (mislabeling which change caused an observed slope change), not a device defect.
- **"CMD_ID_MTU's out-of-bounds index causes a bus fault and a watchdog reset."** Downgraded to uncertain: the linker maps ~132 KB of contiguous SRAM across several regions, so the ~131 KB overrun could land in another mapped region as silent corruption instead of faulting — both outcomes are possible from source alone.
- **"The OTA overflow wraps the flash address and overwrites the resident boot firmware."** Downgraded: this depends on precompiled `spif_write()` address-wrap behavior not visible in source. The confirmed, source-verifiable damage — EEP config-bank erasure at 0x7C000-0x7FFFF — stands without needing the wrap claim (S8).
- **"V25_P10's health-percent staleness (S2) is a live fleet risk today."** Downgraded/scoped: the current 16-unit fleet runs V27/UCAP_P03 exclusively; S2 only matters if a unit is (re)flashed with the still-shipped v25_p10 image.

## Test and tooling gaps

- Host tests (`test_ucap_p03.c`, `test_ucap_p10.c`, `test_ucap_sync.c`, `test_ucap_frame.c`, `test_flash_eep.c`) all compile and pass at HEAD with `-fsanitize=address,undefined`, but none are wired into a build target — only the frame test is even documented (S46).
- The P03 harness tests the pure state machine only; the ISR-side latch/dispatch glue that has produced most of the low-severity findings in this review (S19, S20, S48, S50) is untested and would let a real regression through silently (S44).
- Both flashing tools default to a legacy image the fleet is no longer meant to run (S15, S9), and `fleet_flash_custom.py` reports success even when the post-flash revision is wrong or unverifiable (S45).
- Diagnostic tools were not updated for V27's non-connectable steady state: `ucap_stats.py`'s documented workaround destroys the counters it exists to read (S42), and `bthome_catch_log.py`'s name-based filter/labeling finds nothing (S43).
- No test exercises frame-age staleness (S1) or the P10 health-decay gap (S2) — these are exactly the "never go silent" property the fleet depends on, and there is currently no automated check that it holds.
- One test (`T16`) hedges its expected value with an OR although the underlying function is deterministic — tighten it.

## Suggested order of work

1. **Flip both flashing tools' defaults to v27** (S15, S9). Zero code risk, prevents the next routine reflash from silently regressing a unit to the legacy SYNC drain mode. Do this before anything else.
2. **Add frame-age staleness detection** to the shared sensor path (S1), and extend it to P10's health counter if that image is kept (S2). This is the one change that directly closes the "never silently go dark" gap; expected effect: converts an undetectable failure mode into an HA-visible alarm, at effectively zero battery cost.
3. **Shorten the P03 fault-path timeout** for unanchored wakes (S3), and fix the frame hand-off/latch issues that feed into it (S19, S48, S51 comment fix). Expected effect: bounds the worst-case fault-path drain from ~40-50 µA average down to near the ~5 µA good-path cost, with no protocol change.
4. **Fix the GAP_EndDiscoverable retry gap** (S10) and **the one-failed-connection window collapse** (S11). Expected effect: removes the only remaining paths to a permanently-connectable unit; small reliability/attack-surface win, no battery cost.
5. **Feed the watchdog from the connected loop and add a connection-age timer** (S4). Expected effect: eliminates the reset-reconnect loop risk for the next OTA/inspection session; no effect on steady-state battery.
6. **Correct or drop the TX-power change** (S22) — either restore an honest comment, or pick a code that actually reduces PA current after confirming reception margin. Expected effect: either fixes the battery-slope attribution, or delivers the TX savings the changelog already claims.
7. **Bound the OTA size check against real flash capacity** and exclude the EEP banks (S8). Expected effect: process-safety fix before the next fleet-wide OTA is performed; no effect on current steady-state operation.
8. **Gate the unauthenticated debug/config GATT commands** behind a debug build flag, and clamp `rf_tx_power`/`batt_interval` server-side regardless (S16, S41). Expected effect: closes a persistent-misconfiguration path that could otherwise look identical to a mystery drain.
9. **Add a timeout/backstop for the MOD_ADCC lock** (S18). Expected effect: defense-in-depth against a full-awake (~3 mA) failure mode that would otherwise be invisible to both the watchdog and HA.
10. **If V25_P10 is kept as a fallback image**, fix the RC32K clamp (S5) and the bias learner (S12) before it is ever flashed to a fielded unit again — both are prerequisites, not optional polish, given S5's "permanent all-miss" failure mode.
11. **Delete the legacy V21-V24 SYNC image** from `inkbird_fw/` and both flashing tools once the above tooling fixes are in, rather than carrying its unresolved period-lock-in and CRC-bad-as-miss defects (S6, S14) indefinitely.
12. **Close the test/tooling gaps** (wire host tests into a build target, extend the P03 harness to the ISR glue, fix the two diagnostic scripts for V27's non-connectable steady state, add a frame-staleness host test). Expected effect: no direct battery/reliability change, but prevents the next edit from silently reintroducing any of the above.

---

## Owner triage (2026-09-09)

Decisions taken after reading the review, recorded so the findings above are not re-litigated.

- **S1 (stale data advertised after main-MCU death).** Mechanism confirmed in code: advertising is driven by the link layer's RTC timer (`thb2_main.c:717`, `:804`), and `read_sensors()` copies the last frame and increments the packet id unconditionally (`sensors.c:120-125`), so a dead main MCU leaves the PHY6222 advertising the last temperature with fresh packet ids and HA's offline alarm stays quiet. Probability judged low by the owner (a main MCU dying alone has never been observed). Severity downgraded to low. If ever closed: stop advertising after ~5 min without a CRC-good frame so the existing HA offline alarm fires.
- **S3 (250 ms timeout on an edge-less wake).** Guard if the state machine is touched again: after the first byte the remaining 12 bytes take under 13 ms at 9600 baud, so re-arm the timeout to ~40 ms at first byte and use 40 ms from the start when the wake source was the start bit. Fault path only; the bench showed 202/202 edges. Deferred.
- **S4 (watchdog starves during a read-only connection).** Kept as is. It acts as the 64 s connection deadline the project wanted for battery reasons. Side effects (counter reset, battery-average restart, 60 s connectable window after the reboot) are acceptable; `ucap_stats.py` reads finish in seconds and OTA is all writes.
- **S7 (advertising restart failure has no retry).** Kept. Sleep current stays normal and HA marks the unit unavailable; cost is a battery pull. Not a drain.
- **S8 (OTA bound uses 2 MB on a 512 KB part).** Real (`ble_ota.h:13`) but only reachable with a malformed image; the flash tools send correct headers. One-line clamp when convenient.
- **S9, S15 (tool default images).** Dropped; the operator always sets the image explicitly.
- **S22 (V27 TX-power change is a no-op).** Confirmed: `SDK_VER_CHIP` defaults to QFN32 (`SDK/components/inc/version.h`), where `RF_PHY_TX_POWER_0DBM == RF_PHY_TX_POWER_MAX == 0x1f`. Register value unchanged from V24/V26; RSSI snapshots unchanged. TX power IS changeable: at runtime via `cfg.rf_tx_power` (`config.c:143-144`, GATT config write, persisted) or the compile-time default. Values that change the register on QFN32: `N2DBM` 0x0f, `N5DBM` 0x0a, `N10DBM` 0x04. What 0x1f actually radiates is ambiguous in source (driver header labels it MAX and 0 dBm; `ll.c` report table maps it to both 7 and 0 dBm). Decision: do NOT lower it. Datasheet TX is 4.6 mA at 0 dBm; three ~0.4 ms advertising packets per 10 s cost ~0.5 µA (at most ~1 µA if 0x1f is +7 dBm), under 10 % of the 10-14 µA steady-state budget, while the weakest links sit at −90 to −93 dBm and a 5 dB cut would push them to the decode edge. The energy levers that matter are the sleep floor and the ~5 µA per-frame UART receive.
