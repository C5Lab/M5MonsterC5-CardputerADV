# Deauth Detector on Cardputer ADV

Opening **Deauth Detector** first shows the optional **Trace** choice. Trace is
off by default, and no detector command is sent until **Enter** starts the
watch. Press **Space** to toggle Trace or **Esc/Q** to leave without starting
JanOS.

With Trace off, the all-channel watch uses the unchanged
`deauth_detector_v2` command. With Trace on, it uses
`deauth_detector_v2 trace`. The choice remains in effect for every subsequent
all-channel or selected-BSSID run until this detector screen is closed.
After a successful V2 start, JanOS emits
`[DETECTOR_INFO] format=deauth_v2`. Cardputer shows the watch as active only
after the JanOS start reply and this format confirmation.

Press **S** to stop it and start a fresh scan for a specific-network watch.
After the scan completes, use **Up/Down** to move, **Space** to select one or
more APs, and **Enter** to start. The list shows each AP's scan number, SSID,
channel, and BSSID. An empty selection cannot start.

The selected watch uses `deauth_detector_v2 --only <scan numbers>`. With Trace
enabled, it uses `deauth_detector_v2 --only <scan numbers> trace`. The literal
`trace` token is always last. Cardputer never sends `--trace`, never adds Trace
to legacy `deauth_detector`, and does not add a bare-index detector mode.
Cardputer waits for the JanOS start reply, the V2 format confirmation, and
`[DETECTOR_WATCH]` lines confirming the selected BSSIDs before showing the watch
as active. Repeated scan rows for one BSSID remain selectable by their original
numbers; JanOS reports that BSSID once in the watch list. Press **W** while
watching to inspect the target list, or **S** to stop and scan again. **Esc/Q**
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

Only the exact JanOS response `Unrecognized command` while a V2 start is
pending, before any start or format proof, triggers a retry with the legacy
command on the same UART. The automatic watch then sends bare
`deauth_detector`; a selected watch keeps its scan numbers and sends
`deauth_detector --only <scan numbers>`. Cardputer uses the legacy event parser
for the retry and still requires the JanOS start reply and, for a selected
watch, `[DETECTOR_WATCH]` confirmation before showing it as active. JanOS's
original `deauth_detector` syntax and output remain unchanged. A timeout or
ambiguous response does not trigger an automatic retry.

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

The detector remains passive. Trace adds no frame transmission, active defense,
or countermeasure.

The former automatic three-hit transition remains removed:
`deauth_detector <scan number>` selects a channel, not a network. Exact-network
selection requires JanOS with `--only` and `[DETECTOR_WATCH]` support. Older
JanOS can still use the original all-channel flow through the explicit
fallback above.
