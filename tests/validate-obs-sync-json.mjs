#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const files = process.argv.slice(2);
if (!files.length) {
  console.error('Usage: node tests/validate-obs-sync-json.mjs <sync-json> [...]');
  process.exit(2);
}

const allowedEvents = new Set([
  'start',
  'youtube_play',
  'youtube_pause',
  'youtube_ended',
  'end'
]);

const allowedOverlayFallbacks = new Set([
  'top-left',
  'top-center',
  'top-right',
  'middle-left',
  'middle-right',
  'bottom-left',
  'bottom-center',
  'bottom-right'
]);

let failures = 0;

function fail(file, message) {
  failures += 1;
  console.error(`[FAIL] ${file}: ${message}`);
}

function isFiniteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function requireString(file, obj, key) {
  if (typeof obj[key] !== 'string') {
    fail(file, `${key} must be a string`);
  }
}

function requireNumber(file, obj, key, min, max) {
  if (!isFiniteNumber(obj[key])) {
    fail(file, `${key} must be a finite number`);
    return;
  }
  if (obj[key] < min || obj[key] > max) {
    fail(file, `${key} must be between ${min} and ${max}`);
  }
}

function validateLayout(file, layout) {
  if (!layout || typeof layout !== 'object' || Array.isArray(layout)) {
    fail(file, 'layout must be an object');
    return;
  }

  const expectedStrings = {
    mode: 'overlay',
    overlay_target: 'original',
    overlay_position: 'custom'
  };
  for (const [key, expected] of Object.entries(expectedStrings)) {
    if (layout[key] !== expected) {
      fail(file, `layout.${key} must be "${expected}"`);
    }
  }
  if (!allowedOverlayFallbacks.has(layout.overlay_preset_fallback)) {
    fail(file, 'layout.overlay_preset_fallback must be a known preset fallback');
  }

  requireNumber(file, layout, 'overlay_x', 0, 1);
  requireNumber(file, layout, 'overlay_y', 0, 1);
  requireNumber(file, layout, 'overlay_width', 0.01, 1);
  requireNumber(file, layout, 'overlay_height', 0.01, 1);
  requireNumber(file, layout, 'overlay_size', 1, 100);
  requireNumber(file, layout, 'youtube_volume', 0, 100);

  if (isFiniteNumber(layout.overlay_x) && isFiniteNumber(layout.overlay_width) &&
      layout.overlay_x + layout.overlay_width > 1.01) {
    fail(file, 'layout overlay exceeds horizontal bounds');
  }
  if (isFiniteNumber(layout.overlay_y) && isFiniteNumber(layout.overlay_height) &&
      layout.overlay_y + layout.overlay_height > 1.01) {
    fail(file, 'layout overlay exceeds vertical bounds');
  }
}

function validateSyncPoints(file, syncPoints) {
  if (!Array.isArray(syncPoints) || syncPoints.length < 2) {
    fail(file, 'sync_points must contain at least start and end points');
    return;
  }

  if (syncPoints[0].event !== 'start') {
    fail(file, 'first sync point must be start');
  }
  if (syncPoints[syncPoints.length - 1].event !== 'end') {
    fail(file, 'last sync point must be end');
  }

  let prevReactionTime = -Infinity;
  let playCount = 0;
  let pauseCount = 0;

  syncPoints.forEach((point, index) => {
    if (!point || typeof point !== 'object' || Array.isArray(point)) {
      fail(file, `sync_points[${index}] must be an object`);
      return;
    }
    if (!allowedEvents.has(point.event)) {
      fail(file, `sync_points[${index}].event is unsupported: ${point.event}`);
    }
    if (!isFiniteNumber(point.reaction_time) || point.reaction_time < 0) {
      fail(file, `sync_points[${index}].reaction_time must be a non-negative number`);
    } else if (point.reaction_time < prevReactionTime) {
      fail(file, `sync_points[${index}].reaction_time must be monotonic`);
    } else {
      prevReactionTime = point.reaction_time;
    }

    if (point.youtube_time !== null && !isFiniteNumber(point.youtube_time)) {
      fail(file, `sync_points[${index}].youtube_time must be number or null`);
    }
    if (point.relative_youtube_time !== null && !isFiniteNumber(point.relative_youtube_time)) {
      fail(file, `sync_points[${index}].relative_youtube_time must be number or null`);
    }
    if (point.youtube_time !== null && point.relative_youtube_time !== point.youtube_time) {
      fail(file, `sync_points[${index}] youtube_time and relative_youtube_time should match for OBS exports`);
    }
    if (point.youtube_first_play_time !== null && !isFiniteNumber(point.youtube_first_play_time)) {
      fail(file, `sync_points[${index}].youtube_first_play_time must be number or null`);
    }

    if (point.event === 'youtube_play') playCount += 1;
    if (point.event === 'youtube_pause' || point.event === 'youtube_ended') pauseCount += 1;
  });

  if (playCount === 0) fail(file, 'sync_points must include at least one youtube_play');
  if (pauseCount === 0) fail(file, 'sync_points must include at least one youtube_pause/youtube_ended');
}

function validateFile(file) {
  let data;
  try {
    data = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    fail(file, `invalid JSON: ${error.message}`);
    return;
  }

  if (data.schema_version !== 2) fail(file, 'schema_version must be 2');
  if (data.source !== 'obs-plugin') fail(file, 'source must be obs-plugin');
  requireString(file, data, 'youtube_video_id');
  requireString(file, data, 'youtube_title');
  requireString(file, data, 'reaction_video');
  requireString(file, data, 'composite_video');
  requireString(file, data, 'created_at');

  if (!/_reaction\.(mp4|mkv)$/i.test(data.reaction_video)) {
    fail(file, 'reaction_video should end with _reaction.mp4 or _reaction.mkv');
  }

  validateLayout(file, data.layout);
  validateSyncPoints(file, data.sync_points);

  console.log(`[OK] ${path.relative(process.cwd(), file)}`);
}

for (const file of files) {
  validateFile(file);
}

if (failures > 0) {
  console.error(`${failures} validation failure(s)`);
  process.exit(1);
}
