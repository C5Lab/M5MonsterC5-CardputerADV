# Sniffer clients and per-client deauth

Update both the ADV app and JanOS (`janos_public/official/projectZero/ESP32C5`).
The list uses the opt-in `show_sniffer_clients` API. Older clients continue to use
unchanged plain/vendor/extended JanOS text APIs.

ADV displays an SSID discovered from received management frames, or a real SSID
cached from a scan. If no SSID was discovered, it displays the AP's BSSID/channel.
The header always shows the selected client's BSSID, so equal SSIDs do not imply
the same target. Binary/control bytes in names are sanitized for display only.

Select a client with UP/DOWN and press **D**. ADV sends
`start_deauth_station <BSSID> <station MAC>` and opens the active screen only when
JanOS confirms that exact pair. No scan lookup or SSID matching is involved.
Press ESC to stop; **R** reloads the client list. Duplicate D while starting is
ignored. Cancel/timeout sends `stop`; a timed-out start requires a reload before
retrying, so late acknowledgements cannot reopen the active screen.

The list supports 100 APs / 5000 clients and validates begin/end counts. Incomplete
lists and memory failures are explicit errors. A maximum-size list can take over
a minute over UART. Names of hidden networks can remain unavailable until a frame
reveals them. A start acknowledgement does not guarantee disconnection (e.g. PMF).

Host tests:

```sh
python3 -m unittest discover -s tests -p 'test_sniffer_clients.py'
```

To include a test that feeds actual JanOS command output into the ADV parser:

```sh
JANOS_SOURCE_ROOT=/path/to/projectZero/ESP32C5 python3 -m unittest discover -s tests -p 'test_sniffer_clients.py'
```

In JanOS, run `python3 -m unittest discover -s tests -p 'test_sniffer*.py'`.
The suites cover frozen legacy output, captured identity, BSSID/STA targeting,
full-size lists, special SSID bytes, busy responses, acknowledgements, timeout,
cancel and return from the active screen. Hardware validation with older clients
and both radio bands is a separate remaining check.
