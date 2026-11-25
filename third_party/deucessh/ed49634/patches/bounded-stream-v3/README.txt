Section16 targeted follow-up to the frozen bounded-stream-v2 overlay.
The matching three headers/channel source remain byte-exact v2 inputs.
ssh-trans.c adds a finite occupied global-reply/TX-slot wait (existing timeout,
capped at75 seconds) and a total retained pre-peer-KEX queue ceiling of1024
messages/16MiB, including packets retained across consecutive rekeys.
The incremental bounded-stream.patch is against the preserved v2 ssh-trans.c.
The v2 provenance/patch and upstream archive remain preserved unchanged.
Accepted16MiB receive credit,64KiB rings, non-waiting channel writes, packet
encryption ownership, normal rekey replay and cancellation remain unchanged.
Source change is not qualification; see the current Section16 test evidence.
