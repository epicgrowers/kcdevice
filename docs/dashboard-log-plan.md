# Dashboard Log Terminal Plan

## Goals
- Present device logs inside the dashboard using a terminal-inspired panel that feels familiar to engineers.
- Improve readability by color-coding log lines based on their tag (e.g., `EZO_SENSOR`, `MQTT_TELEM`).
- Preserve the chronological stream while making it obvious when new data arrives.

## Current Behavior
- Logs render as plain text with minimal styling.
- Tag names repeat on each line but do not stand out visually.
- Users need to scan entire lines to locate relevant subsystems.

## Proposed UI
- **Container:** Full-width card with black background (#0b0d0f) and subtle rounded corners.
- **Font:** Use a monospace family (e.g., "JetBrains Mono", "Fira Mono", fallback to `monospace`).
- **Scrollbar:** Always visible vertical scrollbar; horizontal scrolling if line exceeds available width.
- **Line Layout:** `[timestamp] [tag] message` where timestamp stays dim gray (#7a7d80), tag is color-coded pill, message remains light gray (#e6e7e9).
- **Auto-scroll:** Stick to bottom when the user is already at the end; pause auto-scroll if user scrolls upward.

## Color Mapping
| Tag Prefix | Color | Notes |
|------------|-------|-------|
| `EZO_SENSOR` | `#48d597` (green) | Sensor read/commands |
| `SENSOR_MGR` | `#5fe3c2` (aqua) | Cache + sampling |
| `MQTT_TELEM` | `#7bb7ff` (blue) | Telemetry publish results |
| `MQTT_CONN` | `#4f8bff` (azure) | Connection supervisor |
| `PROV_RUN` | `#f4c361` (amber) | Provisioning/wifi state |
| `WIFI_MGR` | `#f28f5c` (orange) | Wi-Fi lifecycle |
| `BOOT_HANDLERS` | `#f1556c` (rose) | Boot progress |
| `SECURITY` | `#d78bff` (violet) | Crypto/storage notices |
| `NETWORK_BOOT` | `#ffaaae` (salmon) | Network service orchestration |
| `default` | `#c5c7ca` | Any other tag |

Implementation detail: maintain a mapping in the frontend and fall back to the default color when the tag is unknown.

## Interaction Ideas
- Default filter chips per subsystem:
	- `MQTT` (covers `MQTT_TELEM`, `MQTT_CONN`, future MQTT_* tags).
	- `SENSORS` (covers `EZO_SENSOR`, `SENSOR_MGR`, `SENSOR_BOOT`, etc.).
	- `WIFI/PROVISIONING`, `SECURITY`, `BOOT/NETWORK`, plus `ALL` and `CUSTOM` views.
- Users can define custom groups in settings (see "Custom Grouping" below) and pin them next to the default chips.
- Quick jump to latest log button.
- Copy line icon on hover for easy sharing.

### Custom Grouping
- Provide a small dialog or drawer in dashboard settings where users map tag prefixes/regex to friendly names.
- Persist preferences per user (localStorage or per-account backend record) so the layout survives refreshes.
- When a log arrives, resolve it against custom groups first; fall back to default buckets; if none match, it lands under `OTHER`.

## Additional Logging Features
- **Search & Highlight:** Inline search bar supporting substring or regex; highlight matches and allow next/previous navigation so users can jump between occurrences quickly.
- **Severity Badges:** Auto-detect severity prefixes (`E`, `W`, `I`, `D`) and add compact glyphs or background tints so critical errors stand out at a glance.
- **Pinned Bookmarks:** Users can pin important lines (e.g., first error) and revisit them via a bookmark tray without losing live context.
- **Download & Share:** Export the visible buffer as text/JSON and generate shareable links that remember filters and scroll offsets for collaboration.
- **Live Stats Strip:** Small footer summarizing metrics like lines/minute, time since last error, and current connection status to give ongoing situational awareness.
- **Client Alerts:** Allow opt-in rules ("notify me if `PROV_RUN` emits 'failed'") that trigger lightweight toasts or sounds, reducing the need to watch continuously.
- **Tag Legend:** Collapsible drawer listing every tag currently in the buffer, its color chip, and line count; clicking a legend item toggles that tag’s visibility.
- **Time Slicing:** Quick presets (last 5/15/60 minutes) plus a calendar/time picker so operators can jump directly to a suspected incident window.
- **Context Mode:** Selecting a line reveals ±10 surrounding entries even if filters hide them, preventing loss of context during deep dives.
- **Session Diffing:** Let users set a baseline marker (e.g., post-boot) and later highlight only the lines that arrived after that point.
- **Performance Overlay:** Optional HUD showing ingest rate, UI frame time, and buffer depth to spot when the browser is falling behind.
- **Inline Actions:** Detect log lines tied to device actions (e.g., Wi-Fi retries) and surface contextual buttons so operators can jump straight to the relevant control panel or trigger a retry without leaving the log view.
- **Correlation Highlights:** When hovering a log line, highlight related entries (e.g., link `MQTT_TELEM` publishes back to the `SENSOR_MGR` snapshot they used) to visualize causal chains without manual searching.

## Next Ideas
- **Share-link hydration:** When someone opens a `?log=` link we generate, auto-apply the encoded filters, search query, time slice, and hidden tags before rendering so the recipient lands in the same investigative context.
- **Virtualized timeline:** Introduce a simple virtualization strategy (e.g., windowed rendering) so the UI stays responsive even when the session buffer grows well beyond 2,000 entries.
- **Alert-to-line navigation:** Make alert toast notifications actionable—clicking one should jump to the matching log line, open its context window, and highlight it for quick triage.

## Data Flow Considerations
1. **Source:** Logs already streamed to the dashboard via WebSocket or HTTP long-poll (confirm actual transport).
2. **Parser:** Split payload into `{ timestamp, tag, message }`. If upstream format varies, create a normalization helper.
3. **State:** Keep a bounded ring buffer (e.g., last 2,000 lines) to avoid memory bloat.
4. **Rendering:** Virtualize the list (e.g., react-window) once line count grows to keep scrolling smooth.

## Edge Cases
- **Burst Logs:** When more than 200 lines arrive in one payload, push them in chunks to keep render responsive.
- **Long Lines:** Wrap text but also allow horizontal scroll for those who prefer single-line view.
- **No Data:** Show placeholder "Waiting for device logs…" with subtle pulse animation.
- **Disconnected:** Overlay warning banner when websocket disconnects; retry automatically.

## Open Questions
1. Do we have a canonical list of log tags or should the UI handle arbitrary values?
2. Should users be able to customize colors per tag?
3. How far back should the dashboard retain logs (session only vs persistent storage)?
4. Is there an audit/security requirement to redact certain tags before showing them?
