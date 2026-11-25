SFXEXEC LOADER DIAGNOSTIC - 2026-10-24-r4
The native loader cause is OPEN. This is one instrumented retry, not a verified fix.

WHAT CHANGED
The qualified r2 SFXEXEC.DLL is byte-identical. SFXDOOR.EXE and SFXDOSHost.exe
now identify BOP/pair/open/report stages, validate/hold the matching files and
log the owned process's image/hash/owner and a read-only NTVDM export inventory.
SFXLDR.DLL is a small, private diagnostic VDD: no CRT, entry-point code or static
NTVDM imports. It is requested by an exact full path only AFTER the direct core
BOP registration fails. If it can load, it asks Windows to load that exact core
path inside the VDM and reports the actual Win32 result, then unloads it. It
does not open a Door, install hooks, access channels, or turn a failed BOP into
success. If the probe cannot register/map its request, the log says that detail
is unavailable. There is no bare-name/core fallback and no System32 placement.

ONE OWNER-CONTROLLED WINDOWS10/32 RETRY
1. Stop the idle SFX test host consoles with Ctrl+C. Leave production Sync and
   its ordinary callers alone. Preserve the current candidate BIN as BIN-r2.
2. Extract this ZIP to a SEPARATE staging folder. Copy ONLY these four files
   from its BIN into the existing local D:\SFXDOS\BIN:
     SFXEXEC.DLL   SFXDOOR.EXE   SFXDOSHost.exe   SFXLDR.DLL
   BIN-IDENTITY.txt gives their exact hashes. Retain the r2 ZIP/BIN and all game
   copies, profiles, LOGS and NODE results. No full SFX install or drive move.
   Do not copy the source ZIP's engineering fixture binaries; none are supplied.
3. Keep the working profile/command that already reached READY. Confirm its
   existing sfx_root=D:\SFX and work_directory=D:\SFXDOS\GAMES\guess, exact Door
   name SFX DOS - SF Guess, mode=uart, dropfile=sfdoors, and designated test alias.
   No -p/-h/-H or saved remote password/pepper. Its Door command stays:
     rlogin 127.0.0.1:15132 -q -P -t sfxdos/sfguess
   The final saved profile was not acquired; do not replace it with a purported
   recovered owner configuration. Generic examples are in EXAMPLES only.
4. Run the existing D:\SFXDOS\Start-SFGuess.cmd locally under the SAME Windows
   user/elevation as accepted SFXRuntime. It resolves its own root and records
   a fresh dated LOGS file. Wait for READY; enter SF Guess ONCE in the SFX test
   login. No debugger, policy/registry/PATH change or production restart is needed.
5. If loading fails, stop this case. Retain the NEW LOGS\sfguess-*.log and report
   whether you returned to the same SFX Door menu. Send that one log for review;
   do not try System32 copies, changing DLL names or other production experiments.
   SFXLOAD lines distinguish BOP AX from a Win32 load error. Probe failure is a
   useful bounded result too; it does not authorize a search/placement workaround.
6. If direct registration succeeds: expect pair/private INI accepted, the actual
   core DLL path/hash, ACTIVE with real node/account, then game input/display.
   Exit normally to the same SFX menu. Only after SF Guess starts successfully
   resume Outpost FOSSIL and the original same-visible-node FOSSIL/UART/mixed
   SFX+Sync matrix. Stop at the first failed case and retain its evidence.

DROP FILES / DIRECTORY EXPLANATION
The host creates its candidate NODE\<actual visible node>\<random8hex> session
folder automatically. It is test-session isolation, not the final managed SFX
Node1..count implementation. Fresh SFDOORS.DAT is created in GAMES\guess; DOOR.SYS
for Outpost goes in its private node/session folder. The log prints the chosen
path. Existing dropfiles are refused/preserved, not overwritten. Only newly
created transient ENV/INI/dropfiles are removed by existing session error/exit
handling; directories, RET/ERR and dated logs remain. An empty session folder
after failure does not establish successful VDD loading or missing generation.
Do not borrow a live Sync dropfile or rename SFDOORS.DAT to SFNODES.DAT.

LIMITS / ROLLBACK
The existing SFX Runtime is not rebuilt: its accepted SHA256 remains
90E2F01DE0BBD018830E30EC58CD3703710A769A286DB1FB39C06A5780AE0C8F.
Runtime owner/path/hash and fresh typed caller/node/session binding, private
channels, blank passwords, time ceiling, game DTR CanDisconnect=false and the
separate operator controls are retained. Their native effects/coexistence remain
pending. A local/mock loader pass is not native NTVDM/BOP proof.

To roll back: with test hosts stopped, restore the THREE r2 BIN files from the
preserved BIN-r2. Leave the new private SFXLDR.DLL unused and retain all evidence;
r2 never requests it. Profiles/games/logs/node results are not reset. No System32
file, Sync component, account/database, security policy, schema, full Setup,
global/per-Door INI/YieldInterval setting or queued starter category is changed.

WINDOWS7/10/11 CHECKS
Windows10/32 native NTVDM is the selected test. Windows7 SP1 x86 import/subsystem
checks are local evidence; its native VDD execution is still pending if no host
is available. Windows7/10/11 x64 must refuse before listening or installed-board
access; no native16-bit pass or third-party DOS runtime is implied or bundled.

SOURCE / VERIFICATION
Complete corresponding source and original notices are under SOURCE and in the
matching engineering ZIP. This ZIP has SHA256SUMS.txt; Verify-Package.ps1 is an
optional read-only checker for the untouched STAGING folder. If execution policy
blocks it, use Get-FileHash for the four BIN files; do not change policy. Source
and compiler/bootstrap fixtures are not owner runtime payloads. No owner games,
credentials, databases, compiler/DDK media or AV vendor binaries are included.
