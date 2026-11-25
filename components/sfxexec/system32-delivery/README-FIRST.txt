SFXEXEC SYSTEM32 COMPONENT TEST 20261024-r2
READY FOR DR REVIEW ONLY. Do not place files or start native tests until that
review releases the exact instructions. This is a candidate, not a verified
native fix. No full SFX Setup is included. Win10/32 is the first native target.

WHAT CHANGES
Keep SFXEXEC.DLL unchanged, but select its verified native Windows system-directory
copy. SFXDOOR.EXE registers the exact basename once using the retained NTVDM
convention. No private-path/probe/alternate-driver retry occurs. SFXDOSHost.exe
resolves Windows' system directory, pins that exact file and the matched launcher,
checks the actual loaded path/hash/file ID and the owned VDM's module inventory,
then sends a session/node-bound approval. The launcher consumes it before installing
game handlers/spawning the game. The caller handshake waits for its matching ack.
Wrong copies, old launchers, failed identities and missing approval fail closed.
SFX's private32hex session+actual node names/credits/ownership remain unchanged.

FILES TO CHANGE, AFTER DR REVIEW
1. Verify the ZIP's independent outer SHA256 and run Verify-Package.ps1 from this
   extraction. BIN-IDENTITY.txt lists the three exact component hashes.
2. Use PLACEMENT-AND-ROLLBACK.txt for the ONLY proposed system file:
      native System32\SFXEXEC.DLL (source here: SYSTEM32\SFXEXEC.DLL).
   Exact existing copy: reuse and record pre-existence. Different copy: STOP.
   This new host does not require/check SBBSEXEC.DLL or another board's files.
3. Preserve your existing D:\SFXDOS\BIN as a NEW unused sibling, for example
   D:\SFXDOS\BIN-before-system32-r2. Do not overwrite any prior preserved BIN.
   Create D:\SFXDOS\BIN and copy ONLY these two files from this new BIN:
      SFXDOOR.EXE
      SFXDOSHost.exe
   Do not leave the old SFXEXEC.DLL beside the launcher: the host refuses it.
   Old SFXLDR.DLL stays in the preserved old BIN; no new probe file is needed.
   Keep profiles, games, Start-SFGuess.cmd, LOGS and NODE results unchanged.
   Record the preserved BIN path with your placement receipt.
4. Close the elevated placement window. Start your existing SF Guess script in
   the same ordinary user/elevation context as Runtime. Keep your existing test
   alias, exact Door name and local command:
      rlogin 127.0.0.1:15132 -q -P -t sfxdos/sfguess
   No -p/-h/-H password mode is needed for this private test bridge.
   Existing SFX root D:\SFX, game directory D:\SFXDOS\GAMES\guess,
   SFGUESS.EXE, UART and fresh SFDOORS.DAT placement remain unchanged.
5. Enter SF Guess ONCE. Expect intended System32 path, BOP registration success,
   pair/open, DRIVER actual path/hash, System32 file/module/pair VERIFIED,
   host approval accepted, ACTIVE with actual node, game display/input and normal
   return to the same SFX menu. AX1 is a BOP result, not a Win32 error code.
   At any failure: stop this attempt, retain the ONE new LOGS file and observed
   menu return; report the exact stage. Do not make an automatic second experiment.

ONLY AFTER SF GUESS PASSES
Use OWNER-TEST-CHECKLIST.txt for Outpost and designated same-node SFX/Sync tests.
Do not force a free production node by renumbering/restarting/disconnecting callers.
Keep independent game copies/state and obey each game's concurrency settings.
Game whole-caller DTR hangup remains CanDisconnect=false. Operator Terminate Door
versus Disconnect Caller remain separate; their native effects must be observed.

RETURN TO PRIOR CANDIDATE
Stop designated SFX test hosts/owned VDMs normally. Preserve this new two-file BIN
under a new unused name; restore the recorded old BIN name to BIN. Keep all ZIPs,
profiles/games/logs/results. A System32 file is removed only by the exact receipt-
bound rollback conditions in PLACEMENT-AND-ROLLBACK.txt; pre-existing files remain.
The prior r4 still has its known failed native registration; rollback is recovery
of that prior selection, not a claim it fixes loading. No SFX/Sync data rollback.

WINDOWS LIMITS / SOURCE
Win7 SP1 x86 compatibility floor retained; actual native qualification PENDING.
Win10 x86 native loading, game behavior and same-node coexistence PENDING.
Win7/10 x64 and Win11: native DOS candidate refuses before installed-board/listener
access. No System32 placement on x64 and no third-party DOS runtime installation.
Complete corresponding source, original notices and exact r2/r4 source baselines
are in SOURCE. See SYSTEM32-REBUILD.txt there; older recipes are retained history.
Neither compiler/DDK/SDK/emulator media nor games/accounts/dropfiles/raw owner
evidence/DBs/credentials are included. R246 starter categories are not included.
