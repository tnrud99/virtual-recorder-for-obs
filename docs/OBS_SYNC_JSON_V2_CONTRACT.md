# OBS Sync JSON v2 Contract

This contract describes the sync JSON produced by the VirtuEdit OBS plugin and
the minimum behavior expected from the editor, server, and viewer before
release. The goal is to keep OBS exports round-trippable while other surfaces
gradually add custom overlay support.

## Producer

OBS plugin exports `schema_version: 2`.

The OBS plugin is authoritative for:

- YouTube video identity and timing.
- Reaction-only output filename.
- OBS combined output filename, when OBS reports it.
- Custom PiP overlay coordinates.
- YouTube relative volume.

## Consumers

- Editor import/export.
- Creator upload flow.
- Server settings and timestamp storage.
- Viewer playback renderer.

All consumers must preserve unknown top-level and `layout` fields during
load, edit, upload, save, and re-export. Dropping unknown fields is a schema
bug even if a surface cannot render them yet.

## Top-Level Fields

Required fields:

```json
{
  "schema_version": 2,
  "youtube_video_id": "dQw4w9WgXcQ",
  "youtube_title": "Example",
  "reaction_video": "260602_sample-title_reaction.mp4",
  "composite_video": "260602_sample-title_combined.mp4",
  "created_at": "2026-06-02T20:00:00Z",
  "source": "obs-plugin",
  "layout": {},
  "sync_points": []
}
```

Field semantics:

- `reaction_video` is the local filename of the reaction-only MP4. The editor
  should first resolve it next to the JSON file, then ask the user to reconnect
  it if missing.
- `composite_video` is the local filename of the OBS combined recording. It is a
  secondary upload asset. It may be empty if OBS never reported a final path.
- `youtube_video_id` is the source of truth for loading the original video.
- `source: "obs-plugin"` identifies OBS-produced sync data.
- `created_at` is informational and should be preserved.

## Layout Fields

OBS exports the original YouTube video as a PiP over the reaction video:

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
    "overlay_height": 0.45,
    "youtube_volume": 50,
    "hide_overlay": false,
    "youtube_captions": false,
    "youtube_caption_language": "auto"
  }
}
```

Coordinate rules:

- `overlay_x/y/width/height` are normalized to the displayed reaction video
  box, not the browser viewport.
- The YouTube PiP is aspect-locked to 16:9 in pixel space. Normalized
  `overlay_width` and `overlay_height` may differ on non-16:9 canvases.
- `overlay_preset_fallback` must be the closest preset-only placement to the
  custom rectangle. Valid values are `top-left`, `top-center`, `top-right`,
  `middle-left`, `middle-right`, `bottom-left`, `bottom-center`, and
  `bottom-right`.
- Desktop and mobile must use the same canonical coordinates unless a future
  explicit mobile layout mode is introduced.
- Consumers must not quietly move a valid custom layout to a different mobile
  position.

Rendering priority:

1. If `overlay_position === "custom"` and all custom coordinates are valid,
   render the custom coordinates.
2. Otherwise render the known preset in `overlay_position`.
3. Otherwise render `overlay_preset_fallback`.
4. Otherwise use the product default preset.

Custom-coordinate behavior:

- OBS should preserve the PiP's 16:9 aspect ratio before exporting custom
  coordinates.
- OBS should warn, but not block, when the user's OBS canvas or output is not
  close to 16:9.
- If custom coordinates are invalid, a consumer may fall back to
  `overlay_preset_fallback` and should preserve the original invalid fields.

## Sync Points

`sync_points` must contain at least `start` and `end`. OBS exports these event
names:

- `start`
- `youtube_play`
- `youtube_pause`
- `youtube_ended`
- `end`

Timing fields:

- `reaction_time` is seconds from reaction recording start.
- `youtube_time` is seconds in the YouTube video, or `null`.
- `relative_youtube_time` matches `youtube_time` for OBS exports, or `null`.
- `youtube_first_play_time` is seconds from reaction start to the first
  YouTube play, or `null`.

Consumers should preserve unknown future event fields. They should reject or
surface malformed timing data rather than silently rewriting sync points.

## Editor Requirements

On import:

- Load `reaction_video` relative to the JSON file when possible.
- Preserve `composite_video` as a secondary asset reference.
- Store all `layout` fields, including unknown fields.
- Render custom overlay coordinates when available, even before drag UI exists.
- If only preset UI is available, display the fallback preset but keep custom
  fields in memory and on export.

On export:

- Keep `schema_version: 2` unless intentionally migrating.
- Preserve custom overlay fields.
- Do not replace `overlay_position: "custom"` with a preset unless the user
  explicitly chooses a preset.
- Preserve unknown top-level, `layout`, and sync point fields.

## Upload And Server Requirements

Creator upload flow:

- Copy OBS `layout` into upload settings without dropping custom fields.
- Until every viewer surface renders custom coordinates directly,
  `settings.overlay_position` may be the preset fallback used by the current
  viewer UI. The canonical custom placement must remain in
  `timestamp_data.layout.overlay_position: "custom"` and in the preserved
  `overlay_x/y/width/height` fields.
- Keep `reaction_video` and `composite_video` as separate concepts.
- If the combined video is uploaded, mark it as a secondary ready-to-upload/mixed
  output, not as the editor source of truth.

Server:

- Allow the custom overlay fields in stored settings.
- Preserve unknown settings and timestamp fields on partial updates.
- When editing metadata such as title, category, captions, or volume, merge
  settings instead of replacing the layout object.

Viewer:

- Prefer custom coordinates from stored settings.
- If settings do not have custom coordinates, use `timestamp_data.layout`.
- If custom is not renderable, use `overlay_preset_fallback`.
- Apply coordinates relative to the reaction video display box.

## Compatibility Rules

- Never discard unknown fields.
- Never silently convert a valid custom layout into a preset layout.
- Never make mobile render a different position from desktop unless the JSON
  explicitly includes a future mobile-specific layout mode.
- Do not treat `composite_video` as a replacement for `reaction_video`.
- Do not require viewer/editor custom-coordinate UI before preserving custom
  data.

## Release Tests

- Validate OBS JSON with `tests/validate-obs-sync-json.mjs`.
- Import an OBS JSON into the editor and export it again; custom layout and
  fields must survive byte-for-field semantically.
- Upload/update metadata and confirm layout fields are not dropped.
- Render desktop and mobile viewer with the same valid custom layout.
- Confirm missing `reaction_video` triggers reconnect UX, not data loss.
- Confirm missing `composite_video` does not block editing.
