# Viewer/Editor Custom Overlay TODO

The OBS plugin exports the original YouTube video as a custom-position PiP over the reaction video.
Current viewer/editor surfaces still use preset overlay positions, so they need a follow-up pass to consume the OBS layout precisely.

See `OBS_SYNC_JSON_V2_CONTRACT.md` for the full JSON contract.

## Data Contract

OBS sync JSON layout fields:

```json
{
  "layout": {
    "mode": "overlay",
    "view_mode": "overlay",
    "overlay_target": "original",
    "overlay_position": "custom",
    "overlay_preset_fallback": "top-right",
    "overlay_size": 45,
    "overlay_x": 0.52,
    "overlay_y": 0.05,
    "overlay_width": 0.45,
    "overlay_height": 0.45
  }
}
```

Coordinate semantics:

- `overlay_target: "original"` means the YouTube original is overlaid on the reaction video.
- `overlay_x/y/width/height` are normalized to the displayed reaction video box, not the full browser viewport.
- The YouTube PiP should render at 16:9 in pixel space. On non-16:9 videos,
  normalized `overlay_width` and `overlay_height` may not match numerically.
- `overlay_preset_fallback` is for current preset-only viewer/editor behavior
  and should be one of the eight supported presets nearest to the custom
  rectangle.
- `overlay_size` is the width percentage fallback for old preset UI.

## Viewer Work

- Preserve current preset behavior when `overlay_position` is one of the known preset names.
- When `overlay_position === "custom"`, apply:
  - `left: overlay_x * 100%`
  - `top: overlay_y * 100%`
  - `width: overlay_width * 100%`
  - `height: overlay_height * 100%` or aspect-ratio-compatible equivalent
- Apply the custom coordinates relative to the reaction video display box.
- Avoid viewport-relative placement so letterboxing, theater mode, and mobile layout do not drift.
- Keep YouTube iframe controls from becoming the primary UX for small PiP sizes.
- Keep fullscreen behavior constrained; the original iframe should not break out of the synchronized viewer layout.

## Editor Work

- `LayoutManager.loadLayoutFromData()` should read and store `overlay_x/y/width/height` when `overlay_position === "custom"`.
- `LayoutManager.getLayout()` should preserve custom fields on export.
- The editor preview should render custom coordinates before the drag UI exists.
- Later, add drag/resize controls that update the custom coordinate fields.
- If the user switches from custom to a preset, keep custom fields in memory if possible, but export the selected preset as authoritative.

## Upload/Server Work

- `ve-creator.html` should copy custom overlay fields from `timestampData.layout` into `settings`.
- `server.js` `VEUrl.settings` schema should allow:
  - `overlay_target`
  - `overlay_preset_fallback`
  - `overlay_x`
  - `overlay_y`
  - `overlay_width`
  - `overlay_height`
- Viewer should prefer custom fields from `settings`, with fallback to `timestamp_data.layout`, then `overlay_preset_fallback`.

## Compatibility Rule

Never discard unknown layout fields during load/export/update. OBS sync files should remain round-trippable even before every surface can render custom coordinates.
