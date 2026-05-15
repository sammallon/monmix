# ProPresenter stage-display product — investigation

**Branch:** `propresenter-investigation` (off `master`)
**Hardware:** CrowPanel Advanced 10.1" (ESP32-P4 + ESP32-C6, EK79007
MIPI-DSI 1024×600, GT911 touch) — same board as monmix.
**Date:** 2026-05-15

---

## TL;DR

The latest ProPresenter API (>=7.17, surfaced via the
`renewedvision-propresenter` npm wrapper v7.7.2 used by Bitfocus
Companion in production) exposes a **plain HTTP/1.1 chunked-streaming
status endpoint** that is a near-perfect fit for embedded use:

- One persistent **POST `/v1/status/updates`** with a JSON array of
  sub-paths in the request body. Server holds the connection open and
  pushes `\r\n\r\n`-delimited JSON envelopes (`{url, data}`) on every
  change. No WebSocket handshake, no frame masking, no ping/pong —
  `esp_http_client` in stream-read mode or mongoose can handle it.
- The exact information needed for a current-and-next stage display
  (`status/slide`, `presentation/active`, `presentation/slide_index`,
  `timer/system_time`, `timers/current`, `stage/message`,
  `look/current`, `status/stage_screens`) is all subscribable.
- **Slides are rendered firmware-side from the payload.** The
  `/v1/status/slide` payload carries lyric/text content plus image
  references; we composite them with bundled LVGL fonts (Montserrat
  Latin) at large size onto an LVGL canvas. ProPresenter's thumbnail
  endpoints exist but are too low-resolution for a primary stage view
  — they are reserved for a "gallery/picker" secondary feature where
  the user surveys multiple slides on one screen and picks one to
  view fullscreen.

**Hardware verdict:** because we render text + image references
firmware-side rather than blitting pre-composed slide bitmaps, the
slide region is **not constrained to 16:9**. Combined with the user's
ability to configure ProPresenter's stage display output to an
arbitrary resolution, both orientations are now viable:

| Orientation | Logical W×H | Strengths |
|---|---|---|
| Landscape 0°/180° (native) | 1024×600 | Long lyric lines (one line per slide) flow well; modest header + footer chrome; matches how musicians typically read sheet music |
| Portrait 90°/270° (new) | 600×1024 | More vertical room per slide for prose / scripture / long announcements; better when a single slide has many short lines |

Concrete sketch for each in §3.4. Pick once we have a sample of actual
slide content to render against. Adding 90/270° rotation support to
monmix's display + prefs is independent of the orientation pick — it
expands the user-selectable options regardless.

**monmix reuse is high.** WiFi bring-up, SD/NVS prefs, config-panel
shell, theme system, sleep/idle scaffolding, splash, console REPL,
coredump pipeline, OTA story, even the build/test harness all port
without touching the new product's core. The replacement is mostly
contained in:

1. A new `app_pp_client.c` (replaces `app_ms_client_ws.c`)
2. A new main page (replaces the fader page in `app_ui.c`) — does its
   own slide text rendering with bundled Montserrat at large size
3. Rotation extended to 90°/270° in `app_display.c` + `app_prefs.c`
4. Optional background-image decoder if PP exposes background media at
   usable resolution (empirical question, see §4)

No SDIO changes, no C6 firmware changes, no hardware changes.

---

## 1 — Hardware feasibility

### 1.1 Panel + rotation + content shape

Because slide content (text + image refs) is rendered firmware-side,
we are not forced to lay out 16:9 boxes. ProPresenter's stage display
output resolution is also user-configurable, so the API-side payload
arrives shaped for whatever aspect ratio we ask it to render against.
Both orientations work — pick on content/ergonomic grounds, not
geometric necessity.

| Mode | Logical W×H | Tradeoffs |
|---|---|---|
| Landscape (native) | 1024×600 | One wide region per slide. Excellent for lyrics (typical lyric lines fit on one 1024 px row at 80 px caps). Less vertical room per slide. |
| Portrait (90°/270°) | 600×1024 | Two stacked tall regions. Better for prose-shaped content (scripture, long announcements) where a single slide has many short lines. Tighter horizontal wrap. |

Either way, the "drawable area per slide" can be made larger than the
forced 16:9 numbers in the earlier framing — e.g. landscape current
slide at 1024×320 (80 px header + 320 current + 180 next + 20 chrome)
gives 327 680 px² per slide; portrait current at 600×440 gives
264 000 px². Landscape now slightly *exceeds* portrait on raw drawable
area, especially for the *current* slide which gets the larger
allocation.

**Rotation status in monmix today:** only 0° and 180° are wired up;
`app_display_rotation_t` has two enum values and the prefs UI offers
two radio buttons. Extending to 90°/270° is a real change but
self-contained:

- `app_display_apply_rotation` already calls `lv_display_set_rotation`;
  LVGL supports `LV_DISPLAY_ROTATION_90` and `_270` out of the box.
- `sw_rotate = true` is already set on the lvgl_port_dsi cfg.
- Double-buffered PSRAM allocation is `LCD_H_RES * LCD_V_RES` (~1.2 MB
  per buffer, 2 buffers = ~2.4 MB) and stays valid regardless of
  rotation — pixel count doesn't change.
- Touch coordinate remap is automatic via LVGL's indev handling when
  the touch device shares the display (already wired up).
- Sim test injection (`app_touch_inject.c`) already has a `switch
  (lv_display_get_rotation(disp))` that handles all four rotations,
  so the test framework follows for free.

Adding 90/270° expands the orientation prefs from 2 options to 4
regardless of which orientation this product defaults to — useful in
both products.

**Risk:** untested whether the EK79007 panel + esp_lcd_mipi_dsi driver
combination handles 90° SW rotation cleanly at the framerate we need.
ProPresenter is not animating frames at 60 Hz — slide changes are
events, not video. Should be fine in practice; empirically verifiable
in ~30 min on hardware.

### 1.2 Bandwidth + memory

- Streaming HTTP/1.1 connection holds open: tens of bytes/sec idle
  (just the `timer/system_time` heartbeat once per second), spikes of
  a few KB on slide change events.
- **Slide text rendering** is the dominant on-device cost: LVGL
  draws a multi-line label at large size (60–80 px caps) per slide
  change. Painting two 600×337 text regions is well under the cost
  of a fader-drag stress run on monmix.
- Bundled LVGL Montserrat covers the Latin set we need. We don't try
  to match ProPresenter's chosen fonts — same approach as a stage
  monitor wedge: legible, consistent, not branded.
- Backgrounds: **if** the API exposes high-resolution background media
  (separate from the low-res thumbnail endpoint), we fetch on slide
  change and decode via ESP32-P4's hardware JPEG (`esp_jpeg`). At
  600×337 RGB565 = ~400 KB per decoded slide bg; comfortable in 32 MB
  PSRAM. **If** the only image surface is thumbnails, slides render
  solid-coloured (theme bg) with text only — still functional and
  consistent with how monitor wedges in pro audio look in practice.
- Thumbnail endpoint is reserved for the *gallery/picker* secondary
  feature, where 6–12 thumbs on a single screen are exactly the right
  resolution match.

### 1.3 Network reliability

ProPresenter is typically on a wired LAN at a fixed IP; far less hostile
than monmix's stage-WiFi-to-MS-laptop path. The reconnect watchdog +
C6/ESP-Hosted wedge workaround from monmix port directly. Worth
keeping the same WiFi reassoc bullet in our pocket for any TCP-wedge
recovery.

---

## 2 — Latest ProPresenter API surface

**Source of truth used:**
1. `renewedvision-propresenter` npm package, v7.7.2 (latest).
   Repo: `https://github.com/JeffreyDavidsz/node-renewedvision-propresenter-api`.
   Wraps every endpoint in typed methods; 134 unique `/v1/...` paths.
2. Bitfocus Companion module
   `bitfocus/companion-module-renewedvision-propresenter-api` (active
   maintenance, "Works best with later versions >=17"). Confirms which
   sub-paths the chunked streaming endpoint accepts and demonstrates
   the production pattern.
3. The jeffmikels 7.9 OpenAPI mirror is **stale** but still useful for
   response *schemas* on stable endpoints — kept under `docs/raw/`.

**Authoritative spec is whatever the running ProPresenter instance
serves at runtime** — most modern apps with OpenAPI host their own
swagger/`/docs` endpoint, but this hasn't been verified for
ProPresenter specifically. Until the user has an instance reachable,
the v7.7.2 wrapper is the field-tested approximation.

### 2.1 Endpoints we actually need

| Endpoint | Method | Use |
|---|---|---|
| `/v1/status/updates` | POST (chunked) | Subscribe to all change events on one socket |
| `/v1/status/slide` | GET | Bootstrap: current + next slide payloads (text content + image refs) — **primary rendering input** |
| `/v1/presentation/active` | GET / stream key `presentation/active` | Title, group, slide count, current presentation UUID |
| `/v1/presentation/slide_index` | GET / stream key `presentation/slide_index` | Current slide index within active presentation |
| `/v1/presentation/{uuid}/thumbnail/{index}?quality=N` | GET | **Low-resolution** preview image; reserved for a gallery/picker secondary screen, NOT used as the primary stage view |
| `/v1/timer/system_time` | GET / stream | Wall clock (also serves as 1 Hz keepalive) |
| `/v1/timers/current` | GET / stream | All countdown/elapsed timers (service countdown, song timer) |
| `/v1/stage/message` | GET / stream key `stage/message` | Stage messages from FOH ("PASTOR — 2 min") |
| `/v1/look/current` | GET / stream | Currently active look (useful as a status indicator) |
| `/v1/status/stage_screens` | GET / stream | Stage screens on/off |
| `/v1/announcement/active` | GET / stream | Announcement layer (separate from main presentation) |

That's everything for a v1 product. Trigger endpoints
(`/v1/trigger/next`, `/v1/presentation/active/next/trigger`, etc.)
exist if we ever want a "musician can advance their own slides"
follow-up — but the user's brief is read-only stage view, so we don't
need them.

### 2.2 Streaming endpoint wire format (empirically confirmed in
the bitfocus + renewedvision-propresenter source)

**Subscribe:**
```http
POST /v1/status/updates HTTP/1.1
Host: propresenter.local:PORT
Content-Type: application/json

["status/slide","presentation/active","presentation/slide_index","timer/system_time","timers/current","stage/message","look/current","status/stage_screens"]
```

**Response:** `HTTP/1.1 200 OK` with `Transfer-Encoding: chunked`,
held open indefinitely. Body is a stream of JSON envelopes separated
by `\r\n\r\n`:

```
{"url":"status/slide","data":{ ... slide payload ... }}\r\n\r\n
{"url":"timer/system_time","data":{...}}\r\n\r\n
{"url":"presentation/slide_index","data":{...}}\r\n\r\n
...
```

`timer/system_time` fires once per second whether anything changed
or not — that's our liveness/heartbeat signal. If we miss two ticks
in a row, treat the connection as wedged and rebuild it.

Boot-time hydration: send GET requests for the same set of paths once
to populate initial state, then the chunked stream takes over for
deltas. Or rely on the initial-value emission that the streaming
endpoint sends for each subscribed path on connect (need to verify
empirically — the wrapper's flow suggests this works but the doc isn't
explicit).

### 2.3 `/v1/status/slide` payload (from the wrapper's typedef +
the stale 7.9 spec — verify empirically)

Roughly:
```json
{
  "current": {
    "uuid": "...",
    "text": "Verse 1 lyrics line 1\nLine 2\nLine 3",
    "image_uuid": "..."
  },
  "next": {
    "uuid": "...",
    "text": "Verse 1 lyrics line 4\nLine 5",
    "image_uuid": "..."
  }
}
```

The `text` is what drives the on-stage display — rendered firmware-
side with bundled Montserrat in a large LVGL label, wrapped to the
slide region (600×337 in portrait). Multiple text "lines" from PP
come delimited; preserve them as separate lines in the label.

The `image_uuid` (or similar) is what you'd pass to the thumbnail
endpoint *for the gallery/picker view*, not for the primary display.
Whether ProPresenter exposes any **high-resolution** image surface
for slide backgrounds — separate from thumbnails — is an open
empirical question (§4 Q4).

**Open empirical questions on the payload:** does PP send rich-text
formatting (color, weight, alignment) or just plain text? Does it
include per-line styling cues? What about non-Latin glyphs the
bundled Montserrat doesn't cover — they'll just box-glyph, which is
acceptable per the scope decision above.

---

## 3 — Architecture sketch

### 3.1 What stays unchanged from monmix

| Module | Role | Change? |
|---|---|---|
| `app_wifi.c` | C6 / ESP-Hosted bring-up, reassoc on TCP wedge | **None.** Port as-is. |
| `app_storage.c` | SD-card mount with dummy host.init dance | **None.** Port as-is. |
| `app_prefs.c` | NVS-primary + SD-mirror with mtime conflict resolve | Extend the prefs struct: add PP host/port, drop MS host/port, add display rotation 90/270 values. Same mechanism. |
| `app_display.c` | EK79007 bring-up, LVGL port, backlight, rotation | **Extend** rotation enum + apply path to handle 90/270. Otherwise unchanged. |
| `app_console.c` | UART REPL with cat-b64 / coredump-b64 / ls | **None.** Port as-is — same diagnostics value. |
| `app_coredump.c` | Postmortem to SD on next boot | **None.** Port as-is. |
| `app_logd.c` | Runtime ring log to SD with heap heartbeat | **None.** Port as-is. |
| `app_ms_info.c` (settings overlay shell) | Config-panel framing | **Repurpose** as PP-info panel — same shell, different content. |
| WiFi config UI / scan UX / DHCP-vs-static / SSID with hidden network | All the post-pilot polish from P6, P7 | **None.** Port as-is. |
| Splash screen + theme system + dark/light | Boot-time UX | **None.** Port as-is; reskin assets later. |
| Sleep / display power mgmt (M7 — pending in monmix) | Idle blank + connectivity-aware sleep | **Same plan applies.** Connectivity gate becomes "PP unreachable" / "PP attached to no presentation". |
| Coredump pipeline + OTA path | Reliability hooks | **None.** Port as-is. |
| `pc_sim/` build + `tests/` framework | Native sim + scripted tests | Adapt: mock PP client replaces mock MS client. The test grammar (tap/screenshot/wait/quit) doesn't change. |

### 3.2 What's new

| Module | Role |
|---|---|
| `app_pp_client.c` + `.h` | Persistent HTTP/1.1 connection to `/v1/status/updates`; parses chunked `\r\n\r\n`-framed JSON; on slide change, pushes the payload to UI. Iface mirrors `app_ms_client` so an OSC-equivalent could swap in later. |
| `app_pp_state.c` + `.h` | Holds current/next slide text + image refs, active presentation title, slide index, system time, stage message, active timers. Subscriber pattern, same shape as `app_state.c`. |
| `app_pp_ui.c` + `.h` | New main page — replaces the fader tileview. Portrait 600×1024 layout (see §3.4). Renders slide text in LVGL labels with bundled Montserrat at large size (60–80 px caps), word-wrapped to the slide region. Optional background fill (per-slide colour from payload, if PP provides) or background image (if the API exposes media at usable resolution — see §4 Q4). |
| Image-decoder shim *(conditional)* | Only if §4 Q4 confirms a high-resolution background-media endpoint. Calls `esp_jpeg` decoder; LVGL canvas binds the RGB565 result. If PP only exposes low-res thumbnails, this module is skipped and slides render text-on-solid-bg, which is consistent with how monitor wedges typically look. |
| Gallery/picker page *(secondary, optional)* | Uses the thumbnail endpoint at its native low resolution to show 6–12 upcoming slides on one screen; tap to view fullscreen. Out of scope for the first product release but baked into the navigation shape so it can land later without restructuring. |

### 3.3 What goes away

| Removed from monmix | Why |
|---|---|
| `app_ms_client_ws.c` / `app_ms_client_osc.c` | Different protocol; replaced by `app_pp_client.c`. |
| `app_state.c` (fader-channel state) | Replaced by `app_pp_state.c`. |
| Fader page in `app_ui.c` (tileview + channel widgets + sliders + mute buttons) | Replaced by `app_pp_ui.c`. |
| Channel picker overlay | No equivalent concept. |
| Touch-injection for fader drag | Irrelevant; test framework's `tap`/`shot`/`wait` still cover what we need. |
| MUTE EN guard / mute-toast | Irrelevant. |
| `app_touch_inject.c` keeps the rotation-aware tap mapping but loses fader-specific helpers. | Trim, don't remove. |

### 3.4 Page composition (two viable layouts)

Stage display output resolution is user-configurable on the
ProPresenter side, and slides are text-rendered on-device, so we are
not boxed into 16:9 regions. Both orientations work; the choice is
ergonomic. Below are concrete starting layouts for each.

**Landscape (1024×600 native, no rotation change needed):**
```
┌──────────────────────────────────────────────────────────┐
│ HH:MM:SS    T-12:34    Stage msg ticker      WiFi · PP   │  60 px header
├──────────────────────────────────────────────────────────┤
│                                                          │
│  CURRENT SLIDE — 1024 × 340                              │  340 px
│  Lyrics rendered in bundled Montserrat                   │
│  ~80 px caps, word-wrapped to width                      │
│                                                          │
├──────────────────────────────────────────────────────────┤
│ NEXT                                                     │
│  NEXT SLIDE — 1024 × 180   (dimmed)                      │  180 px
│  Smaller font, ~50 px caps                               │
├──────────────────────────────────────────────────────────┤
│ Presentation title · Look · countdown · status icons    │  20 px footer
└──────────────────────────────────────────────────────────┘
```

**Portrait (600×1024 after 90° rotation):**
```
┌────────────────────────────────────────────────────┐
│ HH:MM:SS         T-12:34          ⚙  ▣            │  80 px header
├────────────────────────────────────────────────────┤
│                                                    │
│   CURRENT SLIDE — 600 × 440                        │  440 px
│   Bundled Montserrat, 70–80 px caps,               │
│   word-wrapped to 600 px width                     │
│                                                    │
│                                                    │
├────────────────────────────────────────────────────┤
│ NEXT                                               │
│   NEXT SLIDE — 600 × 360   (dimmed)                │  360 px
│   ~50 px caps                                      │
│                                                    │
├────────────────────────────────────────────────────┤
│ Stage message marquee · Presentation title         │  144 px footer
│ Look · WiFi · PP status                            │
└────────────────────────────────────────────────────┘
       600 px wide  ×  1024 px tall after 90° rotation
```

Per-slide drawable area is comparable between the two — landscape
gives the current slide ~348k px² (320×1024 + chrome), portrait gives
it ~264k px² (600×440). Landscape wins for lyric-style content
(typical lines fit on one wide row); portrait wins when slides carry
short-line scripture or prose.

Header content (both):
- Wall clock (from `timer/system_time`)
- Most urgent countdown (from `timers/current` — lowest-positive)
- Settings gear → config panel
- Sleep/wake button

Footer content (both):
- Stage message text (`stage/message`) — marquees if too long
- Active presentation title (smaller weight)
- Status icons: WiFi RSSI, PP reachability dot, current "look"
  indicator

"NEXT" label and the dimmed style on the second region keep the
ordering unambiguous from across a stage.

**Decision rule:** before locking the orientation, render a small
corpus of real slide payloads in both layouts using the sim. Pick the
one that's more readable from a music-stand distance with the user's
typical setlist.

Total: 80 + 337 + 337 + 85 + 185 internal gaps = 1024 px. Fits cleanly.

Header content:
- Wall clock (large, from `timer/system_time`)
- Most urgent countdown (from `timers/current` — pick the lowest-positive)
- Settings gear → config panel (WiFi, PP host, brightness, theme, rotation)
- Sleep/wake button

Footer content:
- Stage message text (from `stage/message`) — marquees if too long
- Active presentation title (small, less prominent than the slide image)
- Status icons: WiFi RSSI, PP reachability dot, current "look" indicator
  (small color swatch)

The "current" slide gets the larger visual weight (could be 360 vs
315 if we want emphasis, but equal is the default and looks balanced).
"NEXT" label on the second slide makes the ordering unambiguous from
across a stage.

---

## 4 — Open empirical questions (need a live ProPresenter instance)

User's standing preference is "verify protocols empirically before
committing to firmware" — `repro_pp_*.py` scripts (gitignored) are the
right shape here. Specific things to confirm against a real
ProPresenter:

1. **Initial value on subscribe.** Does the streaming endpoint emit
   the current state of each subscribed path on connect, or does it
   only emit deltas? If deltas-only, the bootstrap path needs an
   explicit GET per path.

2. **`/v1/status/slide` exact JSON shape and text richness.** Field
   names for current vs next, line-separator convention, **rich-text
   formatting** (color, weight, alignment, per-line styling) versus
   plain text. Drives how the firmware label renders — plain
   `lv_label` is enough for plain text; `lv_span` if PP sends styled
   runs. Worst-case payload size (long verse-and-chorus lyric blocks).

3. **Thumbnail endpoint** — confirm dimensions and format. Expected to
   be too low-res for primary display but should match well with a
   6-up gallery layout (so each thumb is ~200×112). The size sweep in
   the probe script confirms this empirically.

4. **High-resolution background media access.** Separate from
   thumbnails — does ProPresenter expose slide backgrounds (images,
   videos' poster frames) at a resolution suitable for filling a
   600×337 region? Candidate endpoints to probe: `/v1/media/...`,
   `/v1/theme/{id}/slides/{theme_slide}`. If **no**, the firmware
   renders text on a solid theme bg — consistent with how monitor
   wedges look in practice. If **yes**, an optional background-image
   decoder pulls per-slide media on slide change.

5. **Disconnect/reconnect behaviour.** What happens to a chunked
   subscriber when ProPresenter restarts, or when its network drops?
   Does the server emit a clean close, or does the TCP hang? Drives
   the keepalive logic.

6. **Multiple subscribers.** Does ProPresenter handle N concurrent
   `/v1/status/updates` connections (multiple stage-display devices
   in the same venue)? Almost certainly yes, but worth confirming.

7. **Authentication.** Is the API gated by an app-level token, an
   HTTP Basic creds pair, or open-on-LAN? The wrapper's source
   suggests open-on-LAN by default. Need to confirm + decide whether
   we surface a credential field in the config UI.

8. **API version detection.** Does the running instance advertise its
   ProPresenter version? Some endpoints exist only on >=7.x; would be
   useful to detect at connect time and surface "needs ProPresenter
   7.17+" if older.

9. **Behaviour at boundaries.** What does `status/slide` return when
   nothing is presenting? When at the end of a presentation (no
   "next")? When mid-transition?

---

## 5 — Risks

1. **No live ProPresenter instance available yet.** All wire-format
   details above are inferred from npm wrapper + Companion module
   source, not measured. ~30 min of probe time against a real
   instance will resolve all of section 4.

2. **90°/270° rotation is untested on this panel.** Software path
   exists in LVGL, but the EK79007 + esp_lcd_mipi_dsi combo has
   produced surprises before (M8 brightness/rotation work surfaced
   that `esp_lcd_panel_mirror` is empirically a no-op). Bench time
   may be needed.

3. **`esp_jpeg` decoder integration** is straightforward but new
   surface for the codebase — and only needed *if* the §4 Q4 probe
   confirms a high-resolution background-media endpoint. If only
   low-res thumbnails are exposed, we skip the decoder entirely and
   render text on a solid theme bg.

4. **Bundled font coverage.** LVGL's default Montserrat is Latin-only.
   Non-Latin lyrics (e.g. Spanish accented characters mostly fine;
   Hebrew/Arabic/CJK won't render). Scope says we accept this; if a
   future user needs another script, that's a font-bundle change, not
   a redesign.

5. **Wall-clock drift.** ProPresenter pushes `timer/system_time` once
   per second; if the chunked connection wedges, the displayed clock
   freezes. Need a clear "stale clock" visual cue or fall back to
   local SNTP-disciplined time (monmix has the SNTP code, can port).

5. **Stale 7.9 OpenAPI** would mislead if used in isolation. We're
   anchoring on the v7.7.2 wrapper for endpoint enumeration, which
   tracks the latest. Document this in the firmware's README so
   future contributors don't grab the old mirror.

6. **Hardware lifecycle.** If Elecrow EOLs the CrowPanel Advanced or
   pushes a non-binary-compatible revision, both products inherit the
   same risk. Not specific to this investigation but worth flagging.

7. **The C6 SDIO storm.** The monmix product mitigated it via the
   `esp_restart` fallback when the wedge fires (~15 s recovery). A
   read-only stage display device sees less SDIO traffic, but the
   bug is in the C6 firmware so the same Pattern A wedge can fire
   under sufficient RX activity. Same mitigation applies; same
   resolution lives in the slave repo.

---

## 6 — Reuse audit (concrete file-by-file)

| File from monmix | Reuse | Notes |
|---|---|---|
| `main/app_wifi.c` + `.h` | ✓ as-is | C6 transport, reassoc, state subscribers |
| `main/app_storage.c` + `.h` | ✓ as-is | SD with dummy host.init |
| `main/app_console.c` + `.h` | ✓ as-is | UART REPL, cat-b64, coredump-b64 |
| `main/app_coredump.c` + `.h` | ✓ as-is | Flash → SD postmortem |
| `main/app_logd.c` + `.h` | ✓ as-is | Ring log, heap heartbeat |
| `main/app_display.c` + `.h` | extend | Add 90/270 rotation enum + apply |
| `main/app_prefs.c` + `.h` | extend | PP host/port replace MS host/port; rotation enum widened |
| `main/app_time.c` + `.h` | ✓ as-is | TZ apply (used by clock display) |
| `main/app_power.c` + `.h` | ✓ as-is | Brightness + sleep groundwork |
| `main/app_ms_info.c` + `.h` | repurpose | Generic config-panel shell; reskin contents |
| `main/secrets.h.template` | adapt | Drop MS_HOST/PORT, add PP_HOST/PORT |
| `main/app_config.c` + `.h` | replace | Channel selection has no equivalent; new pref schema |
| `main/app_ms_client_ws.c` | replace | New `app_pp_client.c` |
| `main/app_ms_client_osc.c` | drop | No OSC equivalent on PP |
| `main/app_state.c` + `.h` | replace | New `app_pp_state.c` |
| `main/app_ui.c` | rewrite main page | Settings overlay, splash, theme apply: keep. Fader page: gone. |
| `main/app_touch_inject.c` | trim | Keep tap rotation map; drop fader helpers |
| `main/fonts/font_monmix_level.c` | drop | Fader-specific glyph subset |
| `main/images/splash_logo.c` | reskin | Same mechanism, new asset |
| `pc_sim/` | adapt mocks | Mock PP client replaces mock MS client; same SDL infra |
| `tests/sim/`, `tests/hw/`, `tests/unit/` | adapt cases | Test grammar unchanged; new test cases for slide-changed, timer-tick, stage-message-shown |
| `tools/fetch_b64.py`, `tools/fetch_screenshot.py`, `tools/run_steps.py`, `tools/soak.py`, `tools/panic_watch.py` | ✓ as-is | All target the UART REPL, protocol-agnostic |
| `tools/git-hooks/pre-commit` | ✓ as-is | Credential macro check |
| Build config (`sdkconfig.defaults` etc.) | mostly as-is | `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=32`, mempool, console baud, coredump-to-flash all keep |
| `host_performs_slave_ota/` | ✓ as-is | Same C6 OTA path |
| `case/` (3D-printed enclosure) | ✓ or re-cut | Orientation tooling may need a portrait-friendly variant |

**Net new code** is roughly: `app_pp_client.c` (~600 LOC, much of it
ported chunked-stream handling from the existing
`esp_websocket_client` usage pattern), `app_pp_state.c` (~250 LOC,
mirror of `app_state.c`), `app_pp_ui.c` (~800 LOC, new layout), plus
~150 LOC of rotation-extension diffs across `app_display.c` and
`app_prefs.c`. Maybe 1800 LOC of new code total — order of magnitude
smaller than monmix from scratch.

---

## 7 — Recommended next steps (only if user wants to proceed)

1. **Probe scripts against a live ProPresenter instance** —
   resolves all of section 4 in one bench session. ~30 min. Output:
   `repro_pp_*.py` scripts that exercise each endpoint and dump the
   JSON shapes; findings written back into this doc.

2. **Extend rotation to 90/270 on monmix master first.** Useful
   regardless — could be a polish item even if the PP product doesn't
   happen. Self-contained PR.

3. **Spike `app_pp_client.c`** — just the chunked subscriber, parsing,
   and logging-to-UART. No UI yet. ~one session of work; produces a
   demonstrable "ProPresenter says current slide is X, next is Y" on
   the REPL. This is the moment of truth for the streaming-endpoint
   approach.

4. **Build the UI page in `pc_sim` first.** Faster iteration than
   flashing, and the SDL renderer handles JPEG decoding via SDL_image
   so we can mock the thumbnail path against any local PNG/JPEG. Lock
   the layout there before touching the device. **Render real slide
   payloads in both landscape and portrait layouts** against a sample
   setlist and pick the orientation on actual content — not geometry
   guesswork.

5. **Flash + bench.** Confirms rotation, JPEG decode latency, layout
   readability from a music-stand distance.

6. **Repo question for later** — does this live in `monmix/` as a
   sibling project (`stage_view/`), or as a separate repo? Sibling
   gets the existing tooling and dev habits for free; separate has
   cleaner branding if it's ever shared/sold. Defer until after the
   spike confirms viability.

---

## 8 — Artifacts in this worktree

- `INVESTIGATION.md` — this document
- `tools/probe_pp.py` — read-only probe driving all 8 open empirical
  questions in section 4. Run as
  `python tools/probe_pp.py <host> [port]` against a live ProPresenter;
  drops findings under `probe-out/<timestamp>/`. Iterate as Section 4
  answers come in.
- `docs/raw/pro7.9.openapi-spec.json` — stale-but-useful 7.9 mirror
- `docs/raw/propresenter-lib/propresenter.ts` — npm wrapper source
  (the authoritative latest-endpoint catalogue at the time of writing)
- `docs/raw/bitfocus/` — Companion module source, demonstrates
  production usage of the streaming endpoint

Nothing has been committed on the `propresenter-investigation` branch
yet. The worktree itself is gitignored at the umbrella level
(`.copilot/` is in `.gitignore`). Commit + push only on request.
