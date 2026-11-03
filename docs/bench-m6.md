# Bench M6 — remote access bring-up (PLAN.md §12 M6, D36)

What CI proves and what only the bench can. CI compiles the glue in every
build and covers the whole **pure** path with `test_remote` (settings blob,
JSON builders/parsers, HA discovery entities, notify texts, auth gate, CSV).
The QEMU builds compile the glue to stubs — QEMU has no Wi-Fi, so the web
server and MQTT only run on hardware. What the bench proves: the dashboard
actually serves on the LAN, the broker accepts the connection, discovery
appears in Home Assistant, and notifies arrive.

## Web dashboard

Boot with Wi-Fi configured (`set wifi <ssid> <pass>` or the wizard), serial
console open. Expected markers after the radio comes up:

```
web dashboard on :80
```

Then open `http://<hub-ip>/` from a phone on the same LAN (the IP prints in
`remote`). Checklist:

- Status line shows `<n>/<m> online`, mqtt state, port.
- Node cards: state dot (green/amber/red), supply V, stock estimate,
  last feed, next occurrence.
- **Feed now** with the node busy or unpaired → "refused: busy" / HTTP 409.
- History table refreshes; CSV and backup links download.
- Settings pane saves (`saved` toast, hub applies and re-serves).

Auth: `set web 1 80 1` (or the web form) turns on the PIN gate — the page
prompts once and every API call carries `X-AIO-Pin`. Wrong PIN → HTTP 401.
Fail-closed: with `web_auth` set but **no PIN configured, everything 401s** —
set a PIN first (`pin set 0000 1234`).

## MQTT + Home Assistant

```
set mqtt mqtt://<broker>:1883 <user> <pass>    (bare host works too)
remote
```

`remote` prints the connection state (`up` once CONNACK arrives). On connect
the hub publishes, per §6.7:

- `aio/hub/status` = `online` (retained; LWT flips it to `offline`)
- `aio/node/<id>/state` + `aio/node/<id>/config` (retained, refreshed 30 s)
- `homeassistant/{button,sensor,binary_sensor}/aio-n<id>-*/config`

HA then shows, per node: a **Feed now** button (amount = first schedule
entry), Last-feed and Stock-estimate sensors, an Online binary sensor, and a
Supply-voltage sensor for profiles with the supply-monitor flag. Feed from
HA (button or `aio/node/<id>/cmd/feed` with `{"amount":N}`) and watch the
node run — a bad payload gets a notify, never a feed.

Notify checks (FR20): power a node off mid-peace → `aio/hub/notify`
"offline" within ~30 s; pull its feed result negative (or kill the link) →
"feed failed"/"NO-LINK" notify; run the hopper under 20% → one "low stock"
notify per boot.

## Known limits (v1)

- The low-stock latch clears only on reboot (D36 note in remote_net.cpp).
- `/api/events` returns at most the newest 24 rows; full export is the CSV.
- `mqtts://` is accepted by the URL parser but the client runs without a
  custom CA bundle — treat TLS as [VERIFY] before exposing anything beyond
  the LAN (§6.7: "TLS if flash/RAM allow", not yet done).
