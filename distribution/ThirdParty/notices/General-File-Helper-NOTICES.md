# Strict helper review source — notices and lineage

## October 21, 2026 YMODEM-G upload startup correction

The general-file G receiver now repeats its G request, within the existing
eight-attempt bound, while no header or first data block has begun. This permits
manual client file selection and the Synchronet sender's post-header cancellation
poll consuming an already-arrived G. Partial frames, CRC/sequence errors and
timeouts after streaming begins remain fatal. No sender/download, ZMODEM,
window, path, capability, receipt, deadline or cancellation policy changes.
Earlier delivered helper/source/package snapshots remain preserved. GPL-2.0-or-
later terms and upstream notices remain unchanged. Qualification is recorded
separately; this notice does not claim owner SyncTERM acceptance.

## October 18 finite receive-buffer compatibility candidate

Exact new fork of the 83 materials bound by section9-helper-file/r8. That
original source, executable, manifest and every earlier delivered ZIP remain
unchanged. The only executable behavior change advertises an 8192-byte receive
buffer in standard ZRINIT for general-file uploads. This bounds each sender
frame before acknowledgement and leaves room for worst-case escaping within
the existing 64 KiB SSH channel. Send direction, GPL process boundary, native
handle/manifest/receipt admission, CRC/EOF, cancellation, timeout and size caps
are unchanged. Original notices below describe prior lineage, not a new DR
approval. Current direct owner GO requires testing and exact matched materials;
no DR review is authorized. Not qualified until new evidence establishes it.

## October 13 separate packet-only development candidate

This tree was copied hash-exact from the approved completed-empty-batch helper
materials before the new fork edits. Original 256 KiB helper/source/materials
remain unchanged under ../section8b-helper-empty-batch. This candidate is a
separate executable from the same pinned licensed source, not a replacement or
a library linked into SFX. It is NOT qualified, activated or packaged yet.
Packet receive/send are exact separate entry points; generic CLI, INI and
environment discovery are unreachable. Receive permits REP/ZIP metadata only,
uses fixed payload.rep and the approved 16 MiB cap. Send reads only protected
fixed payload.qwk, disallows initial resume/input purge, binds full EOF and
ZRINIT acknowledgement, and requires bounded CRC-valid ZFIN plus actually
written OO. A remote skip is never delivery. Both directions count input/output
bytes for independent host accounting and keep existing 60s/300s deadlines,
8192-byte subpacket arrays and fixed/bounded ACK window. Prior notices below
are retained as lineage only; they do not require a new DR audit or prove new
packet-helper qualification. Current GO requires CR qualification, exact source/
binary/GPL materials and integration tests before shipping this candidate.

## October 9 completed-empty-batch candidate (not activated)

The reviewed client-compat helper and its75 materials remain byte-exact in
../section8b-helper-client-compat. This separate candidate adds profile4 reason3:
attempted0/completed0/bytes0/zeof0/batch1/failure0, no payload and an S0 DSZ row.
It never labels an empty batch as a file or authorizes posting. Initial consumed
bytes are recorded without additional reads and reparsed only on initial ZFIN:
optional exact rz CR or CRLF startup, zero to8 CRC-valid zero-field ZRQINITs,
one CRC-valid zero-field ZFIN, followed live by exact OO. Prefix256 bytes and
per-header32 raw/four flow-control limits prevent skipped junk becoming success.
No scanner restart, arbitrary drain, timeout extension or retry sleep is added.
Ordinary file receive and refused-offer paths keep their original guards; only
receipt version changes for those paths. The common bounded header parser is
also used by the pre-existing strict refusal wrapper, still requiring ZFIN.
The host must independently validate full output, exact owned file set, DSZ,
input ledger/tail, fresh authority and deadlines before restoring editor input.
This is GPL isolated helper preparation; no GPL code is linked/copied into SFX.
Exact independent source/binary review is required before shipping activation.

## October 9 strict refusal-end correction after DR-POLICY-001

The preceding policy candidate remains unchanged in ../section8b-helper-policy.
DR withheld that candidate because its general header scanner discarded arbitrary
prefix bytes before a valid ZFIN. External-process negative controls reproduce
failure0 refusal receipts after injected Q for extension and size refusals.
This separate candidate replaces only the strict refusal header receive call
with a fixed 32-raw-byte parser: CRC16/CRC32 binary or CRC16 hex, one/two ZPAD,
legal binary escapes and hex LF/CRLF (including parity terminators). At most
four XON/XOFF bytes, including metadata ZCRCW's final XON, are permitted outside
escape pairs. These are protocol flow control, never arbitrary discarded data.
No resynchronization, retry, sleep, post-header drain or new deadline exists.
Literal OO is still mandatory after the response. Profile3 is unchanged.
Ordinary upstream zmodem.c and successful payload receive behavior are unchanged.
Only this candidate's strict receiver/provenance/test/build/notices differ from
the withheld candidate. It needs its own exact review before any activation.

## October 9 separate extension/size policy candidate

Owner-authorized bounded correction, NOT approved for activation or shipping.
The delivered completion helper and all74 materials remain unchanged under
`../section8b-helper-followup`. Profile3 adds case-insensitive final extensions
ans/asc/txt and a bounded ZSKIP/ZFIN/OO refusal for an unsupported extension or
an announced size above 262144 bytes. No payload is created for that refusal.
Malformed metadata, extra files, missing completion, timeout, cancellation and
actual-data overruns remain failures. Existing content validation stays in SFX.
Version3 receipts distinguish refusal from completed payload and retain exact
input consumption. The CLI remains the strict `--sfx-message-v1` entry point.
No host may mix receipt profiles or accept exit0/reset alone as synchronization.

Only the separate helper's strict profile, receiver, state header and provenance
headers change from the delivered source; upstream zmodem.c stays exact to that
delivered source. GPL isolated tests/proof and build/receipt scripts are adapted
for the candidate. These are not SFX application code or installed qualification.
Complete candidate source, licenses, notices, patches and twice-built identities
must accompany exact review. Earlier modification history below is retained.

## October 9 separate completion-accounting candidate

Owner-authorized bounded bug-correction preparation; NOT approved for activation
or shipping. The original delivered helper and all73 materials remain unchanged
under `../section8b-helper`. This separate tree retains every upstream notice,
license and original strict guard. Only the strict receive addition gains an
input-byte counter and version2 completion receipt; CLI remains the same strict
`--sfx-message-v1` entry, but old and new receipt profiles must never be mixed.
Provenance headers identify the new candidate. New GPL helper proof material
`host-completion-review.hpp` tests a proposed host-owned ledger predicate, not
shipping SFX integration; no helper implementation is linked into SFX.
Build and receipt scripts use distinct followup output/evidence roots. The review
must identify complete corresponding source/patch/materials and twice-built exact
binary before any shipping pin is changed. Earlier provenance below is retained.

This folder is the review source for a **separate executable**, based on the
official Synchronet mirror revision d83fbe2a41580e37314ea671bb54fd9fedb338cd.
It is not linked into SFX and is not an owner-test/shipping distribution.
No legal-clearance claim is made. The initial checkpoint records the source
and individual license audit, including the complete input inventory.

Retain every original file header. Upstream copyright/credits include Rob
Swindell, Stephen Hurd, Jacques Mattheij, XPDEV contributors, RSA Data Security
and Steve Reid. Full grants/conditions and disclaimers travel with their files;
this summary does not replace them.

- The complete SEXYZ executable is treated as GPL-2.0-or-later. Upstream SEXYZ,
  XMODEM and supporting sbbs3 files explicitly carry that grant.
- XPDEV/hash/SMB support has the individually inventoried LGPL-2.0-or-later
  grants. Transitive SMB headers are included; no SMB implementation is linked.
- Original zmodem.c/.h have the retained two-clause BSD grant and Mattheij
  credit/rights-transfer history. Do not remove or relabel that original grant.
- md5.h contains RSA attribution/use/derivative conditions; sha1.h declares
  Steve Reid public domain. Neither algorithm implementation is compiled here.
- dllexport.h/text.h and strwrap.h/xpbeep.h lack individual license grants;
  the pinned upstream copyright statement covers the relevant GPL/LGPL project
  group. Preserve that statement; do not pretend these are file-level grants.
- New strict-profile files and the strict-receiver additions are supplied
  under GPL-2.0-or-later, with all upstream notices retained. This is confined
  to the separate helper, not a grant to link it into proprietary SFX code.

Full official GPL-2.0 and LGPL-2.0 texts and the upstream copyright snapshot
are in `licenses/`. No SFX terms may restrict helper source/GPL rights.
Any later approved binary distribution must include/offer these exact source
materials, required build scripts/headers, modifications, notices and manifest
alongside the matched binary. A moving upstream URL is not corresponding source.

## Modification record — October 8, 2026

Three original files changed: sexyz.c, zmodem.c, zmodem.h. Two new files:
sfx_message_profile.h and sfx_message_receive.inc. Modifications add a dedicated
Win32 `--sfx-message-v1` entry point, bounded original metadata validation,
persistent single-file/size/completion/cancel failures, create-new fixed target,
no INI/environment/purge behavior, clock bounds and structured private result.
No artwork fixture or SFX application code is copied into this helper.

Fixed GIT headers identify this as a modified review candidate, not pristine
upstream. `build.ps1` records v143 Win32/x86 /MT /Brepro and exact input hashes.
`protocol-tests.cpp` is GPL isolated helper test material, not a product component.
The patch/manifest receipt and report identify exact builds and qualification
limits. Future edits invalidate their identity and require requalification.

## Modification record — October 9 client compatibility candidate

The separate strict receiver now accepts the ZCRESUM conversion offer in
addition to unspecified and binary offers. This is capability compatibility,
not partial-file resume: the owned fixed payload is still created exclusively
and reception always starts with ZRPOS zero. All size, extension, metadata,
CRC, single-file, exact completion, clock and private receipt rules are retained.
Text conversion and nonzero transport/reserved flags remain rejected. The exact
NetRunner trace motivates this change; IcyTerm has not been physically qualified.
The protocol tests add old-helper negative controls and fresh-file compatibility
regressions. SFX source and shipping pins are unchanged pending exact review.
# October19 general-file private IPC performance fork

The file-only entry point uses two fixed 4096-byte private socket buffers,
flushed at protocol boundaries/before reads and waits/before the receipt.
Receive accounting counts consumed octets, send accounting counts actually
sent octets. No extra drain, payload paths, protocol change, larger receive
credit, weakened CRC/EOF/caps/cancellation or receipt format change. The
standard 8192-byte receive credit remains. Old helper/source and the separate
message/QWK binaries remain unchanged. This separate helper retains the
existing GPL-2.0-or-later grant; no GPL helper code is linked into SFX.

# General-file sender credit-wait correction

After a CRC-valid ACK actually frees transmit credit, the general-file sender
polls additional ACKs without an unnecessary one-second wait. Duplicate ACKs
which leave the window full retain the blocking wait. ACK range/ordering,
16KiB TX limit, 8KiB receive credit, CRC/EOF/final handshake and receipt guards
are unchanged. Deterministic sender-loop model tests assert both branches;
actual-process protocol qualification remains separate. Other helper binaries
and all earlier helper/source forks stay unchanged.

# October 21, 2026 Section10 general-file protocols

This separately supervised executable adds a strict XMODEM/YMODEM/YMODEM-G
file-job coordinator in sfx_xy_transfer.inc, under GPL-2.0-or-later. Existing
ZMODEM job behavior and bounded transport remain. The host owns every payload
handle, filename mapping, authorization, limit and final accounting decision.
No helper code is linked into SFX. The message and QWK helper binaries and
sources remain separate and unchanged. All original grants below are retained.

The new profile has an explicit protocol in its version2 completion receipt;
the accepted ZMODEM profile remains version1. Modifications also touch the
strict entry dispatcher, file coordinator and reproducible build driver.
Exact corresponding source, this notice, original licenses and build manifest
must accompany the matched binary in the optional engineering/source companion.
The lean package identifies that companion and its exact SHA256. Historical
candidate/review wording below records provenance, not the current shipping GO.

