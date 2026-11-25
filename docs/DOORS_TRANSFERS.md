# Spitfire-X BBS — Doors and Transfers

## Fresh-install starter categories (owner-approved 2026-10-24)

Completing first-run setup on an untouched new board creates these empty
categories beneath Door Menu, in this order:

1. Main
2. BBS Transporter
3. Utilities & Tools

Each category requires at least security level **10** for both Access and
Execution, with Sysop Only unchecked. The root Door Menu is unchanged. These
defaults create no Door definitions, commands, remote targets or credentials.
Category creation is part of the first-run completion transaction; interruption
cannot leave partially created defaults. Upgrades and ordinary restarts preserve
existing boards, including an intentionally empty Door Menu. Any Door
customization made before first-run completion is also preserved.

## Live configuration requirement (owner-approved 2026-09-13)

When Doors are implemented, a saved valid Door must become available to eligible
online callers at their next Door Menu display/refresh, without reconnecting or
waiting for all nodes to empty. An active Door retains its coherent launch
settings; later launches use the revised definition. Disabling/removing a Door
prevents new launches without implicitly terminating its current process or
deleting files. Any deferral is limited to the affected operation and visibly
explained. See [the live-configuration decision](../DECISIONS.md).
This records a future owning-stage requirement; the R1 correction implements
live JOKER only and does not implement Doors or transfers.

## Status and authority

**Status: LOCKED DOOR DESIGN — IMPLEMENTATION NOT AUTHORIZED**

The project owner completed and locked all 45 items in the Detailed Door Menu / door-runtime Q&A round on August 12, 2026. This document is the authoritative SFX door/runtime design produced by that round.

Supporting design history:

- `docs/design_history/SPITFIRE_NG_DETAILED_DOOR_MENU_QA.md` is the permanent completion record.
- `docs/design_history/SPITFIRE_NG_DETAILED_DOOR_MENU_QA_SOURCE.txt` preserves the completed Q&A verbatim.

The transfer portions of this document defer to `docs/FILE_AREAS_AND_TRANSFERS.md`. Neither completed design round authorizes Phase 2 or production implementation.

## Behavioral rule

Preserve applicable OGSF caller-facing door and transfer workflows unless an intentional SFX deviation is registered. Modern process, transport, and configuration mechanisms must not silently redesign the interaction.

## Door setup priority and drop-file location (owner-approved 2026-10-22)

The owner reports more than 450 installed Doors on X-Bit BBS and identifies
Door games as the primary BBS focus. SFX Door setup must be simple, comfortable
for a large collection and closely follow familiar Synchronet options. Use
plain Sysop labels and sensible defaults. SFX owns node-directory creation,
node-number substitution and drop-file generation/placement; routine setup
must not require extra Sysop-written plumbing for those tasks.

Each Door has a **Place Drop File In** setting with these three choices:

- **Node Directory** (default): the current caller's logical node directory,
  `<InstallRoot>\Node\<number>`.
- **Start-up Directory**: the Door's configured startup/working directory.
- **Temporary Directory**: a private SFX-managed directory for that launch.

The choice belongs to the shared Door definition, independently of its menu
placements. Preserve the configured startup directory and selected placement;
do not silently move the file or change the working directory. Refresh the
selected drop file before launch and protect against stale data or another
caller's file. Temporary cleanup is limited to the completed launch's owned
files. Per-caller safety belongs in the implementation and must not make the
ordinary setup workflow clunky or introduce unapproved concurrency rules.

The initial owner test Doors are Outpost Trader (16-bit FOSSIL/UART,
52-line DOOR.SYS in the node directory), SF Guess a Number (16-bit direct UART,
SFDOORS.DAT in the startup directory), and Daily Blocks (native Windows 32-bit,
socket I/O, DOOR32.SYS in the node directory). SFDOORS.DAT is an additional
required compatibility format for the selected SF Guess Door. The owner's
Daily Blocks reference command is `dblockswin32 -N%# -Dd:\sbbs\node%#\door32.sys`;
SFX resolves its own selected installation root rather than hard-coding D:\sbbs.

See the [owner setup screenshots and preserved samples](../research/architecture/evidence/section11-door-owner-inputs-20261022/OWNER_INPUTS_AND_QA.txt)
and [three location choices](../research/architecture/evidence/section11-door-owner-inputs-20261022/screenshots/drop-file-location-options-synchronet.png).
This records Section 11 design decisions; implementation remains on HOLD.

## Per-node drop-file directories and node-number substitution

**OWNER-APPROVED FUTURE REQUIREMENT — September 21, 2026.** This is Section 11
Door-stage design, explicitly unrelated to the current Section 3 build.
It authorizes no Door implementation or installation-tree change now.

Keep node directories beneath one singular `Node` directory:

~~~text
<InstallRoot>\Node\1\
<InstallRoot>\Node\2\
...
<InstallRoot>\Node\<configured-node-count>\
~~~

Numbers are the existing logical node IDs (1..128 maximum), not account IDs,
Door IDs or process IDs. Do not create `Node1`, `Node2`, etc. in the SFX root.
Door executables/shared game data remain in their configured Door directory.
Default Node Directory placement writes the caller/session drop file beneath
that node. An explicitly selected Start-up Directory or Temporary Directory
uses the approved per-Door location instead; the configured node directories
remain managed by SFX in all cases.

Support `%#` in configured Door launch arguments as a BBS-owned substitution
for the launching caller's logical node number, expanded before process launch.
The Door owns its own switches. The owner's illustrative OTrader command is:

~~~text
otrader /dD:\SFX\Node\%#
~~~

On node 1 the resulting argument is `/dD:\SFX\Node\1`; on node 2 it is
`/dD:\SFX\Node\2`. If that Door's documented `/d` takes a directory, it can read
its configured drop file, for example `DOOR.SYS`, from there. This is not a
verified OTrader launch recipe or a claim that every Door uses `/d`; validate
each Door's documented argument syntax and selected drop-file format.

Persistent SFX-managed in-tree executable, working, drop-file, support and
runtime-profile paths remain **root-relative**. Resolve the actual selected
installation root at launch; the example does not hard-code `D:\SFX` into the
portable configuration contract. Use validated argument-vector construction,
not unrestricted shell expansion. Quoting and any DOS-runtime path mapping
belong to the platform adapter and require qualification with that runtime.

Generate or refresh the selected supported drop file immediately before Door
launch so caller identity, security and remaining time are current. Login-only
creation is not sufficient. Preserve minimal-secret contents and the existing
supported-format scope; this does not require emitting every format on login.
Bind the stable node directory to its current session, prevent another node or
later caller from seeing stale drop files, and finish supervised-process cleanup
before reusing it. A stable per-node path must retain private per-session
ownership/lifetime; it is not permission for uncontrolled shared scratch state.

The game must itself support multiple concurrent callers and safely manage its
shared game data. Separate drop-file directories alone cannot make a single-user
Door multi-user-safe. This does not add a separate SFX Door concurrency cap or
queue.

Future qualification covers two simultaneous nodes with distinct caller files,
node reuse without stale identity, a nondefault installation root, validated
arguments/path mapping and cleanup on return, disconnect, timeout and crash.
No such Door execution has been performed or claimed by this design record.

## Caller inactivity while in Doors

The owner-approved [AFK policy](SECTION3_OWNER_FOLLOWUP_REQUIREMENTS.md#4-configurable-caller-inactivity-timeout)
also applies to Door callers. Default total inactivity is five minutes;
authenticated Sysops are exempt under the configured Sysop-security threshold.
Unlike ordinary SFX screens, **Doors receive no injected four-minute warning**.
At timeout SFX disconnects and performs controlled Door/session cleanup even
if the game has no inactivity handling of its own.

Supported Door I/O adapters must reliably observe actual caller keyboard input
for this timer. Door output, animation, keepalives and protocol traffic do not
count. Do not claim this guarantee for an unobserved raw-socket handoff.
Enforcement and native/DOS/remote-path evidence remain with the later Door
implementation; no current Door support is implied.

## Transfers

The completed SEXYZ evaluation recommends **CONDITIONAL GO** as the provisional preferred transfer direction, only through a separately supervised, replaceable external process with a narrow socket/file/result contract. SFX does not link or copy SEXYZ code. Production use, bundling, and redistribution remain unapproved until an exact version/source closure is selected, the complete-helper license chain is clarified upstream, compliant notices/corresponding source are prepared, and Win32/x86 Windows 7-11 plus protocol/failure interoperability tests pass.

SFX retains authorization, generated batch manifests, upload quarantine, download allowlists, byte/time limits, socket-handle allowlisting, process supervision, result validation, accounting, disconnect/cancel behavior, cleanup, and version provenance. SEXYZ output and `DSZLOG` are untrusted results, not authority.

The caller-facing File Menu and transfer lifecycle are now locked in
`docs/FILE_AREAS_AND_TRANSFERS.md`. Protocol component availability remains
subject to the separate dependency, licensing, packaging, platform, and interoperability gates.

Key transfer locks are:

- session-only download and upload queues with parallel Batch menus;
- preflight authorization, availability, time, size, ratio, daily-limit, and
  approved-path checks before helper launch;
- direct start for a saved concrete protocol, with no redundant confirmation or
  silent fallback; `Select at time of transfer` prompts;
- normal caller time accounting without injecting warnings into an active
  protocol stream;
- structured completion/failure summary, success-only accounting, safe menu
  return, and per-batch optional automatic logoff;
- mandatory private upload staging and fail-closed validation;
- per-area Upload Requirement, Download Requirement, and Sysop Review policy;
  and
- optional external Sysop-installed ClamAV scanning, never bundled by SFX.

## Door configuration

- Door setup belongs in `SFXConfig.exe`, not BAT-file-heavy native configuration.
- A configured actual Door definition may launch a local native/DOS program or use SFX's native outbound RLogin gateway to a configured remote BBS, game server, or service. The remote path does not invoke `rlogin.exe` or another helper.
- Native Door Menu entries are numbered; familiar `[Q]uit` and `[G]oodbye` commands remain reserved.
- Every configured actual door has one permanent Door ID. Door Entries target that ID, and the same Door ID may be placed on multiple menus without duplicating launch/runtime configuration.
- SFX v1 supports at most 999 configured actual Door definitions installation-wide.
- The configuration contains one root Door Menu plus at most 999 submenu definitions, using permanent Menu IDs 001 through 999. The same Menu ID may be referenced from multiple parents when depth and cycle validation pass.
- Each Door Menu may contain at most 99 total Door/Submenu Entries. Every placement has a stable Sysop-assigned menu-local display number; gaps are allowed, entries display in ascending number order, and SFX never automatically renumbers them.
- Selecting `[D]` from Main always opens the top-level Door Menu.
- SFX v1 uses four selection-tree levels total. Level 1 is the root menu; levels 2 and 3 are submenu displays; level 4 is the actual Door Entries inside the final submenu. The root and level-2 menus may mix Door Entries and Submenu Entries. A level-3 menu may contain Door Entries only; there is no level-4 submenu display.
- Circular submenu references and invalid/dead target IDs are rejected.
- Every Door Menu/submenu has a permanent internal Menu ID independent of location. Submenu Entries target Menu IDs through configuration; paths are not encoded in filenames.
- Every menu has an automatically generated default display. Optional Sysop-created `.ANS/.ASC` art under the protected SFX display-data root overrides that menu's generated display when present.
- The locked custom display convention is `DOOR.ANS/.ASC` for the root and `DR001.ANS/.ASC` through `DR999.ANS/.ASC` for submenu IDs, with normal exact-security suffixes such as `DR00110.ANS`. Path-derived filenames are prohibited.
- A menu placement may override its caller-facing title and optional short description without changing the shared target definition. Generated menu rows contain the stable local number and title only.

Each actual Door Entry has one plain-language numeric security mode in
`SFXConfig.exe`: **Minimum Security** requires caller security `>=` the configured
value, **Maximum Security** requires `<=`, and **Exact Security** requires `=`.
Minimum Security is the default. A numerically ineligible Door Entry is hidden
from generated and custom-art Door Menus, and SFX repeats authorization at
selection/launch so custom artwork, stale input, or direct numeric entry cannot
bypass the rule. Every configured numeric security value must use the locked
caller-security domain 1-999; level 0 and out-of-range values are invalid.

Every actual Door Entry also has **Sysop Only: On/Off** in `SFXConfig.exe`,
defaulting to Off. When Off, its numeric security mode/value applies. When On,
Sysop status replaces the numeric rule and the editor disables the numeric
controls so the effective policy is unambiguous. Sysop status uses SFX's global
configurable Sysop-security threshold. Level 999 is the absolute domain maximum
and shipped threshold default, but is never hard-coded as a Sysop exception or
Door Entry rule. If the threshold is lowered, every valid level at or above it
through 999 receives Sysop authority. Non-Sysops do not see a Sysop Only entry,
and SFX rechecks status at selection/launch.

SFX v1 has no individual-caller allow/deny overrides for particular Door
Entries and no hidden per-caller exception table in `SFXConfig.exe`. Eligibility
uses inspectable per-door Enabled/Disabled state plus numeric security or Sysop
Only. The same effective policy controls visibility and is rechecked at
selection/launch. A future exception model would require a separate owner
decision and an explicit administration and audit design.

SFX v1 has no day/time availability schedule on actual Door Entries or Door
Menus/submenus. An enabled entry is available whenever its access rule permits;
a disabled entry is unavailable. `SFXConfig.exe` has no door schedule editor,
availability-window fields, or scheduled menu-tree hiding. OGSF Scheduled
Events—which execute system utilities/jobs at configured times—are a different
subsystem and remain a separate design area.

SFX v1 also has no per-door concurrent-user/instance limit, single-instance
switch, Busy policy, or waiting queue. Any number of eligible callers may use
the same configured door simultaneously, subject only to overall node/system
capacity and normal process/resource availability. Each caller retains normal
per-session process/drop-file isolation, but SFX does not serialize the door's
own shared data. The Sysop is responsible for installing/configuring door
software that safely supports the intended multi-node usage. A door may report
its own busy condition; SFX does not create one from a configured concurrency
cap.

OGSF's `SFDOOR.DOC` wording and supplied SFGUESS documentation support Greater
Than or Equal, Less Than or Equal, and Equal modes. A broader historical manual
example used strict less-than; SFX intentionally defines Maximum Security as
inclusive `<=` to keep configuration consistent and unambiguous.

### SFXConfig Doors setup

The current native **Doors** page starts with named categories and their direct
Door/category counts. Open a category to see the numbered choices callers see;
Main Door Menu contains the top-level entries. Existing number gaps remain.
Choose **Add Door**, **Edit / Open**, or **Add Existing Door**. Existing Doors and
destination menus are selected by name; stable internal identities and revisions
are not part of ordinary setup.

**Add/Edit Door** puts the name, enabled state, access and **Command Line** on
one scrolling page. Optional masked connection settings open under **Advanced
RLogin Settings**. The supported built-in command is `rlogin`, without a leading `?`:

```text
rlogin remote.example:5130 -q -t xtrn=GAME
rlogin remote.example:5130 -q -t xtrn_sec=MENU
```

SFX parses this bounded UI representation into its existing native RLogin target.
It does not execute a shell, script or external rlogin program. Hostnames,
numeric IPv4/IPv6, quoted names and port 513 by default are supported. Options
are `-q` quiet, `-P` no connection pause, `-c` client name, `-s` remote account,
`-t` terminal/game/menu selection, and `-T` connection timeout (1-120 seconds).
`-p` automatically forwards the successfully authenticated caller's current
login password; `-h` automatically derives the compatible hashed credential
before transmission. Plain `-p` remains plain. These
password flags cannot be combined with each other or client/server overrides.
Unsupported options, expressions and conflicting combinations retain the draft
and explain the problem beside the settings. They never silently fall back.

Password method, automatic caller credential or saved remote credential, salt
and protected pepper remain in optional Advanced settings. Secrets are masked and omitted from Command
Line; blank secret inputs keep the saved protected values when applicable, and
explicit removal checkboxes remove them. Automatic modes use a wiping,
account/verifier-bound session copy of the verified login password, with no
second local prompt and no reversible account-password store. A successful
password change replaces that copy; a stale proof refuses automatic launch;
connection cleanup clears it. Persistent account passwords remain Argon2 verifiers.
Configured/predefined hashing remains available through Password method and the
masked saved password. A blank salt uses the BBS QWK ID. The banner checkbox
applies when `-q` is absent. Existing quiet, pause, identity, terminal, timeout,
credential and pepper settings are preserved through the structured target.
Inline `-H password` and attached `-hPEPPER` remain unsupported in Command Line;
use the masked Advanced controls for those values. Native/loopback compatibility
does not imply new external-server or physical owner acceptance.

Shared Door settings apply wherever that Door appears. Its number, caller-facing
name and description belong to the selected menu entry. Access uses **At least
this security level**, **At most this security level**, **Exactly this security
level**, or **Sysop Only**, retaining the existing inclusive comparisons and BBS
Sysop-level policy. Editing through **All Configured Doors** changes only shared
settings and hides unrelated menu fields. Add there creates an unplaced Door;
place it later through a category's **Add Existing Door**.

**Save Changes** validates the proposed changes through the existing protected
configuration operations. Successful changes are available on the next caller
Door-menu refresh; a caller already inside a Door keeps captured settings. Errors
retain the draft. If shared settings saved but placement did not, the page says
so and keeps the remaining menu edits. **Check Save Result** checks the same
unconfirmed operation with editing frozen. A conflict retains the draft until
**Reload Latest Settings (discard edits)** explicitly reloads current settings.
Ordinary navigation retains the existing unsaved-change confirmation.

Management actions distinguish shared use, copying and deletion:

- **Add Category** selects its parent menu by name and its caller number.
  **Edit Category** changes the shared name. **Show Category on a Menu** adds a
  named placement subject to existing depth/cycle limits.
- **Show on Another Menu** shares the selected Door/category. **Edit Menu Entry**
  changes only its number, displayed name and description on the current menu.
- **Remove from This Menu** confirms removal of that entry only; the Door/category
  and other placements remain. Cancel is initially focused.
- **Duplicate Door** creates an independent disabled, unplaced configuration,
  including its protected settings. It copies no program/data directory, artwork
  or menu placement. Internal identity allocation is automatic.
- **Duplicate Category** creates an unplaced category with the same entry
  references, preserving shared Door/nested-category settings without recursively
  copying definitions or artwork.
- **All Configured Doors** includes unplaced and disabled Doors. **Delete Door
  Configuration** requires no remaining placements and confirms deletion of its
  settings and protected credentials. Active launches retain their captured
  settings. **Delete Category** requires an empty, unplaced category; the main
  Door Menu cannot be deleted.

The native theme and keyboard navigation follow the SFXConfig/SFXFileMan style;
long settings pages scroll at compact sizes and supported DPI scales. Existing
local Program/DOS definitions remain retained, but their setup/execution is held
pending the separate local Doors/SBBSEXEC decision. This UI correction does not
implement that engine or change the schema, native RLogin protocol, session
supervision, caller access, time accounting or return-menu rules. Earlier local
Door/drop-file design requirements elsewhere in this document remain future
scope rather than claims of implemented controls.

Controlling example:

```text
[D] Door Menu
      |
      +--> DOOR.ANS
             |
             +--> Sci-Fi       -> DR001.ANS
                    |
                    +--> TW & Clones -> DR004.ANS
                           |
                           +--> Trade Wars 2002
                           +--> Trade Wars V
                           +--> Outpost Trader
```

Here level 1 is `DOOR.ANS`, level 2 is `DR001.ANS`, level 3 is `DR004.ANS`, and level 4 is the actual doors inside `DR004`. No additional submenu level is allowed.

Every Door Menu has a permanent internal Menu ID plus a separate
Sysop-configurable human-readable Menu Name. Generated menus display a
human-readable breadcrumb such as `Door Menu > Sci-Fi > TW & Clones`.
Navigation is:

- `[B]` Back to the displayed parent Menu Name;
- `[D]` Main Door Menu;
- `[Q]` BBS Main Menu;
- `[G]` Goodbye and Logoff; and
- Enter performs Back one level, or returns to the BBS Main Menu when used at
  the root Door Menu.

Choices already at their destination are hidden; for example, `[B]` and
`[D]` are not shown at the root.

Generated Door Menus use the shared light-bar switch, palette, and caller
preference while always retaining direct numeric/plain-text operation. Menus
longer than the caller terminal height use automatic Next/Previous pages and
direct number entry. Generated rows contain entry number and title only.

Administratively disabled, invalid, security-ineligible, or otherwise
unlaunchable entries are hidden until available; SFX does not show Busy,
Scheduled, or Maintenance placeholder rows. If no root entries are available,
SFX still presents the empty root Door Menu with its native commands rather
than silently returning to Main.

After a door ends normally and return/cleanup checks complete, SFX returns the
caller directly to the exact menu/submenu from which it launched. There is no
global or per-door post-door/welcome-back display.

### Door Bulletins

SFX v1 retains a global Door Bulletins index with numbered/selectable bulletin
pages. The pages may contain door news, instructions, status, and
Sysop/door-generated scoreboards.

`[X] Door Bulletin Menu` is a fixed, reserved command at every Door Menu level.
It cannot be assigned to a door or submenu entry. The generated command prompt
shows `[X]` even when custom Door Menu `.ANS/.ASC` artwork is used. It opens the
global bulletin index on demand and returns to the exact menu/submenu that
invoked it. Door Bulletins are not a BBS Main Menu command and are never shown
automatically before a Door Menu.

That on-demand rule governs opening bulletin pages; it does not suppress the
classic new-content notice. During normal returning-caller login, Door
Bulletins participate in the same new-content-since-last-logon stage as normal
BBS bulletins and the SFX newsletter. If enabled Door Bulletins have
successfully published new content since the caller's previous successful
logon, SFX identifies their permanent Bulletin IDs and configured titles. The
notice does not open the pages; the caller still selects `[X] Door Bulletin
Menu` to view them. Only successfully validated/published revisions count, so
missing, unreadable, invalid, or failed source refreshes do not produce false
update notices. Exact prompt wording and screen composition remain part of the
later login conformance pass.

The owner-supplied OGSF examples confirmed the historical index-to-page model:
`SFDORBUL` listed numbered entries and `DORBUL1`/`DORBUL2`-style pages
contained door-game scores. They were reviewed as behavioral evidence only;
their content/artwork was not copied.

SFX always has a generated global Door Bulletin Menu/index. Optional custom
index artwork is `DBUL.ANS/.ASC` under the protected SFX display-data root; absence falls back to the
generated index. Sysop-created static bulletin pages use permanent Bulletin IDs
and the locked names `DB001.ANS/.ASC` through `DB999.ANS/.ASC` under
the protected SFX display-data root. Normal
exact-security overrides apply, such as `DB00110.ANS` for Bulletin ID 001 and
exact security level 10. These names do not encode Door Menu nesting and remain
distinct from `DOOR`/`DR` menu displays and `DP` pre-door displays. SFX v1
supports at most 999 configured Door Bulletin page definitions, using permanent
Bulletin IDs 001 through 999. A permanent Bulletin ID is also the caller
selection number: 1 selects DB001, 4 selects DB004, gaps are allowed, and the
index displays entries in ascending Bulletin-ID order. SFX never automatically
renumbers bulletins and has no separate bulletin display-number/order field.
This capacity uses the locked pagination/navigation behavior. Missing-source
handling and built-in SFXConfig management are locked below.

Door Bulletins support two source modes per definition: Sysop-created static
`DBnnn.ANS/.ASC`, or a configured door-generated score/status source. The
latter is expected to be a common primary use. It stores a permanent Door ID,
a path relative to that door's approved working directory, and explicit
ANSI/ASCII interpretation. A relative `SCORE.ANS` therefore resolves within
that approved working directory without hard-coding an installation path in
portable configuration.

Absolute source paths, traversal, and reparse/symlink escapes are rejected. A
door never gains unrestricted write access to the protected SFX display-data root or arbitrary
SFX storage. Callers never read a live door output file. SFX validates bounded
size, supported format, and terminal content, then exposes only an atomic,
SFX-controlled snapshot. Refresh timing is an engineering detail.

An enabled definition remains in the index when its source is missing,
unreadable, invalid, or an update fails. Selection shows exactly `This Door
Bulletin is currently unavailable.` and returns to the Door Bulletin Menu. SFX
does not serve a last known-good or stale snapshot. It logs the actual failure
and alerts the Sysop without exposing paths or internal details. Viewing resumes
automatically when a valid source returns; the administrative Disabled switch
remains the way to intentionally hide the definition.

Every enabled Door Bulletin is public to every authenticated caller who can
reach a Door Menu and select `[X] Door Bulletin Menu`. Bulletins have no minimum
security, Sysop-only, schedule, or per-caller allow/deny policy and do not
inherit restrictions from an associated door. A caller may therefore view a
public game scoreboard without permission to launch that game. Enabled/Disabled
is solely an administrative publication switch; disabled definitions are hidden.

The generated Door Bulletin Menu uses the shared light-bar interface when
available and always shows/accepts each permanent Bulletin ID as a direct caller
selection number. Type the number and press Enter; for example, 12 opens
Bulletin 012 / DB012. Long indexes use automatic terminal-height pages with
`[N]ext` and `[P]revious`. With light bars in use, Enter opens the highlighted
item. After a bulletin finishes displaying, SFX waits for Enter and returns to
the index. `[Q]` returns to the exact Door Menu/submenu that invoked `[X]`.
`DBUL.ANS/.ASC` changes presentation only and cannot remove these behaviors.

### SFXConfig Door Bulletins section

Door Bulletin configuration is built into `SFXConfig.exe` in a visible section
labeled **Door Bulletins**; it is not a separate program. Its list shows at
least permanent Bulletin ID, title, Enabled/Disabled, source type, and current
Healthy/Unavailable status. It provides Add, Edit, Enable, Disable, Preview,
Validate/Refresh, and confirmed Remove Definition actions.

The editor contains Bulletin ID, title, Enabled/Disabled, and source type.
Static definitions use locked `DBnnn.ANS/.ASC` lookup. Door-generated sources
also configure Door ID, relative source path, and ANSI/ASCII interpretation.
Preview/validation provides useful Sysop-only diagnostics without exposing
secrets, and no caller-access fields exist. Removing a definition never silently
deletes its static or door-generated source file. Definitions participate in
normal configuration validation, versioning, and backup.

### Optional pre-door displays

Every configured door may have an optional pre-launch display under the
protected SFX display-data root. Its stable 8.3-compatible base uses the permanent Door ID:
`DP001.ANS/.ASC` through `DP999.ANS/.ASC`. Normal exact-security overrides
apply, such as `DP00110.ANS` for Door ID 001 and exact security level 10.

SFX shows the applicable display after an eligible door is selected and before
launch. It then continues automatically. The locked `@PAUSE@` display token
waits for Enter when the Sysop includes it. If no applicable pre-door display
exists, launch proceeds normally without an error, substitute display, or extra
prompt. There is no separate caller action for reading this information without
launching the door and no separate per-door continuation setting.

Trusted Door Menu/pre-door artwork may use the locked `@DOORNAME@`,
`@DOORNUM@`, and `@DOORMENU@` context codes from `DISPLAY_CODES.md` when the
renderer has the corresponding authorized selection/menu context. Door program
output and door-generated bulletin snapshots remain untrusted and never expand
display codes.

- Door time always counts against normal caller BBS session time. SFX v1 has no per-door Free, multiplier, fixed-charge, or other time-policy override.
- SFX v1 has no per-door maximum minutes per run, daily door minutes, launches per session, or launches per day; normal overall caller/session and system limits apply.
- SFX has no spendable credit/currency economy and Door Entries never charge a per-launch credit/cost. File-transfer ratios/accounting are not spendable BBS credits.
- SFXConfig adds no separate per-door launch-confirmation switch. Optional pre-door displays and `@PAUSE@` provide the only door-specific warning/acknowledgement path before direct launch.
- Caller disconnect must be detected and the node recovered/cleaned up.

Target drop files:

- `DOOR.SYS`
- `DORINFOx.DEF`
- `CHAIN.TXT`
- `CALLINFO.BBS`
- `DOOR32.SYS`
- `SFDOORS.DAT` (owner-selected SF Guess a Number compatibility)

Support FOSSIL-aware and non-FOSSIL doors where practical.

## DOS Program Support

**Owner decision 2026-10-22:** keep the configurable **Support 16-bit Doors**
option available on both 32-bit and 64-bit Windows. Do not hide, gray out or
refuse the setting solely because Windows is64-bit. An x64 Sysop may enable
it when providing a compatible DOS runtime. Use one authoritative DOS-support
setting; this does not require duplicate controls or automatic enablement.

SFX does not bundle, install or promise a built-in x64 DOS virtual machine.
The owner explicitly excludes testing the third-party x64 DOS runtime route
from this phase. Keep that optional compatibility route available without
claiming it qualified; actual prerequisite/launch errors must be reported
plainly rather than treated as successful Door support.

The owner linked [WineVDM/OTVDM](https://github.com/otya128/winevdm) while recalling
the compatibility project's name. Its README primarily describes Win16 support
and says DOS emulation is incomplete. The reviewed Synchronet DOSXTRN source
specifically references NTVDMx64. These are different projects; recording the
option does not select WineVDM, NTVDMx64 or a version/build as a tested SFX
dependency. No x64 runtime installation/testing is authorized by this Q&A.

SFX remains responsible for validated configuration, correct supported drop files/environment, supervised launch, disconnect handling, time accounting, and cleanup within the approved integration boundary.

SVDM is excluded from SFX consideration. DOSXTRN, SBBSEXEC and the Virtual
UART/FOSSIL/VDD stack remain gated Windows DOS-door implementation candidates
behind a platform-specific adapter. They are NTVDM/VDD-specific; x64 support
still depends on a Sysop-supplied compatible runtime. The owner has selected
the 32-bit Windows deployment policy below. Exact source/build/licensing,
runtime/host interfaces and coexistence qualification remain to be completed;
this design decision is not an implementation or system-file mutation GO.

### 32-bit Windows DLL installation and production coexistence (2026-10-22)

Keep the original **SBBSEXEC.DLL** name. On 32-bit Windows installs using this
DOS support, SFX Setup installs the verified required DLL in the actual Windows
System32 directory and displays a brief plain-language note during installation.
The Sysop should not have to copy it manually afterward. Use the existing
ordinary Setup coordinator and restricted administrator-worker boundary.

If SBBSEXEC.DLL already exists there, bypass copying/replacement and leave the
existing file intact. Check its identity/compatibility without executing it as
an elevated discovery step. Existence alone is not a compatibility result: an
unsupported existing build must be reported plainly, never silently overwritten
or counted as ready. Do not rename it, alter its permissions or remove a shared
copy during SFX uninstall. No deployment to the owner's system is authorized
by this Q&A.

The owner will run SFX alongside production X-Bit Synchronet on the same host.
Simultaneous DOS Doors in both BBSes are therefore an explicit compatibility
requirement. Sharing a DLL file is supported by the Windows loader, but that
alone does not establish isolation of named driver resources.

Read-only review of upstream97fab8d84a68882d1ceff0bed7cc05497f4d5973 finds
SBBSEXEC channels named by node number, including mailslots wr%d/rd%d beneath
\\.\mailslot\sbbsexec and sbbsexec_hungup%d / sbbsexec_hangup%d / sbbsexec_carrier%d
events. Synchronet's xtrn.cpp uses matching names. Reusing those internal IDs
for both BBSes can cause collisions. This is a source-based integration finding,
not a reproduced failure or identification of the owner's installed DLL.

SFX must use collision-free driver communication identities/isolation while
retaining ordinary visible SFX node numbers and Node\<number> directories.
The exact mechanism is not selected here. The DOS launcher passes its driver
node value in a byte; a simple unlimited numeric offset must not be assumed.
Do not modify production Synchronet's DLL, configuration, files or node numbers
to accommodate SFX. Later approved qualification must cover concurrent Doors
with overlapping visible node numbers (including node1 in both BBSes), correct
caller I/O and independent cancellation/disconnect/cleanup. Simultaneous use
is not claimed proven until that implementation and qualification are complete.

[Exact reviewed upstream materials and hashes](../research/architecture/evidence/section11-door-owner-inputs-20261022/sbbsexec-coexistence-review/source-manifest.json)
are reference evidence only, not an approved shipping dependency pin.

## Control Panel behavior

While a caller is in an external door, the node view shows caller, door, time, and process/session status. Door-screen capture is not required.

The Control Panel provides separate confirmed **Terminate Door** and **Disconnect
Caller** actions. Terminate Door stops the supervised external process/session
without automatically disconnecting the caller and attempts safe recovery to
the BBS session and exact launching Door Menu. Disconnect Caller ends the BBS
session and safely terminates/cleans the associated supervised process tree.
Both actions are explicit and logged.

Closing the node-view tab must not terminate the logical node or door session
and invokes neither operator action.

Closing the full Control Panel is different. In default Desktop Mode, confirmed
Control Panel Exit initiates orderly shutdown of the entire Desktop-hosted BBS.
When callers and/or doors are active, the warning offers **Shutdown Now**,
**Graceful Shut Down**, or **Cancel**. Cancel leaves the BBS running. Graceful Shut Down
stops new calls and allows active doors/sessions only until a finite deadline;
the Sysop may escalate to Shutdown Now at any time. SFX does not inject BBS
warning text into an external door's I/O stream. A door that returns before the
deadline rejoins normal SFX presentation; a door still active at the deadline
has its supervised process tree terminated/cleaned before the associated caller
session is disconnected and finalized. In Service Mode, closing the Control
Panel detaches and leaves service-hosted door/runtime state active; explicit
Service Stop uses the same authoritative shutdown state machine.

## Security boundary

- Local doors and transfer helpers are untrusted external-process boundaries; remote RLogin targets and all bridged input are untrusted network boundaries.
- Do not grant direct database access by default.
- Minimize inherited handles, environment data, filesystem access, and privileges.
- Validate commands, arguments, working directories, drop-file paths, upload/download paths, and returned status.
- Do not place passwords or unnecessary secrets in drop files or environments.
- Log failures locally without exposing sensitive internals to callers.
- Exact isolation/sandboxing policy remains unresolved and must be reviewed before implementation.

The required platform adapter launches with an explicit executable/argument vector, private per-session directory, generated secret-minimized drop files, minimal environment and inherited handles, process-tree supervision, time/resource limits, structured exit status, and idempotent cleanup. Native, configured DOS, and future Linux runtimes must expose the same portable door-session behavior.
