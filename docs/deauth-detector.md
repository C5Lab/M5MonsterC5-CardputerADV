# Deauth Detector on Cardputer ADV

Opening **Deauth Detector** starts the existing all-channel JanOS command.
Press **S** to stop it and start a fresh scan for a specific-network watch.
After the scan completes, use **Up/Down** to move, **Space** to select one or
more APs, and **Enter** to start. The list shows each AP's scan number, SSID,
channel, and BSSID. An empty selection cannot start.

The selected watch uses `deauth_detector --only <scan numbers>`. Cardputer waits
for the JanOS start reply and `[DETECTOR_WATCH]` lines confirming the selected
BSSIDs before showing the watch as active. Repeated scan rows for one BSSID
remain selectable by their original numbers; JanOS reports that BSSID once in
the watch list. Press **W** while watching to inspect the target list, or **S**
to stop and scan again. **Esc/Q** opens an exit confirmation; **Enter** confirms
and **Esc/Q** cancels. After confirmation, Cardputer sends `stop` (or waits for
an already pending `stop`) and leaves only after JanOS prints
`All operations stopped.`. The earlier `Deauth Detector stopped.` line does not
confirm that JanOS has finished stopping. A failed send or missing final reply
keeps the screen open and shows that stop is unconfirmed; **Esc/Q**, then
**Enter** retries. A failed or timed-out scan, rejected command, or missing
reply is shown as an error rather than a running detector.

Deauth and CSA are separate observations. The CSA view shows the network,
BSSID, claimed transmitter, receive channel, announced target channel, RSSI,
and announcement fields. A CSA announcement alone is not proof of an attack,
does not confirm that the AP changed channels, and does not identify the
physical transmitter.

The old quick start remains available through bare `deauth_detector`. The
former automatic three-hit transition is removed: `deauth_detector <scan
number>` selects a channel, not a network. Exact-network selection requires
JanOS with `--only` and `[DETECTOR_WATCH]` support. Older JanOS can still use
the original all-channel flow.
