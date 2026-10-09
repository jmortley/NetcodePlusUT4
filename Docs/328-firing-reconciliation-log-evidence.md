# 328 firing reconciliation: local log evidence

Read-only verification performed on 2026-09-04. This document describes the supplied logs, not a live reproduction or verified server binary. No C++ build was run.

## Files and session identity

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `C:\Users\MrJmo\Downloads\UnrealTournament (21).log` | 2,685,997 | `40551BA52BB7B63677F809AFFB46A007498C4B4D4BEED766FBC8261752E004E9` |
| `C:\Users\MrJmo\Downloads\UnrealTournament-backup-2026.09.03-23.59.19.log` | 245,372 | `80427B5C0573441B2EB583EA2D6EE83C1B92FC0F4FDAC1D398D7F65F2A859D43` |
| `C:\Users\MrJmo\OneDrive\Documents\Instance_08200350390907040000024EDB3C00D0.log` | 133,482 | `FF37AF166F97EFA5E8EB544CE015CBB6FA93ABE22594C450C8A31965F3198CB8` |

`UnrealTournament (22).log` was not found in targeted searches of Downloads, Documents, OneDrive Documents, Desktop, OneDrive Desktop, or Codex attachments. No equivalence to that absent file can be established. The user subsequently identified the backup and `(21)` as the client files to inspect; neither was silently substituted for `(22)`.

`(21)` and the server log contain the same September 4 Rankin session. The client loads `DM-Rankin-LE` at `2026.09.04-07.01.26:288` (line 1027); the server loads that map at `07.01.22:783` (line 570), then records the matching client identity joining at `07.01.29:927` (line 605). The same player identity is present in the client diagnostics. The session ends near `07.12.32` in both files. Sixteen client sends also align with the server rejection rows listed below. Network addresses and account identifiers are intentionally omitted here.

The backup is a **different, earlier completed launch/session**, despite its filename. Its first line says `Log file open, 09/03/26 01:27:47`; it loads Rankin at `2026.09.03-08.45.22:477` (line 1170), and closes at `2026.09.03-09.02.25:374` (line 1532; local close text is `09/03/26 02:02:25`). `(21)` begins with local open text `09/03/26 23:59:19`, then contains September 4 UTC rows. These files must not be concatenated as one firing session. The backup has zero `RocketM1Diag`, `CLIENT_SEND`, `NCFire`, or `REJECTED Rapid Fire` rows; it cannot extend the numbered-event evidence in `(21)`.

Log header/local close times differ from the UTC-style row timestamps. Cross-machine row timestamps have not been calibrated tightly enough to infer sub-frame input ordering.

## Independently recounted observations

Counts below use `(21)` and the supplied server log only.

| Observation | Count |
| --- | ---: |
| Client `[RocketM1Diag] CLIENT_SEND`, mode 0 | 81 |
| Client `[RocketM1Diag] CLIENT_ACK`, mode 0 | 110 |
| Sent rocket-primary IDs with no later equal mode-0 ACK | 16 |
| Server `REJECTED Rapid Fire` rows, including retries | 110 |
| Server mode 0 / minimum 0.960 | 78 |
| Server mode 0 / minimum 0.660 | 14 |
| Server mode 0 / minimum 1.260 | 14 |
| Server mode 1 / minimum 0.582 | 4 |
| Server rejection rows reporting `Delta: 0.000` | 18 |
| Client `ON-COOLDOWN -> retry scheduled` | 380 |
| Client `CROSS-MODE IsFiring -> retry queued` | 94 |
| Client `Deferring physical fire input across weapon swap` | 440 |
| Client `NCFire.Debounce` rows | 0 |
| Server `RocketM1Diag` rows | 0 |

The no-later-equal-ACK test matches each mode-0 send against all subsequent mode-0 ACK rows for the same ID. It does not turn an ACK into a projectile ledger, distinguish all retry causes, or prove that every other event fired successfully. These are row counts; 110 server rejection rows are not 110 unique failed shots.

Each of the sixteen IDs below has a nearby server mode-0/minimum-0.960 rejection. Times are the recorded row times, with date `2026.09.04` omitted. Alignment supports correspondence; it does not establish which callback advanced server `LastFireTime`.

| Event | Client line | Client send | Server line | First nearby rejection | Server delta |
| ---: | ---: | --- | ---: | --- | ---: |
| 7 | 3822 | 07:03:19.113 | 665 | 07:03:19.125 | 0.060 |
| 9 | 4580 | 07:03:29.321 | 671 | 07:03:29.335 | 0.034 |
| 17 | 8446 | 07:05:41.722 | 749 | 07:05:41.746 | 0.000 |
| 20 | 8698 | 07:05:47.987 | 752 | 07:05:48.009 | 0.043 |
| 39 | 9327 | 07:06:01.998 | 755 | 07:06:02.017 | 0.000 |
| 42 | 9655 | 07:06:11.286 | 757 | 07:06:11.307 | 0.017 |
| 44 | 9939 | 07:07:56.823 | 774 | 07:07:56.847 | 0.026 |
| 46 | 10116 | 07:07:59.777 | 777 | 07:07:59.800 | 0.000 |
| 59 | 10912 | 07:08:16.958 | 786 | 07:08:16.985 | 0.000 |
| 62 | 11058 | 07:08:24.101 | 793 | 07:08:24.141 | 0.000 |
| 64 | 11206 | 07:08:26.640 | 795 | 07:08:26.668 | 0.017 |
| 82 | 12744 | 07:08:58.369 | 812 | 07:08:58.399 | 0.043 |
| 86 | 13217 | 07:09:06.541 | 815 | 07:09:06.571 | 0.043 |
| 95 | 13738 | 07:11:09.385 | 833 | 07:11:09.415 | 0.000 |
| 100 | 13921 | 07:11:14.429 | 840 | 07:11:14.458 | 0.043 |
| 105 | 14634 | 07:11:57.653 | 859 | 07:11:57.691 | 0.043 |

## Specific examples and limits

**Event 7:** The prior local rocket-primary send is event 6 at client time `91.0430`, wall time `07:03:12.643` (line 3520), with ACK 6 at line 3529. Event 7 is sent at client time `97.5161`, wall time `07:03:19.113` (line 3822), a local elapsed interval of **6.4731 seconds**. Its client predicted projectile has `PROJECTILE_SPAWN_OK` at line 3823. The server rejects at `07:03:19.125` with `Delta: 0.060 < Min: 0.960` (line 665), followed by retries at lines 666-667. Client lines 3825, 3826, and 3833 report ACK 6, and there is no later ACK 7. This establishes client/server rate-state disagreement. It does not identify a server firing callback or prove a server projectile was spawned.

**Events 96-100:** Client lines 13819-13829 contain sends 96/97 and matching ACKs. Event 98 initially receives ACK 97, event 99 initially receives ACK 98, and event 100 initially receives ACK 99 (lines 13862-13932). Server lines 835-842 contain aligned small-delta rejection bursts. ACK 98 and ACK 99 therefore do appear later, which is why neither is in the sixteen-ID no-later-equal-ACK list. Equal ACKs must be interpreted with request timing/context; their existence alone does not prove the corresponding predicted projectile was accepted at send time. The same weapon instance is present through these sends, so the observed issue is not exclusive to switching.

**Event 62:** Client line 11054 reports Stop event 61 with `pending0=0` at `07:08:23.732`, local time `402.5235`. At `07:08:24.101`, local time `402.8936`, transactional state entry, send event 62, and a successful client predicted projectile occur with `pending0=1` (lines 11055-11059): approximately **370 ms** later. No intervening `AUTWeaponFix` Start log occurs in that interval. This is a strong unwanted-fire candidate, but the diagnostics do not observe all native/raw input or all upstream stock paths; absence of a physical/upstream press is not proven.

**Event 17:** Client line 8442 explicitly transfers cross-mode Retry 0 to pawn PendingFire at `07:05:40.842`. Event 17 is sent at `07:05:41.722` (line 8446), before the logged mode-0 Stop at `07:05:41.953` (line 8455). This is compatible with legitimate held-through-switch input before release and does not prove a phantom shot. Likewise, a stale-pending clear reports the bit that was cleared, not which callback set it.

## Causality and provenance

The appropriate conclusion is: **“Source-supported mechanism consistent with the observed failures.”** A complete incident chain still needs server firing-source and outcome telemetry.

- Server `StateLayout` lines 608-613 establish configured classes, including rocket-primary transactional and rocket-alt charged-transactional classes. They do not identify the live state/callback responsible for a particular failure.
- The server log has no `RocketM1Diag` rows. It cannot prove equip completion, stock sync, charged handoff, or another callback caused any specific shot or `LastFireTime` update.
- `LastFireTime` is rate state, not a spawn/damage ledger. Overrides, missing projectile classes/owners, spawn failure, direct/watchdog writes, and rhythm compensation can separate it from the actual firing outcome or execution time.
- `Delta: 0.000` is not a unique previous-frame signature; same-dispatch writes, rounding, and snapped times can also fit.
- Cross-machine wall times and local world-time deltas are insufficient for a claim that event 82 preceded physical input by 13 ms.
- The logs do not establish the exact live NCP binary hash or server source commit. A UT4AC build label or configured state class does not supply that provenance.
- Zero logged `NCFire.Debounce` engagements provides no evidence that optical-switch debounce caused these failures.
- Stock reconciliation filtering and dropped-release identity are distinct concerns. These logs do not justify accepting duplicate Stops whenever `PendingFire` is true, nor do they establish that filtering stock sync makes every later release ID unique.

## Verification boundaries

Counts were reproduced with PowerShell/regex reads and SHA-256 via `Get-FileHash`. The existing repository test convention uses `WITH_DEV_AUTOMATION_TESTS` and `IMPLEMENT_SIMPLE_AUTOMATION_TEST` under `Source/Private/Tests`. No engine automation tests or C++ builds were run for this evidence document. Source-diff checks and focused non-engine state-model checks can support review; only compiled runtime testing with captured event, ACK, request provenance, release ownership, aim, and actual outcome records can close the live behavior acceptance matrix.
