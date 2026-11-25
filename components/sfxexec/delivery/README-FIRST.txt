SFXEXEC COMPONENT TEST - 2026-10-24-r2
Engineering candidate. Native SFX + Sync coexistence is PENDING your test.

Extract so the candidate is C:\SFXDOS (short ASCII path, local fixed disk).
This is not Setup. It contains only SFXEXEC.DLL, matching DOS SFXDOOR.EXE,
the minimum SFXDOSHost.exe bridge, instructions and complete component source.
Do not replace the accepted SFX Bin files or any Synchronet files.

FIRST SETUP (Windows10/32 with working native NTVDM)
1. Run Verify-Package.ps1 before editing profiles. The supplied manifest checks
   every package file; BIN-IDENTITY.txt lists the three exact image hashes.
   In PowerShell: & C:\SFXDOS\Verify-Package.ps1
   If local execution policy prevents that optional checker, use Get-FileHash
   for the three BIN files against BIN-IDENTITY.txt; do not change policy.
2. Copy an independent Outpost Trader game directory into GAMES\OUTPOST,
   and an independent SF Guess directory into GAMES\SFGUESS. Use preserved game
   archives/independent game state, never the live X-Bit game folders. Games
   are not included. If NTVDM/short paths are unavailable, stop that case.
3. Edit PROFILES\outpost.ini and PROFILES\sfguess.ini. Set caller_alias to your
   designated SFX test alias. Confirm sfx_root, independent work_directory,
   sysop_name, location and security_level match this test account. The default
   five-minute budget is a TEST ceiling; SFX still enforces the caller's actual
   time/authority. Statistics not available to this bridge are neutral test
   values, not updates to the user's BBS record. Password fields stay blank.
4. In the existing SFXConfig Door setup add two test Doors, with these EXACT
   Door names and command lines (one per profile):

   SFX DOS - Outpost
   rlogin 127.0.0.1:15131 -q -P -t sfxdos/outpost

   SFX DOS - SF Guess
   rlogin 127.0.0.1:15132 -q -P -t sfxdos/sfguess

   Use no -p/-h/-H flags or saved password/pepper. These are private loopback
   test connections to the DOS bridge, not external servers. Restrict each
   test Door to the designated test account/security policy as appropriate.
   Door names must match profile door_name exactly. Profiles do not modify
   any SFX/Sync configuration automatically.
5. Before entering either Door, run Start-Outpost.cmd / Start-SFGuess.cmd in
   the SAME Windows user/elevation context as SFXRuntime. Leave them running.
   Each displays READY and records a new dated console log under LOGS. One
   active session per profile/game copy. Then log into the SFX test account
   and open its test Door. ACTIVE must report the real SFX node/account and
   actual driver path/hash. A normal exit should return to the same SFX menu.

The bridge requires the accepted October24-r1 SFXRuntime.exe, SHA256:
90E2F01DE0BBD018830E30EC58CD3703710A769A286DB1FB39C06A5780AE0C8F
It checks the socket peer PID, executable path/hash and Windows owner, then
binds a fresh typed Runtime Door entry to the caller/node/generation/session.
It requires ordinary Runtime file logging at <sfx_root>\Logs\SFXRuntime.log.
An unavailable/ambiguous/stale record fails closed; it does not guess a node.
The bridge never opens the SFX database or borrows SFXConfig's protected IPC.

USE THE SHORT OWNER-TEST-CHECKLIST.txt NEXT
Same-node FOSSIL/UART and SFX controls require native observation. Static/harness
passes do not establish coexistence. The minimum bridge uses existing SFX
gateway termination/time controls; its native VDM/job behavior is still pending.
CanDisconnect=false disables a GAME-requested whole-caller DTR disconnect in
this first profile. Test SFX's separate operator controls and ordinary caller
logoff instead. Completing local-Door engine/DTR integration is a later review.

LOADER / RESULTS
SFXDOOR requests ONLY the full private path to BIN\SFXEXEC.DLL. There is no
SBBSEXEC or bare DLL fallback. SFXDOSHost checks the driver's actual module
path/file identity/hash before forwarding game output. Private full-path VDD
loading on this Win10/32 host is PENDING, not claimed proven. If loading fails,
stop and send the new LOGS file/error; do not copy a DLL into System32. If native
loader evidence requires system placement, a revised reviewed instruction must
first name C:\Windows\System32\SFXEXEC.DLL, the exact candidate hash and rollback.
Never replace or rename production SBBSEXEC.DLL/DOSXTRN/INI.

Transient drop/ENV/INI files belong to the private candidate session. Launcher
RET/ERR results and their directories are retained under NODE\<actual node>\
<8-hex-session>. LOGS files are retained. No prior results are cleaned up.
If startup refuses an existing object/file or a non-private/shared VDM, stop
the case and retain the evidence. Do not terminate a pre-existing NTVDM process.

ROLLBACK
Exit the designated test Door, then Ctrl+C in each candidate host console.
Only a new VDM owned by that host/job may be terminated by it. Disable/remove
the two SFX test Door placements through normal SFXConfig when finished.
Keep C:\SFXDOS and logs/results for review; no uninstall/cleanup is required.
No accepted SFX binary, production Sync file, System32 file, AV setting, schema
or global PATH was changed by this candidate. Three queued starter categories
are not included. Full Setup and broader local-Door integration are later.

SOURCE / NOTICES
SOURCE\ contains complete corresponding component/bridge source and recipes.
The matching source/engineering ZIP supplies the same source plus build and
qualification receipts. GPL/LGPL notices and rights are retained; no compiler,
SDK/DDK media, emulator runtime, owner game, account/database or credentials ship.
Keep the component and source ZIPs together. This candidate is unsigned.
