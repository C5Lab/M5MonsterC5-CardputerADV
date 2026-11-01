# Deauth Detector on Cardputer ADV

Opening **Deauth Detector** shows four modes: **Passive**, **Trace**,
**Defence**, and **Defence + PMKID Lab**. Passive is the default. Press
**Space** to cycle, **Enter** to continue, or **Esc/Q** to leave without
starting JanOS. Passive and Trace may start immediately for all networks.
Defence modes never offer an all-network active start: Cardputer first sends
`stop`, waits for the exact final stop acknowledgement, performs a fresh scan,
and requires at least one selected BSSID. PMKID Lab presents a separate warning
that it can interfere with legitimate client joins and requires an additional
**Enter** confirmation.

The exact V2 commands are:

```text
deauth_detector_v2
deauth_detector_v2 trace
deauth_detector_v2 --only <indices>
deauth_detector_v2 --only <indices> trace
deauth_detector_v2 --only <indices> trace defence
deauth_detector_v2 --only <indices> trace defence pmkid-decoy
```

Cardputer never sends `--trace`, never places suffix tokens among scan indices,
and never adds active suffixes to the legacy `deauth_detector` command. The
selected mode remains in effect until this detector screen is closed.
After a successful V2 start, JanOS emits
`[DETECTOR_INFO] format=deauth_v2`. Cardputer shows the watch as active only
after the JanOS start reply and this format confirmation.

Press **S** to stop it and start a fresh scan for a specific-network watch.
After the scan completes, use **Up/Down** to move, **Space** to select one or
more APs, and **Enter** to start. The list shows each AP's scan number, SSID,
channel, and BSSID. An empty selection cannot start.

The selected watch uses the corresponding `--only` command above. Passive and
Trace keep their existing behavior; Defence always includes the explicit
`trace defence` suffix, and PMKID Lab adds `pmkid-decoy` last.
For Passive/Trace selected watches, Cardputer waits for the JanOS start reply,
the V2 format confirmation, and `[DETECTOR_WATCH]` lines confirming the selected
BSSIDs before showing the watch as active. Repeated scan rows for one BSSID
remain selectable by their original numbers; JanOS reports that BSSID once in
the watch list. Current Defence firmware instead emits its strict
`armed targets=<n>` record before the ordinary start and format lines and does
not repeat WATCH rows. ADV latches that early record only when its target count
and PMKID mode exactly match the fresh local selection, then still rejects all
later Defence telemetry whose BSSID is not in that selection. This count proof
does not independently echo each BSSID identity. Press **W** while watching to
inspect the local target list, or **S** to stop and scan again. **Esc/Q**
opens an exit confirmation; **Enter** confirms and **Esc/Q** cancels. After
confirmation, Cardputer sends `stop` (or waits for an already pending `stop`)
and leaves only after JanOS prints `All operations stopped.`. The earlier
`Deauth Detector stopped.` line does not
confirm that JanOS has finished stopping. A failed send or missing final reply
keeps the screen open and shows that stop is unconfirmed; **Esc/Q**, then
**Enter** retries. A failed or timed-out scan, rejected command, or missing
reply is shown as an error rather than a running detector.

Trace starts in the requested state and waits for JanOS to acknowledge that its
confirmation logic is ready. The complete status sequence is:

```text
[DETECTOR_INFO] format=deauth_v2
[DETECTOR_TRACE] armed
[DETECTOR_TRACE] confirming_bssid=AA:BB:CC:DD:EE:FF first_channel=1 window_ms=1000
[DEAUTH_V2] ...
[DEAUTH_V2] ...
[DETECTOR_TRACE] locked_channel=2
```

The format line confirms only the V2 record format. It neither arms Trace nor
confirms a channel. `armed` means JanOS is waiting for an eligible DEAUTH
observation. `confirming_bssid` identifies the claimed BSSID whose observations
JanOS is evaluating, the first observed channel, and the fixed one-second
confirmation window. Cardputer accepts that line only with a valid MAC,
`first_channel` from 1 through 255, and the exact `window_ms=1000` value.

The one-second channel-selection algorithm runs entirely in JanOS. Cardputer
only displays JanOS acknowledgements: it does not start a timer, choose a
channel, infer a lock from a DEAUTH record, or make CSA trigger Trace. One or
more V2 records may therefore update the normal DEAUTH counter and latest
observation while the UI still shows confirmation in progress. Only the
explicit `locked_channel` status changes the UI to locked. A successful tune
stops detector-initiated hopping, although an active Wi-Fi connection may still
control the physical radio channel.

For reference, JanOS performs the one-second selection as follows. On 2.4 GHz
it samples the first receive channel plus the numeric `first_channel +/- 2`
range. On 5 GHz it samples the previous, current, and next entries in the
detector channel list. Only frames claiming the BSSID named by
`confirming_bssid` contribute. JanOS ranks candidates by frame count, then
average RSSI, then the scan-cache channel, and finally the first receive
channel. A CSA `TO` channel never contributes to Trace selection. These are
JanOS decisions; Cardputer neither reproduces nor validates the algorithm.

If the final tune fails, JanOS reports:

```text
[DETECTOR_TRACE] lock_failed_channel=2 error=ESP_ERR_INVALID_STATE
```

Cardputer clears the in-progress confirmation, shows the failed channel and
error, and states that monitoring and hopping continue. A later valid
`confirming_bssid` line starts another attempt, which can subsequently fail or
lock successfully.

After 20 seconds without an accepted DEAUTH claiming the locked BSSID, JanOS
releases the channel and rearms Trace:

```text
[DETECTOR_TRACE] released_channel=2 reason=idle idle_ms=20000
[DETECTOR_TRACE] armed
```

Cardputer accepts the release only for the currently acknowledged lock, with a
channel from 1 through 255 and the exact `reason=idle idle_ms=20000` fields. It
clears the lock, shows the released channel, and reports that Trace is rearmed
and detector hopping continues. The following exact `armed` line is
idempotent, so the released-channel context can remain visible until a new
confirmation begins. Only a later `confirming_bssid` marker starts a new
attempt. Cardputer does not run a local idle timer and does not infer release
from the event stream. In JanOS, only an accepted DEAUTH claiming the locked
BSSID refreshes the 20-second deadline; a different BSSID, a malformed frame,
or any CSA announcement does not.

One leading JanOS prompt (`> ` or `>`) is accepted on every Trace status line.
The parser requires the remaining status syntax exactly. Malformed,
out-of-order, or unsolicited `[DETECTOR_TRACE]` lines are still consumed as
status lines: they never activate the detector, increment DEAUTH or CSA
counters, trigger an alarm, or replace either latest observation. Explicit
valid `locked_channel` and `lock_failed_channel` acknowledgements remain
accepted while a V2 Trace run is pending or active even if an older JanOS build
does not emit `armed` or `confirming_bssid`. Detector activation still
independently requires the JanOS start reply, V2 format confirmation, and, for
a selected watch, every required `[DETECTOR_WATCH]` proof.

A CSA announcement may be displayed during any Trace phase, but it never
changes Trace state. Starting Stop during confirmation, after a lock, after a
failed tune, or after an idle release keeps the last acknowledged Trace state
visible until JanOS prints the exact final `All operations stopped.` reply.
`Deauth Detector stopped.`, a timeout, or another unconfirmed reply does not
pretend that remote Trace state has been cleared. After the final
acknowledgement, the next Trace-enabled run starts by waiting for `armed` again.

## Autonomous Defence status

Defence is controlled entirely by JanOS after the selected-BSSID start. ADV
does not participate in the receive-to-transmit path and does not infer an
action from a received DEAUTH, disassociation, CSA, Trace lock, provisional
lock, or trigger. It strictly consumes these status records before event
parsing:

```text
[DETECTOR_DEFENCE] armed targets=<n> trace=fast pmkid=<monitor|decoy>
[DETECTOR_DEFENCE] trigger=<deauth|disassoc|csa|pmkid_probe> BSSID=<mac> STA=<mac|-> RX=<ch> decision=<decoy_4way|decoy_pmkid|observe> reason=<token>
[DETECTOR_DEFENCE] provisional_bssid=<mac> channel=<ch>
[DETECTOR_DEFENCE] tx=<decoy_4way|decoy_pmkid> stage=<m1_m2|m1_pmkid> result=<ok|error> frames=<0..2> latency_us=<n> error=<none|token>
[DETECTOR_DEFENCE] suppressed=<n> reason=<rate-token> BSSID=<mac> window_ms=1000
[DETECTOR_DEFENCE] released_bssid=<mac> channel=<ch> reason=idle idle_ms=20000 response=<decoy_4way|decoy_pmkid> suppressed=<n>
[DETECTOR_DEFENCE] stopped observations=<n> tx_pairs=<n> pmkid_m1=<n> responses=<n> satisfied=<n> errors=<n> dropped=<n> outcome=<responded|observed|idle>
```

Field order, spelling, numeric bounds, response/stage pairing, selected BSSID,
and transaction order are validated. One leading JanOS prompt is accepted. Every
line with the exact marker prefix is status traffic even when malformed, so it
cannot become a DEAUTH/CSA event. Only exact `tx ... result=ok` records in an
eligible response state increment the visible response count. A trigger is an
observation and an `outcome=responded` summary proves only that JanOS reported a
completed decoy transmission; neither proves that an attack was prevented or
that a real handshake was not captured.

The stopped summary's `responses` is JanOS's count of successful decoy
transmissions. `satisfied` counts later eligible observations suppressed after
an already successful response; it is not another transmission and does not
increase the live response count. `outcome=observed` means selected-target
traffic without a reported successful response, while `idle` means no selected
target observation.

The compact UI distinguishes waiting for arm, armed, provisional channel,
responding, response OK/error, Trace lock, released/rearmed, unsupported, and
remote uncertainty. Unselected BSSID records cannot replace selected-session
context. Stop pending, a timeout, a send failure, and a disconnect preserve the
last confirmed Defence and Trace state. Only exact `All operations stopped.`
clears the remote state. The earlier detector-specific stop line and the
`[DETECTOR_DEFENCE] stopped ...` summary do not finalize cleanup.

Ordinary Defence uses a bounded decoy four-way-handshake response reported by
JanOS. PMKID Lab is experimental because a legitimate authentication and
association sequence can look identical at this observation level. Cardputer
never transmits deauthentication, disassociation, jamming, or corrective CSA,
and it never labels the claimed transmitter as a physical attacker.

## Optional Deauth Forensics v1

The normal detector, Trace, and Defence paths work without forensic telemetry.
Press **W** from an all-network view, or press **W** twice through the selected
WATCH view, to open the compact Forensics page. If the active firmware has not
emitted a valid v1 row, the page shows `Forensics unavailable`; this is a
supported compatibility state, not an error and not simulated evidence.

ADV strictly accepts this optional record after the same single JanOS prompt
normalization used by the other detector records:

```text
[DEAUTH_FORENSIC/1] FRAME=<deauth|disassoc> CH=<1..233|-> BSSID=<mac> TX=<mac> DA=<mac> RSSI=<-127..0|-> REASON=<0..65535|-> SEQ=<0..4095|-> RETRY=<0|1|-> PROTECTED=<0|1|-> FC=<0x0000..0xFFFF|-> TS_US=<0..UINT64_MAX>
```

Field names, order, separators, ranges, hexadecimal width, and the marker
version are exact. Missing, duplicate, reordered, extra, truncated, oversized,
or malformed fields and MAC addresses are rejected. `-` remains visibly
unknown; ADV does not substitute zero or infer a value from another record.
`TS_US` is monotonic JanOS boot time rather than wall time. `TX` remains only
the transmitter address claimed by the received frame.

Forensic rows are evidence enrichment only. They never increment DEAUTH/CSA
counters, replace the latest V2 observation, create a history event, change
Trace or Defence state, count a Defence response, or trigger an alarm, sound,
or notification. The strict `[DEAUTH_V2]` record remains the sole source of
detector event counts.

The session keeps at most 16 detail rows. Older detail is evicted locally and
increments the saturating `Drop` count while the accepted-row total and summary
continue. Unique BSSID/DA and reason-code tables are recalculated within this
bounded detail model. All public totals saturate instead of wrapping.

A burst uses the exact `(FRAME, BSSID, TX, DA)` key. A key change always starts
a new burst. With an unchanged key, a gap up to and including 20,000,000 us
stays in the burst; a larger gap starts another. The page reports rows, bursts,
largest burst, local detail drops, RSSI minimum/rounded-average/maximum, and
protected, unprotected, retry, and unknown totals. `Unknown` counts every
unavailable optional field (`CH`, `RSSI`, `REASON`, `SEQ`, `RETRY`, `PROTECTED`,
or `FC`) with saturation. A mixed RSSI aggregate carries a compact `?N` missing
sample count, so known samples never hide unavailable RSSI evidence.

Sequence interpretation compares the latest row with the most recent
comparable stream retained in the bounded model:

- identical sequence with `RETRY=1` is `retransmission`;
- identical sequence with `RETRY=0` is `repeated sequence`;
- another comparable value shows only the modular 12-bit `d+N` delta;
- fewer than two comparable rows shows `insufficient sequence data`.

These labels are neutral. They do not prove injection, physical sender
identity, AP origin, or attack prevention, and they do not compare management
sequence numbers with beacon numbering because v1 carries no beacon reference.

The local forensic session starts only after the existing detector start
proofs confirm a new run. Ingestion closes immediately on Stop, timeout, send
failure, or disconnect, preserving the last summary. Only exact
`All operations stopped.` finalizes that summary; late rows while the remote
generation is uncertain or a new start is still pending are ignored. The next
confirmed run resets the bounded session. The stopped page is explicitly
labelled `Evidence only`. While Stop is pending, timed out, send-failed, or the
connection is lost, the page retains its evidence and explicitly shows remote
state uncertainty. After a successful Stop-for-scan acknowledgement, **W** from
the fresh scan picker reopens the finalized summary until another run is
confirmed.

Each processed deauth frame produces one V2 line, with no parallel legacy line:

```text
[DEAUTH_V2] CH: <ch> | BSSID: <mac> | TX: <mac> | DA: <mac> | RSSI: <rssi> | REASON: <0..65535|-> | SSID: <text>
```

An asynchronous detector record can appear immediately after the JanOS REPL
prompt, for example `> [CSA] RX: ...`. Cardputer accepts one leading `>` with
an optional space on active detector records. Instructional `Output:` examples
and status lines are not counted as events.

`TX` is the transmitter address claimed by the frame, not proof of the physical
sender. `DA` is the frame's recipient address; a broadcast `DA` has no single
identified individual client. `REASON` is the frame's reason code in the range
0 to 65535. A DEAUTH record is a received observation, not proof of an attack or
of a client disconnecting. Cardputer accepts `REASON: -` only with an
individual unicast `DA`; for a protected unicast frame, the reason bytes are
encrypted and Cardputer shows the reason as unavailable (PMF).

For Passive and Trace only, the exact JanOS response `Unrecognized command`
while a V2 start is pending, before any start or format proof, triggers a retry
with the legacy command on the same UART. The automatic watch then sends bare
`deauth_detector`; a selected watch keeps its scan numbers and sends
`deauth_detector --only <scan numbers>`. Cardputer uses the legacy event parser
for the retry and still requires the JanOS start reply and, for a selected
watch, `[DETECTOR_WATCH]` confirmation before showing it as active. JanOS's
original `deauth_detector` syntax and output remain unchanged. A timeout or
ambiguous response does not trigger an automatic retry.

Defence and PMKID Lab never fall back to a passive legacy command. The exact
`Unrecognized command` response, or an explicit invalid Defence/PMKID option
diagnostic, becomes `Defence unsupported`. A timeout, malformed status,
ambiguous reply, send failure, or disconnect remains visible uncertainty and
does not authorize fallback.

When Trace was requested, this exact legacy fallback is visibly marked as
Trace unavailable and the legacy command never receives the `trace` token. An
older V2 implementation may instead reject `trace` as an invalid detector or
watch index, or with the exact
`Invalid detector option; use --only before scan indices.` diagnostic.
Cardputer reports Trace as unavailable and does not automatically restart
without Trace.

Deauth and CSA are separate observations. CSA has its own counter and view and
does not play an audible alarm. The CSA view shows the network, BSSID, claimed
transmitter, receive channel, announced target channel, RSSI,
and announcement fields. A CSA announcement alone is not proof of an attack,
does not confirm that the AP changed channels, and does not identify the
physical transmitter.

Passive and Trace remain receive-only. Active Defence is a JanOS-owned bounded
decoy response for controlled networks; the Cardputer UI only selects targets,
sends commands, and displays strict telemetry.

The former automatic three-hit transition remains removed:
`deauth_detector <scan number>` selects a channel, not a network. Exact-network
selection requires JanOS with `--only` and `[DETECTOR_WATCH]` support. Older
JanOS can still use the original all-channel flow through the explicit
fallback above.

These host tests compile the real Cardputer screen with deterministic UART and
UI fixtures. They verify controller parsing and state transitions only. They do
not prove a firmware build, flashed-device UART behavior, RF transmission,
timing, client impact, or interoperability with a capture campaign.
