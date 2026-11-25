# Spitfire-X BBS — Display and Menu System

## Live definitions (owner-approved 2026-09-13)

Future generated menus reflect saved valid definitions and current caller access
on the next relevant display/refresh while callers remain online. A number in
an already displayed menu must never silently launch a different resource after
an edit: retain a coherent selection context or redisplay and request selection
again. Active operations retain their settings, with explicit affected-operation
deferral where needed. Do not rewrite optional Sysop artwork automatically.
See [the live-configuration decision](../DECISIONS.md); this is a future menu
stage obligation, not menu implementation in the R1 live-JOKER correction.

## Behavioral rule

Preserve observable OGSF display, menu, prompt, navigation, novice/expert, hot-key, security-visibility, and caller-interaction behavior unless an explicit SFX deviation is registered.

Changing the native file format or menu-definition storage is not permission to redesign the caller experience.

## Reserved system-notice area - owner clarification October26

For menu-delivered system notices, including caller logon/logoff events, use
SFX's existing reserved event area between the menu artwork and the current
time/session-status lines and command prompt. Preserve surrounding spacing;
do not add padding rows or shift the footer. Present only at a safe boundary,
preserving typed input and caller context. Retain ANSI/plain-text behavior and
Sysop artwork. This is the owner's placement clarification for Section12,
not an instruction to alter menu art or implement/retest product code now.
See `ONLINE_CALLER_NOTIFICATIONS.md` and the Section12 notice Q&A record.

## Native display files

Runtime assets use a protected display-data root beneath `C:\SFX`. R2 does not invent its exact post-rename subpath; that path and transition remain assigned to R5/installer design.

Native formats:

- `.ANS` — ANSI/color
- `.ASC` — ASCII/non-color

**LOCKED native encoding:** Both formats are byte-authored IBM CP437 and permit the complete CP437 repertoire, including extended box-drawing and other high-bit glyphs. `.ASC` is non-ANSI, not seven-bit-only: raw ANSI escape sequences are prohibited. `.ANS` uses the same CP437 bytes and may additionally contain only SFX's reviewed safe ANSI subset. UTF-8 is not the native v1 asset format. Synchronet Ctrl-A codes are not adopted; SFX uses the bounded typed token and presentation-control system in `DISPLAY_CODES.md`.

Message bodies have a separate owner-confirmed v1 requirement to preserve and
display CP437/ANSI artwork, not merely use an ANSI reader/editor interface. See
[Message-body ANSI artwork](MESSAGE_ANSI_ART_REQUIREMENTS.md). That future
untrusted-content rendering profile must be qualified separately; this trusted
display-asset contract is not proof of implemented message-art support.

The OGSF `.CLR` / `.BBS` native convention is not carried forward.

## Selection model

Use an unsuffixed fallback plus an optional exact-security override.

Example:

```text
MAIN.ANS
MAIN10.ANS
```

For a security-level 10 caller, use `MAIN10.ANS` if it exists; otherwise use `MAIN.ANS`.

Exact-security suffixes correspond only to valid configured caller levels 1-999. Level 0 is invalid and has no caller override; the unsuffixed file is the generic fallback, not a level-0 display.

Apply the same rule to FILE, MSG, and other approved OGSF display/menu names. A Sysop is not required to maintain a separate display for every security level.

The behavior when neither the exact override nor generic fallback exists must be defined per display class during behavior/conformance work; do not guess a universal fallback.

## Phase 3A pre-login presentation — locked, portable workflow owner-accepted

SFX always shows its hardcoded Sysop/node/version system message. Optional valid
`SFPRELOG.ASC` comes first, followed by one blank row, that system message, one
blank row, and the ANSI question. It supplements, never replaces, the message.
It is deliberately ASCII because graphics has not yet been selected. Missing,
unreadable, invalid or unavailable prelog simply omits the optional artwork.
See `SECTION5_LOGIN_MENU_FOLLOWUP.md` for the superseding owner correction.

Every connection then asks `Use ANSI graphics? [Y/n]`. `ENTER`/`Y` selects ANSI and `N` selects ASCII for the active session only. The answer is asked again on the next connection, and future `[Y] Your Settings -> [G] Graphics` changes only the current session.

After successful login and acknowledgment of the welcome information, SFX may
display matching `WELCOME1.ANS`/`.ASC` through
`WELCOME99.ANS`/`.ASC` in numeric order, with gaps skipped (September28 owner
extension). ANSI uses ANS and ASCII uses ASC; there is no cross-format fallback.
Missing, unreadable or invalid files are skipped without a generated replacement.
The sequence is bounded by the existing aggregate source/output/include limits.
Each welcome clears before display, pauses for Enter, then clears afterward.
See `WELCOME_PLACEMENT.md` for the superseding owner correction. These welcome
displays never use security-level suffixes. This rule
does not establish fallback behavior for other display classes.

Phase 3A Windows 3 and 4 owner-accept this transport-independent selection/presentation workflow behind the trusted-display and terminal ports. Window 4 reuses the selected graphics mode for optional `NEWREG` and post-authentication `NEWUSER` in its deterministic portable workflow. After failed post-registration `SY:` resumes existing-caller entry, `NEWUSER` is entitled only when the account actually authenticated has the same stable `AccountId` returned by the registration commit; another valid account completes without that display. Real file/terminal/runtime adapters remain separately unauthorized.

## RIP graphics

RIP graphics are not supported in the initial SFX version. Initial native display support is ANSI and ASCII only.

RIP may be reconsidered as a deferred future enhancement. Do not imply current RIP compatibility.

## Display @-codes

The owner-approved SFX v1 display-code catalog and authoring contract are
authoritative in `DISPLAY_CODES.md`. Native trusted `.ANS/.ASC` artwork uses
readable `@TOKEN@` forms. SFX retains the 19 safe original Spitfire names and
adds the approved modern caller/account, system/session, message, file, Door,
Chat, and FTN/network values. Invisible OGSF single-byte control-character
aliases are historical evidence only and are not native v1 syntax.

The system supports simple bounded left/right/center/zero-pad width modifiers,
a small named set of duration/byte/date/time formats, the locked `@PAUSE@`
Enter behavior, OGSF-compatible paging/abort controls, caller-terminal BEL,
clear-screen, and bounded presentation-only `@INCLUDE:filename@`. ANSI color
and cursor artwork remains in `.ANS`; `.ASC` remains non-color CP437. Display
@-codes are value/presentation features, not a scripting language.

`@PASSWORD@` is prohibited and `@BIRTHDATE@` is not in the generic v1
catalog. Expansion occurs only in trusted Sysop-authored SFX display assets
and later explicitly approved trusted template classes. Message bodies,
uploads, imported descriptions, Chat/emote text, door output, FTN/network
content, and other caller/external material remain inert. Tokens cannot invoke
commands, scripts, modules, doors, helpers, external programs, arbitrary
filesystem reads, or unrestricted database/configuration access.

## Menus

The Door Menu-specific model in this section is owner-approved and closed. Its permanent completion record and verbatim Q&A source are preserved under `docs/design_history/`; the full runtime/configuration design is authoritative in `docs/DOORS_TRANSFERS.md`.

`.MNU` files are not a native SFX requirement. SFX will use a new native menu-definition mechanism managed through the approved configuration direction.

That mechanism must be capable of preserving applicable OGSF behavior, including:

- familiar command letters and prompts except where a documented difference applies;
- commands hidden or unavailable according to configured security;
- navigation between Main, Message, File, Door, Sysop, and related sections;
- novice/expert and hot-key behavior where retained;
- familiar `[Q]uit` and `[G]oodbye` meanings.

### Shared numbered light-bar rule

Every selectable light-bar interface shows a number beside each item wherever
stable/displayed numbered choices reasonably fit. Typing that displayed number,
including a multi-digit value such as 12, directly selects the item without
requiring arrow-key traversal. The same numbers remain available when light bars
are Off or unavailable, so light bars never create arrow-only navigation.

Where numbered selection does not fit an established interaction, SFX retains
the applicable direct tag/key workflow; numbering complements rather than
replaces tagged-file and similar controls. Exact multi-digit input buffering is
an implementation detail and must preserve the displayed-number behavior.

Door entries are intentionally numbered rather than limited to the old alphabetic door slots.
Each actual Door Entry uses one locked numeric security mode: Minimum Security
(`>=`, the default), Maximum Security (`<=`), or Exact Security (`=`).
Numerically ineligible entries are omitted from generated and custom-art Door
Menu choices, and authorization is repeated when an entry is selected/launched.
Every configured numeric Door Entry value and exact-security display suffix must use the locked caller-security domain 1-999; level 0 and out-of-range values are invalid.

Each actual Door Entry also has `Sysop Only: On/Off` in `SFXConfig.exe`, with Off
as the default. Off applies the numeric rule. On replaces it with SFX's global
configurable Sysop-status threshold and disables the numeric editor controls.
Non-Sysops do not see the entry, and status is rechecked at selection/launch;
security level 999 is the absolute domain maximum and shipped threshold default,
but not a hard-coded Sysop exception or Door Entry rule. Lowering the threshold
grants Sysop authority to every valid level at or above it through 999. SFX v1 has no individual-caller Door Entry allow/deny overrides or
hidden exception table. The same inspectable per-door policy controls menu
visibility and is rechecked at selection/launch.

SFX v1 has no day/time availability schedules on Door Entries or Door
Menus/submenus and performs no scheduled menu-tree hiding. Enabled/Disabled and
the effective security rule determine whether an actual Door Entry is shown.
OGSF-style system Scheduled Events are a separate subsystem, not Door Menu
availability controls.

SFX v1 has no per-door concurrent-user limit or related Busy/queue state that
hides or disables a Door Entry. Eligible callers may select the same door
simultaneously, subject only to overall SFX node/system availability. A door
program's own busy response remains door-owned behavior.

`SFXConfig.exe` manages this model through one coordinated **Doors** interface:
a Door Menu Hierarchy, Selected Menu Entries editor, and shared Door Catalog/
detail editor. The hierarchy exposes nesting and shared references; the entries
view exposes numbered Door/Submenu targets and order; the catalog owns each
permanent Door definition and lists all placements. Depth and circular-reference
validation is immediate. This organization was informed by owner-supplied
Synchronet screenshots but is an SFX-specific nested-menu design, not a copy
of Synchronet's flat sections or interface.

Copy Entry creates a new placement referencing the same permanent Door/Menu ID.
Duplicate Door and Duplicate Menu create distinct definitions and require new
unique IDs; duplicate IDs block save. Door duplication copies configuration
fields only and starts disabled/unplaced. Menu duplication copies placement
references without recursively duplicating targets and starts unattached. Neither
operation copies program/data directories or display artwork.

SFX v1 Door Menus use four selection-tree levels total. Main `[D]` always opens the level-1 root menu. Levels 2 and 3 are submenu displays; level 4 is the actual Door Entry leaf level inside the final submenu. The root and level-2 menus may mix Door Entries and Submenu Entries. A level-3 menu is final and may contain Door Entries only. Circular references are invalid.

Every configured door has a permanent Door ID, and reusable Door Entries target
that one shared definition. SFX supports at most 999 configured actual doors.
The configuration contains one root Door Menu plus up to 999 submenus with
permanent Menu IDs 001 through 999; a Menu ID may be referenced from multiple
parents when depth/cycle validation passes. Each menu contains at most 99 total
entries. Entries use stable Sysop-assigned menu-local numbers with gaps, display
in ascending number order, and are never automatically renumbered.

Each menu/submenu has a permanent internal Door Menu ID independent of location. Submenu Entries target those IDs, so display lookup and identity do not encode the path. Every menu has a generated/default display. Optional matching Sysop-created `.ANS/.ASC` art under the protected SFX display-data root is used when present; otherwise SFX uses the generated menu.

The locked convention is `DOOR.ANS/.ASC` for the root and `DR001.ANS/.ASC`
through `DR999.ANS/.ASC` for submenu IDs. Normal exact-security overrides apply,
such as `DR00110.ANS`; path-derived names are prohibited.

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

In this controlling example, `DOOR.ANS` is level 1, `DR001.ANS` is level 2, `DR004.ANS` is level 3, and the actual doors are level 4. There is no further submenu display.

Every Door Menu has a permanent internal ID and a separate human-readable,
Sysop-configurable Menu Name. Generated menus show the name in navigation labels
and display a breadcrumb such as `Door Menu > Sci-Fi > TW & Clones`.
`[B]` returns to the named parent, `[D]` opens the root Door Menu, `[Q]`
returns to the BBS Main Menu, and `[G]` invokes normal Goodbye/Logoff. Enter
moves back one level; at the root it returns to the BBS Main Menu. Redundant
destinations are hidden, including `[B]` and `[D]` at the root.

Each placement may override its title and optional short description without
altering its shared Door/Menu definition. Generated rows contain number and title
only. The shared light-bar system remains optional and direct number entry always
works. Menus longer than terminal height use automatic Next/Previous pages.

Disabled, invalid, security-ineligible, or otherwise unlaunchable entries are
hidden rather than displayed as Busy/Scheduled/Maintenance placeholders. An
empty root remains on its Door Menu with native commands available. Normal door
return restores the exact launching menu; SFX provides no post-door/welcome-back
display.

Every Door Menu level also provides fixed, reserved `[X] Door Bulletin Menu`.
It cannot be assigned to a door or submenu entry. The generated command prompt
shows `[X]` even when custom Door Menu `.ANS/.ASC` artwork is used. It opens the
global numbered Door Bulletin index on demand and returns to the exact invoking
menu. Bulletins are not automatically shown and are not a BBS Main Menu
command.

The on-demand rule applies to opening pages, not to new-content notification.
During normal returning-caller login, SFX reports enabled Door Bulletins whose
successfully validated/published content changed since the caller's previous
successful logon, alongside the normal bulletin/newsletter new-content stage.
The notice identifies permanent Bulletin IDs and configured titles but does not
automatically open them; callers use `[X]` to view the pages. Failed or invalid
source refreshes do not create an update notice. Exact login-screen wording and
composition remain for the login conformance pass.

SFX always generates the global Door Bulletin Menu/index. Optional custom
index artwork is `DBUL.ANS/.ASC` under the protected SFX display-data root; absence uses the
generated index. Sysop-created static bulletin pages use permanent Bulletin IDs
and `DB001.ANS/.ASC` through `DB999.ANS/.ASC`, with normal exact-security overrides
such as `DB00110.ANS`. Bulletin filenames do not encode Door Menu nesting and
are distinct from `DOOR`/`DR` menu displays and `DP` pre-door displays. SFX v1
supports up to 999 configured bulletin page definitions with permanent IDs 001
through 999. The permanent ID is also the caller selection number; gaps are
allowed, entries display in ascending ID order, and SFX never automatically
renumbers them or maintains a separate display-order field. Caller presentation
uses the separately resolved pagination model.

A Door Bulletin definition selects either a Sysop-created static DBnnn page or
a configured door-generated score/status source. Door-generated sources use a
permanent Door ID, a source path relative to that door's approved working
directory, and an explicit ANSI/ASCII interpretation. SFX rejects path escapes
and never serves the live file; only a validated, bounded, atomic
SFX-controlled snapshot reaches callers. Doors do not gain unrestricted write
access to the protected SFX display-data root or arbitrary SFX storage. If a configured enabled
source is missing, unreadable, invalid, or its update fails, its entry remains
visible and selection shows `This Door Bulletin is currently unavailable.`
before returning to the index. SFX serves no stale snapshot, logs and alerts
the Sysop safely, and resumes normal viewing automatically when a valid source
returns.

Every enabled bulletin is shown to every authenticated caller who can reach
`[X] Door Bulletin Menu`; it has no security, Sysop-only, schedule, or
per-caller restrictions and does not inherit access policy from an associated
door. Enabled/Disabled is an administrative publication switch, and disabled
bulletins are hidden from the generated index.

The generated bulletin index uses the shared numbered light-bar system and
always accepts a permanent Bulletin ID typed directly followed by Enter.
Long indexes page by terminal height with `[N]ext` and `[P]revious`. With light
bars in use, Enter opens the highlighted bulletin. Completion waits for Enter
and returns to the index. `[Q]` returns to the exact invoking Door Menu/submenu.
Optional `DBUL.ANS/.ASC` artwork is presentation-only and cannot remove these
controls.

`SFXConfig.exe` contains a built-in **Door Bulletins** section for Bulletin-ID,
title, administrative publication state, static/door-generated source mapping,
ANSI/ASCII interpretation, preview, validation/refresh, and status. It has no
caller-access fields. Removing a definition does not delete its source display
or door output file.

Optional per-door pre-launch displays live under the protected SFX display-data root and use the
permanent Door ID: `DP001.ANS/.ASC` through `DP999.ANS/.ASC`. They support the
normal exact-security suffix, such as `DP00110.ANS`. The applicable display is
shown immediately before launch and continues automatically. The locked
`@PAUSE@` token waits for Enter when included. A missing display causes no error
or extra prompt; the door launches normally. SFX provides no separate caller
command for reading a pre-door display without launching its door and no
per-door confirmation-prompt switch.

Exact native menu schema is unresolved and must not be invented during documentation or prototype work.

## Chat Lobby, Node Chat, and Page Sysop presentation

The locked functional design is in `CHAT_ROOMS_AND_CHAT.md`.

Main Menu `[C]` enters the stock native Lobby/Chat Room directly. Its live-room Chat Menu/Help shows `/R`, `/L`, `/W`, `/T`, `/?`, `/Q`, `/G` as owner-agreed slash commands after the later October26 removal of B Block / Ignore; reuse Your Settings N as Do Not Disturb, which blocks incoming private texts, private room whispers and Node Chat requests while leaving public room conversation and the separate logon/logoff notice preference alone. Remove the built-in/default U uMRC entry. There is no separate pre-entry picker or new H mapping. Optional Sysop-installed chat Doors use the planned shared menu-placement feature. A generated functional Lobby display is always available; optional `.ANS/.ASC` artwork under the protected SFX display-data root may replace presentation but not native navigation, access checks, notices or fallback. Exact Chat/Page/Local display basenames remain unresolved pending configuration design and later OGSF evidence.

ANSI-capable callers receive a full-screen scrolling conversation view with a dedicated input line. ASCII/limited terminals receive a functionally equivalent text view. Room and node lists follow the shared numbered rule and paginate to usable terminal height with Enter for more and Q to quit; a 128-node configuration must not distort the layout.

Room streams show join, leave, change, and disconnect notices. Saved BBS-wide caller login/logoff notices default On. Chat channel recording/auditing is excluded from v1.0b; do not add room recording indicators or controls.

Private Node Chat owner refinement October26: provide a built-in stock ANSI
border and optional Sysop `pchat.ans`/`pchat.asc` presentation files. Owner art is
optional, not required for the default. Live chat/header/input/help areas remain
SFX-owned; custom presentation cannot move them or interfere with input. Keep
CP437/safe-ANSI/protected-reading policy and functional built-in/plain fallback.
Owner approved October27 the stock border, optional pchat.ans/.asc files and
80x24/79x23-frame rules. See `PCHAT_DISPLAY_AUTHORING_RULES_APPROVED_20261027.txt`.
Coordinates/plain-mode rules are agreed design, not implemented or qualified
rendering. The October26 draft remains preserved unchanged. Existing chat.ans is a separate menu asset.
Preserve edits; future update README must distinguish shipped defaults,
optional/reference templates and runtime-generated changes. CR4 HOLD.

Who's On-line may show an accessible room name but uses safe generic Chat Lobby activity when the viewer is not eligible to know the room. Page Sysop and Local Chat unavailable/already-paged/unanswered/entry/completion states always have generated functional messages and may use optional `.ANS/.ASC` displays with fallback. Private Sysop Chat transcript capture is excluded from v1.0b; do not add capture controls or capture-state indicators.


## File Menu presentation

The locked File Menu design is in `FILE_AREAS_AND_TRANSFERS.md`.

Approved 8.3-compatible display bases under the protected SFX display-data root are:

- `FILE.ANS/.ASC` for the main File Menu;
- `FGRP.ANS/.ASC` for File Group selection; and
- `FAREA.ANS/.ASC` for File Area selection.

They use the normal exact-security suffix override and generated fallback. The
generated selector uses File Group -> File Area plus direct numeric Area entry.
File Menu status always identifies the current Group and Area. A reserved event
line above it carries safe live notices and compact post-transfer results.

Generated file lists and selectors use the reusable full-screen light-bar
presentation by default. The Sysop can disable light bars globally and configure
their colors with live preview. When enabled globally, callers may save On/Off;
they do not choose colors. New installations default On with the stock subdued
grayscale palette. ASCII/non-cursor sessions automatically receive a numbered
text alternative.

`[?]` offers only currently available File Menu commands and uses newly written,
configuration-aware SFX help. The separate OGSF `[H]` transfer tutorial is not
retained.

## Presentation architecture guardrail

Domain and application logic must not scatter raw socket writes, `printf`, `std::cout`, platform console operations, or hard-coded ANSI escape sequences throughout caller workflows. Caller interaction routes through a terminal/session-presentation boundary that can provide text/ANSI output, named-display rendering, screen/cursor control, input, prompts, paging, and menu rendering. Exact API types and method names are deferred.

This boundary serves native SFX first. It also permits a future, separately approved scripting adapter to expose a safe subset without granting transport, terminal, or runtime ownership to customization code.

## Internal command routing

Menus route to native application handlers through stable internal command identities or an equivalent registry rather than a monolithic switch tied directly to terminal/database/process code. Illustrative identities such as `main.messages`, `main.files`, `message.read`, or `file.download` are provisional and are not a public scripting API.

Stock handlers are canonical. If future approved customization observes, augments, or overrides a selected command, disabling that customization must restore the native handler without recompiling SFX. Core security visibility and point-of-action authorization apply before and during all routing and cannot be overridden.

The default display/menu package must remain recognizably Spitfire and must not require scripting to function. Extensibility may later let advanced Sysops alter selected caller-facing behavior, but SFX must not become a generic scripting host with a Spitfire skin.

## Update README stock display inventory - owner direction October26

Every authorized update README includes Display/menu changes with exact stock
ANS/ASC filenames, visible changes, actual install-relative destinations and
archive locations for loose reference files in Display-Updates/. Compare actual
shipping bytes with the prior delivered package. Distinguish runtime-generated
screens and state when no stock displays changed. Sysops can extract/edit/merge
stock references; installed custom ANS/ASC files stay untouched. For current
customization instructions, see the [customization reference](sysop/customization-reference.html).
This is a future packaging/review rule, not a build/package/live-copy GO.
