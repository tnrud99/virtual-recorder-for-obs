#include "scene-manager.hpp"
#include "audio-track-selection.hpp"
#include "obs-utils.hpp"
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

static const char *SCENE_SOURCE_NAME = "Virtual Recorder Scene";
static const char *LEGACY_SCENE_SOURCE_NAME = "VirtuEdit Reaction";
static const char *WEBCAM_SOURCE_NAME = "Virtual Recorder Webcam";
static const char *LEGACY_WEBCAM_SOURCE_NAME = "VirtuEdit-Webcam";
static const char *MIC_SOURCE_NAME = "Virtual Recorder Mic";
static const char *LEGACY_MIC_SOURCE_NAME = "VirtuEdit-Mic";
static const char *BROWSER_SOURCE_NAME = "Virtual Recorder Player Controls";
static const char *MANAGED_MARKER_KEY = "virtuedit_managed";
static const char *PREVIOUS_BROWSER_SOURCE_NAME =
	"Control YouTube Here While Recording";
static const char *OLDER_PREVIOUS_BROWSER_SOURCE_NAME_2 =
	"Watch YouTube Here for Sync";
static const char *OLDER_PREVIOUS_BROWSER_SOURCE_NAME =
	"Virtual Recorder Synced Player";
static const char *OLDER_BROWSER_SOURCE_NAME_VR =
	"Virtual Recorder YouTube Player";
static const char *LEGACY_BROWSER_SOURCE_NAME = "VirtuEdit YouTube Player";
static const char *OLDER_BROWSER_SOURCE_NAME = "VirtuEdit - Watch Video";
static const char *OLDEST_BROWSER_SOURCE_NAME = "VirtuEdit-YouTube";
static const float DEFAULT_PIP_SIZE = 0.45f;
static const float DEFAULT_PIP_RIGHT_MARGIN = 0.03f;
static const float DEFAULT_PIP_TOP = 0.05f;
static const float PIP_ASPECT = 16.0f / 9.0f;
static const float TRANSFORM_EPSILON = 0.5f;

static float pipHeightForWidth(float width)
{
	return width / PIP_ASPECT;
}

static bool nearlyEqual(float a, float b)
{
	return std::fabs(a - b) <= TRANSFORM_EPSILON;
}

static float clampFloat(float value, float minValue, float maxValue)
{
	return std::max(minValue, std::min(value, maxValue));
}

static int clampInt(int value, int minValue, int maxValue)
{
	return std::max(minValue, std::min(value, maxValue));
}

static bool asciiEqualsIgnoreCase(const char *left, const char *right)
{
	if (!left || !right)
		return false;
	while (*left && *right) {
		const unsigned char leftChar = static_cast<unsigned char>(*left++);
		const unsigned char rightChar = static_cast<unsigned char>(*right++);
		if (std::tolower(leftChar) != std::tolower(rightChar))
			return false;
	}
	return *left == '\0' && *right == '\0';
}

static uint32_t protectedObsAudioMixerMask()
{
	static constexpr uint32_t kAllAudioMixers = (1u << 6) - 1u;
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return kAllAudioMixers;

	const char *mode = config_get_string(config, "Output", "Mode");
	const bool advanced = asciiEqualsIgnoreCase(mode, "Advanced");
	uint32_t protectedMask = 0;

	if (advanced) {
		// Keep configured stream and VOD tracks untouched as users may stream
		// while Virtual Recorder owns the normal OBS recording output.
		protectedMask |= virtuedit_audio_track_bit(
			config_get_int(config, "AdvOut", "TrackIndex"));
		protectedMask |= virtuedit_audio_track_bit(
			config_get_int(config, "AdvOut", "VodTrackIndex"));

		const char *recordingType =
			config_get_string(config, "AdvOut", "RecType");
		if (asciiEqualsIgnoreCase(recordingType, "FFmpeg")) {
			protectedMask |= static_cast<uint32_t>(config_get_uint(
				config, "AdvOut", "FFAudioMixes"));
		} else {
			const char *format =
				config_get_string(config, "AdvOut", "RecFormat2");
			if (!format || !*format)
				format = config_get_string(config, "AdvOut",
							   "RecFormat");
			if (asciiEqualsIgnoreCase(format, "flv")) {
				protectedMask |= virtuedit_audio_track_bit(config_get_int(
					config, "AdvOut", "FLVTrack"));
			} else {
				uint32_t recordingMask = static_cast<uint32_t>(
					config_get_uint(config, "AdvOut", "RecTracks"));
				if (recordingMask == 0) {
					// Match OBS's legacy RecTracks fallback behavior.
					recordingMask = static_cast<uint32_t>(
						config_get_uint(config, "AdvOut",
								"TrackIndex"));
				}
				protectedMask |= recordingMask;
			}
		}
	} else {
		// Simple streaming uses mixer 1 and its archive/VOD encoder uses mixer 2.
		protectedMask |= virtuedit_audio_track_bit(1) |
				 virtuedit_audio_track_bit(2);

		const char *format =
			config_get_string(config, "SimpleOutput", "RecFormat2");
		if (!format || !*format)
			format = config_get_string(config, "SimpleOutput", "RecFormat");
		const char *quality =
			config_get_string(config, "SimpleOutput", "RecQuality");
		if (asciiEqualsIgnoreCase(format, "flv") ||
		    asciiEqualsIgnoreCase(quality, "Stream")) {
			protectedMask |= virtuedit_audio_track_bit(1);
		} else {
			protectedMask |= static_cast<uint32_t>(config_get_uint(
				config, "SimpleOutput", "RecTracks"));
		}
	}

	return protectedMask & kAllAudioMixers;
}

static std::string sanitizeReactionOutputFormat(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char ch) {
			       return static_cast<char>(std::tolower(ch));
		       });
	if (value == "mp4" || value == "mov" || value == "mkv")
		return value;
	return "";
}

static bool isManagedVirtualRecorderSource(const char *name)
{
	if (!name)
		return false;
	return std::strcmp(name, SCENE_SOURCE_NAME) == 0 ||
	       std::strcmp(name, WEBCAM_SOURCE_NAME) == 0 ||
	       std::strcmp(name, MIC_SOURCE_NAME) == 0 ||
	       std::strcmp(name, BROWSER_SOURCE_NAME) == 0 ||
	       std::strcmp(name, PREVIOUS_BROWSER_SOURCE_NAME) == 0 ||
	       std::strcmp(name, OLDER_PREVIOUS_BROWSER_SOURCE_NAME_2) == 0 ||
	       std::strcmp(name, OLDER_PREVIOUS_BROWSER_SOURCE_NAME) == 0 ||
	       std::strcmp(name, OLDER_BROWSER_SOURCE_NAME_VR) == 0 ||
	       std::strcmp(name, OLDER_BROWSER_SOURCE_NAME) == 0 ||
	       std::strcmp(name, OLDEST_BROWSER_SOURCE_NAME) == 0 ||
	       std::strcmp(name, LEGACY_SCENE_SOURCE_NAME) == 0 ||
	       std::strcmp(name, LEGACY_WEBCAM_SOURCE_NAME) == 0 ||
	       std::strcmp(name, LEGACY_MIC_SOURCE_NAME) == 0 ||
	       std::strcmp(name, LEGACY_BROWSER_SOURCE_NAME) == 0;
}

static bool nameMatchesBaseOrNumberedCopy(const char *name, const char *base)
{
	if (!name || !base)
		return false;
	if (std::strcmp(name, base) == 0)
		return true;

	const size_t baseLen = std::strlen(base);
	if (std::strncmp(name, base, baseLen) != 0)
		return false;
	if (name[baseLen] != ' ')
		return false;

	const char *suffix = name + baseLen + 1;
	if (!*suffix)
		return false;
	while (*suffix) {
		if (*suffix < '0' || *suffix > '9')
			return false;
		suffix++;
	}
	return true;
}

static bool isManagedPlayerControlsName(const char *name)
{
	return nameMatchesBaseOrNumberedCopy(name, BROWSER_SOURCE_NAME) ||
	       nameMatchesBaseOrNumberedCopy(name, PREVIOUS_BROWSER_SOURCE_NAME) ||
	       nameMatchesBaseOrNumberedCopy(name, OLDER_PREVIOUS_BROWSER_SOURCE_NAME_2) ||
	       nameMatchesBaseOrNumberedCopy(name, OLDER_PREVIOUS_BROWSER_SOURCE_NAME) ||
	       nameMatchesBaseOrNumberedCopy(name, OLDER_BROWSER_SOURCE_NAME_VR) ||
	       nameMatchesBaseOrNumberedCopy(name, OLDER_BROWSER_SOURCE_NAME) ||
	       nameMatchesBaseOrNumberedCopy(name, OLDEST_BROWSER_SOURCE_NAME) ||
	       nameMatchesBaseOrNumberedCopy(name, LEGACY_BROWSER_SOURCE_NAME);
}

static bool isBrowserLikeSource(const char *id)
{
	return id && std::strcmp(id, "browser_source") == 0;
}

static bool hasManagedPlayerControlsMarker(obs_source_t *source)
{
	if (!source)
		return false;

	const char *id = obs_source_get_unversioned_id(source);
	if (!isBrowserLikeSource(id))
		return false;

	obs_data_t *settings = obs_source_get_settings(source);
	bool managed = settings &&
		       obs_data_get_bool(settings, MANAGED_MARKER_KEY);
	if (settings)
		obs_data_release(settings);
	return managed;
}

struct PlayerControlsEnumCtx {
	SceneManager::PlayerControlsState state;
};

struct PlayerControlsItemCtx {
	std::vector<obs_sceneitem_t *> items;
};

static bool enumPlayerControlsItems(obs_scene_t *, obs_sceneitem_t *item,
				    void *param)
{
	auto *ctx = static_cast<PlayerControlsEnumCtx *>(param);
	if (!ctx || !item)
		return true;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (hasManagedPlayerControlsMarker(src)) {
		ctx->state.count++;
		if (obs_sceneitem_visible(item))
			ctx->state.visibleCount++;
		else
			ctx->state.hasHidden = true;
	}

	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, enumPlayerControlsItems,
					       param);

	return true;
}

static bool collectPlayerControlsItems(obs_scene_t *, obs_sceneitem_t *item,
				       void *param)
{
	auto *ctx = static_cast<PlayerControlsItemCtx *>(param);
	if (!ctx || !item)
		return true;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (hasManagedPlayerControlsMarker(src)) {
		obs_sceneitem_addref(item);
		ctx->items.push_back(item);
	}

	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item,
					       collectPlayerControlsItems,
					       param);

	return true;
}

static bool isCaptureDeviceSource(const char *id)
{
	if (!id)
		return false;
	return std::strcmp(id, "av_capture_input") == 0 ||
	       std::strcmp(id, "dshow_input") == 0 ||
	       std::strcmp(id, "v4l2_input") == 0 ||
	       std::strcmp(id, "coreaudio_input_capture") == 0 ||
	       std::strcmp(id, "wasapi_input_capture") == 0 ||
	       std::strcmp(id, "pulse_input_capture") == 0 ||
	       std::strcmp(id, "alsa_input_capture") == 0;
}

static bool isManagedCaptureSourceName(const char *name)
{
	if (!name)
		return false;
	return std::strcmp(name, WEBCAM_SOURCE_NAME) == 0 ||
	       std::strcmp(name, LEGACY_WEBCAM_SOURCE_NAME) == 0 ||
	       std::strcmp(name, MIC_SOURCE_NAME) == 0 ||
	       std::strcmp(name, LEGACY_MIC_SOURCE_NAME) == 0;
}

static SceneManager::OverlayPos defaultOverlayPosition()
{
	auto canvas = virtuedit_get_canvas_size();
	float canvasW = static_cast<float>(canvas.width);
	float canvasH = static_cast<float>(canvas.height);
	float h = DEFAULT_PIP_SIZE;
	if (canvasW > 0.0f && canvasH > 0.0f)
		h = pipHeightForWidth(canvasW * DEFAULT_PIP_SIZE) / canvasH;

	return {1.0f - DEFAULT_PIP_SIZE - DEFAULT_PIP_RIGHT_MARGIN,
		DEFAULT_PIP_TOP, DEFAULT_PIP_SIZE, h};
}

SceneManager::SceneManager()
{
	loadConfig();
}

// --- Config persistence (UUID tracking) ---

void SceneManager::saveConfig()
{
	char *path = obs_module_config_path("config.json");
	if (!path)
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "scene_uuid", sceneUuid.c_str());
	obs_data_set_string(data, "webcam_uuid", webcamUuid.c_str());
	obs_data_set_string(data, "mic_uuid", micUuid.c_str());
	obs_data_array_t *audioArr = obs_data_array_create();
	for (const std::string &uuid : reactionAudioUuids) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "uuid", uuid.c_str());
		obs_data_array_push_back(audioArr, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(data, "reaction_audio_source_uuids", audioArr);
	obs_data_array_release(audioArr);
	obs_data_set_string(data, "browser_uuid", browserUuid.c_str());
	obs_data_set_bool(data, "webcam_uses_existing_source",
			  webcamUsesExistingSource);
	obs_data_set_bool(data, "mic_uses_existing_source",
			  micUsesExistingSource);
	obs_data_set_int(data, "youtube_volume", savedVolume);
	obs_data_set_bool(data, "reaction_encoder_custom",
			  reactionEncoderCustom.enabled);
	obs_data_set_int(data, "reaction_encoder_crf",
			 reactionEncoderCustom.crf);
	obs_data_set_int(data, "reaction_encoder_video_bitrate",
			 reactionEncoderCustom.videoBitrate);
	obs_data_set_int(data, "reaction_encoder_audio_bitrate",
			 reactionEncoderCustom.audioBitrate);
	obs_data_set_string(data, "reaction_output_format",
			    reactionOutputFormat.c_str());
	obs_data_set_bool(data, "onboarding_seen", onboardingSeen);
	obs_data_set_bool(data, "initial_dock_float_shown",
			  initialDockFloatShown);
	obs_data_set_bool(data, "player_controls_size_initialized",
			  playerControlsSizeInitialized);
	obs_data_set_bool(data, "scene_item_lock_restore_pending",
			  sceneItemLockBackupActive);
	obs_data_set_bool(data, "browser_item_was_locked",
			  browserItemWasLocked);
	obs_data_set_bool(data, "webcam_item_was_locked", webcamItemWasLocked);
	writeAudioRestoreBackup(data);

	// Ensure directory exists
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_save_json_safe(data, path, ".tmp", ".bak");
	obs_data_release(data);
	bfree(path);
}

void SceneManager::loadConfig()
{
	char *path = obs_module_config_path("config.json");
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path, ".bak");
	bfree(path);
	if (!data)
		return;

	sceneUuid = obs_data_get_string(data, "scene_uuid");
	webcamUuid = obs_data_get_string(data, "webcam_uuid");
	micUuid = obs_data_get_string(data, "mic_uuid");
	reactionAudioUuids.clear();
	obs_data_array_t *audioArr =
		obs_data_get_array(data, "reaction_audio_source_uuids");
	if (audioArr) {
		size_t count = obs_data_array_count(audioArr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *entry = obs_data_array_item(audioArr, i);
			if (!entry)
				continue;
			const char *uuid = obs_data_get_string(entry, "uuid");
			if (uuid && *uuid)
				reactionAudioUuids.push_back(uuid);
			obs_data_release(entry);
		}
		obs_data_array_release(audioArr);
	}
	if (reactionAudioUuids.empty() && !micUuid.empty())
		reactionAudioUuids.push_back(micUuid);
	if (micUuid.empty() && !reactionAudioUuids.empty())
		micUuid = reactionAudioUuids.front();
	browserUuid = obs_data_get_string(data, "browser_uuid");
	webcamUsesExistingSource =
		obs_data_get_bool(data, "webcam_uses_existing_source");
	micUsesExistingSource =
		obs_data_get_bool(data, "mic_uses_existing_source");
	if (obs_data_has_user_value(data, "youtube_volume"))
		savedVolume = (int)obs_data_get_int(data, "youtube_volume");
	if (obs_data_has_user_value(data, "reaction_encoder_custom"))
		reactionEncoderCustom.enabled =
			obs_data_get_bool(data, "reaction_encoder_custom");
	if (obs_data_has_user_value(data, "reaction_encoder_crf"))
		reactionEncoderCustom.crf = clampInt(
			(int)obs_data_get_int(data, "reaction_encoder_crf"), 0,
			51);
	if (obs_data_has_user_value(data, "reaction_encoder_video_bitrate"))
		reactionEncoderCustom.videoBitrate = clampInt(
			(int)obs_data_get_int(data,
					      "reaction_encoder_video_bitrate"),
			500, 100000);
	if (obs_data_has_user_value(data, "reaction_encoder_audio_bitrate"))
		reactionEncoderCustom.audioBitrate = clampInt(
			(int)obs_data_get_int(data,
					      "reaction_encoder_audio_bitrate"),
			64, 320);
	if (obs_data_has_user_value(data, "reaction_output_format"))
		reactionOutputFormat = sanitizeReactionOutputFormat(
			obs_data_get_string(data, "reaction_output_format"));
	if (obs_data_has_user_value(data, "onboarding_seen"))
		onboardingSeen = obs_data_get_bool(data, "onboarding_seen");
	if (obs_data_has_user_value(data, "initial_dock_float_shown"))
		initialDockFloatShown =
			obs_data_get_bool(data, "initial_dock_float_shown");
	if (obs_data_has_user_value(data, "player_controls_size_initialized"))
		playerControlsSizeInitialized = obs_data_get_bool(
			data, "player_controls_size_initialized");
	if (obs_data_has_user_value(data, "scene_item_lock_restore_pending"))
		sceneItemLockBackupActive = obs_data_get_bool(
			data, "scene_item_lock_restore_pending");
	if (obs_data_has_user_value(data, "browser_item_was_locked"))
		browserItemWasLocked =
			obs_data_get_bool(data, "browser_item_was_locked");
	if (obs_data_has_user_value(data, "webcam_item_was_locked"))
		webcamItemWasLocked =
			obs_data_get_bool(data, "webcam_item_was_locked");
	loadAudioRestoreBackup(data);
	obs_data_release(data);
}

void SceneManager::writeAudioRestoreBackup(obs_data_t *data) const
{
	const bool shouldPersist = trackSetupActive || audioRestorePending;
	obs_data_set_bool(data, "audio_restore_pending", shouldPersist);
	if (!shouldPersist)
		return;

	const int track = trackSetupActive ? separatedTrack : pendingSeparatedTrack;
	const auto &backups =
		trackSetupActive ? mixerBackups : pendingMixerBackups;
	obs_data_set_int(data, "audio_restore_track", track);

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &backup : backups) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "uuid", backup.uuid.c_str());
		obs_data_set_string(entry, "name", backup.name.c_str());
		obs_data_set_int(entry, "mixers", backup.mixers);
		obs_data_array_push_back(arr, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(data, "audio_restore_backups", arr);
	obs_data_array_release(arr);
}

void SceneManager::loadAudioRestoreBackup(obs_data_t *data)
{
	audioRestorePending = obs_data_get_bool(data, "audio_restore_pending");
	pendingSeparatedTrack = -1;
	pendingMixerBackups.clear();
	if (!audioRestorePending)
		return;

	pendingSeparatedTrack =
		static_cast<int>(obs_data_get_int(data, "audio_restore_track"));
	obs_data_array_t *arr = obs_data_get_array(data, "audio_restore_backups");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *entry = obs_data_array_item(arr, i);
			if (!entry)
				continue;
			const char *uuid = obs_data_get_string(entry, "uuid");
			const char *name = obs_data_get_string(entry, "name");
			uint32_t mixers = static_cast<uint32_t>(
				obs_data_get_int(entry, "mixers"));
			pendingMixerBackups.push_back(
				{uuid ? uuid : "", name ? name : "", mixers});
			obs_data_release(entry);
		}
		obs_data_array_release(arr);
	}

	if (pendingMixerBackups.empty()) {
		audioRestorePending = false;
		pendingSeparatedTrack = -1;
	}
}

void SceneManager::saveVolume(int v)
{
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	savedVolume = v;
	saveConfig();
}

ReactionEncoderCustomSettings
SceneManager::getReactionEncoderCustomSettings() const
{
	return reactionEncoderCustom;
}

void SceneManager::saveReactionEncoderCustomSettings(
	const ReactionEncoderCustomSettings &settings)
{
	reactionEncoderCustom.enabled = settings.enabled;
	reactionEncoderCustom.crf = clampInt(settings.crf, 0, 51);
	reactionEncoderCustom.videoBitrate =
		clampInt(settings.videoBitrate, 500, 100000);
	reactionEncoderCustom.audioBitrate =
		clampInt(settings.audioBitrate, 64, 320);
	saveConfig();
}

void SceneManager::clearReactionEncoderCustomSettings()
{
	reactionEncoderCustom.enabled = false;
	saveConfig();
}

std::string SceneManager::getReactionOutputFormat() const
{
	return reactionOutputFormat;
}

void SceneManager::saveReactionOutputFormat(const std::string &format)
{
	reactionOutputFormat = sanitizeReactionOutputFormat(format);
	saveConfig();
}

void SceneManager::setOnboardingSeen(bool seen)
{
	onboardingSeen = seen;
	saveConfig();
}

void SceneManager::setInitialDockPlacementShown(bool shown)
{
	initialDockFloatShown = shown;
	saveConfig();
}

void SceneManager::setPlayerControlsSizeInitialized(bool initialized)
{
	playerControlsSizeInitialized = initialized;
	saveConfig();
}

obs_source_t *SceneManager::findSourceByUuidOrName(const std::string &uuid,
						   const char *fallbackName)
{
	if (!uuid.empty()) {
		obs_source_t *src = obs_get_source_by_uuid(uuid.c_str());
		if (src)
			return src;
	}
	if (fallbackName) {
		obs_source_t *src = obs_get_source_by_name(fallbackName);
		if (src)
			return src;
	}
	return nullptr;
}

void SceneManager::releaseSourceRef(obs_source_t *&target)
{
	if (!target)
		return;
	obs_source_release(target);
	target = nullptr;
}

void SceneManager::adoptSourceRef(obs_source_t *&target, obs_source_t *next)
{
	if (target)
		obs_source_release(target);
	target = next;
}

int SceneManager::removeSceneItemsForSource(obs_source_t *source,
					    obs_sceneitem_t *keepItem)
{
	if (!scene || !source)
		return 0;

	struct RemoveCtx {
		obs_source_t *source;
		obs_sceneitem_t *keepItem;
		std::vector<obs_sceneitem_t *> items;
	};

	RemoveCtx ctx = {source, keepItem, {}};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *ctx = static_cast<RemoveCtx *>(param);
			if (!item || item == ctx->keepItem)
				return true;

			obs_source_t *itemSource =
				obs_sceneitem_get_source(item);
			if (itemSource != ctx->source)
				return true;

			obs_sceneitem_addref(item);
			ctx->items.push_back(item);
			return true;
		},
		&ctx);

	for (obs_sceneitem_t *item : ctx.items) {
		if (item == browserItem)
			browserItem = nullptr;
		if (item == webcamItem)
			webcamItem = nullptr;
		if (item == micItem)
			micItem = nullptr;
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}

	return static_cast<int>(ctx.items.size());
}

void SceneManager::releaseSceneRef()
{
	if (!scene)
		return;
	disconnectSceneSignals();
	obs_scene_release(scene);
	scene = nullptr;
	sceneSrc = nullptr;
	browserItem = nullptr;
	webcamItem = nullptr;
	micItem = nullptr;
}

SceneManager::~SceneManager()
{
	if (trackSetupActive)
		restoreAudioTracks();
	if (sceneItemLockBackupActive)
		lockSceneItems(false);

	releaseSourceRef(webcamSrc);
	releaseSourceRef(micSrc);
	releaseSceneRef();
}

bool SceneManager::createScene()
{
	// Find existing scene by UUID first, then by name
	obs_source_t *existing =
		findSourceByUuidOrName(sceneUuid, SCENE_SOURCE_NAME);
	if (!existing)
		existing = obs_get_source_by_name(LEGACY_SCENE_SOURCE_NAME);
	if (existing) {
		releaseSceneRef();
		scene = obs_scene_from_source(existing);
		if (scene) {
			sceneSrc = existing;
			sceneUuid = obs_source_get_uuid(existing);
			obs_source_set_name(sceneSrc, SCENE_SOURCE_NAME);
			connectSceneSignals();
			saveConfig();
			blog(LOG_INFO,
			     "[VirtuEdit] Reusing existing scene (uuid=%s)",
			     sceneUuid.c_str());
			return true;
		}
		obs_source_release(existing);
	}

	releaseSceneRef();
	scene = obs_scene_create(SCENE_SOURCE_NAME);
	if (!scene) {
		blog(LOG_ERROR, "[VirtuEdit] Failed to create scene");
		return false;
	}
	sceneSrc = obs_scene_get_source(scene);
	sceneUuid = obs_source_get_uuid(sceneSrc);
	connectSceneSignals();
	saveConfig();

	blog(LOG_INFO, "[VirtuEdit] Scene created: %s", SCENE_SOURCE_NAME);
	return true;
}

bool SceneManager::selectReactionScene(const std::string &sourceUuid)
{
	obs_source_t *selected = nullptr;
	if (sourceUuid.empty())
		selected = obs_frontend_get_current_scene();
	else
		selected = obs_get_source_by_uuid(sourceUuid.c_str());

	if (!selected) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Reaction scene selection failed: source not found");
		return false;
	}

	obs_scene_t *selectedScene = obs_scene_from_source(selected);
	if (!selectedScene) {
		obs_source_release(selected);
		blog(LOG_ERROR,
		     "[VirtuEdit] Reaction scene selection failed: source is not a scene");
		return false;
	}

	releaseSceneRef();
	scene = selectedScene;
	sceneSrc = selected;
	const char *uuid = obs_source_get_uuid(selected);
	sceneUuid = uuid ? uuid : "";
	connectSceneSignals();
	saveConfig();

	blog(LOG_INFO, "[VirtuEdit] Reaction scene selected: %s",
	     obs_source_get_name(selected));
	return true;
}

bool SceneManager::addBrowserSource(obs_source_t *browserSrc)
{
	if (!scene || !browserSrc)
		return false;

	// Check if already in scene (by UUID, then name)
	if (!browserUuid.empty()) {
		obs_source_t *existing =
			obs_get_source_by_uuid(browserUuid.c_str());
		if (existing) {
			if (existing == browserSrc) {
				browserItem = obs_scene_find_source_recursive(
					scene, obs_source_get_name(existing));
			}
			obs_source_release(existing);
			if (browserItem) {
				obs_source_t *src =
					obs_sceneitem_get_source(browserItem);
				if (src) {
					browserUuid = obs_source_get_uuid(src);
					obs_source_set_name(src, BROWSER_SOURCE_NAME);
				}
				blog(LOG_INFO,
				     "[VirtuEdit] Browser source reused (uuid)");
				return true;
			}
		}
	}
	browserItem = obs_scene_find_source(scene, BROWSER_SOURCE_NAME);
	if (!browserItem)
		browserItem =
			obs_scene_find_source(scene, PREVIOUS_BROWSER_SOURCE_NAME);
	if (!browserItem)
		browserItem = obs_scene_find_source(
			scene, OLDER_PREVIOUS_BROWSER_SOURCE_NAME_2);
	if (!browserItem)
		browserItem = obs_scene_find_source(
			scene, OLDER_PREVIOUS_BROWSER_SOURCE_NAME);
	if (!browserItem)
		browserItem =
			obs_scene_find_source(scene, OLDER_BROWSER_SOURCE_NAME_VR);
	if (!browserItem)
		browserItem =
			obs_scene_find_source(scene, LEGACY_BROWSER_SOURCE_NAME);
	if (!browserItem)
		browserItem =
			obs_scene_find_source(scene, OLDER_BROWSER_SOURCE_NAME);
	if (!browserItem)
		browserItem =
			obs_scene_find_source(scene, OLDEST_BROWSER_SOURCE_NAME);
	if (browserItem) {
		obs_source_t *src = obs_sceneitem_get_source(browserItem);
		if (src) {
			if (src != browserSrc) {
				browserItem = nullptr;
			} else {
				browserUuid = obs_source_get_uuid(src);
				obs_source_set_name(src, BROWSER_SOURCE_NAME);
			}
		}
		if (browserItem) {
			saveConfig();
			blog(LOG_INFO,
			     "[VirtuEdit] Browser source reused (name)");
			return true;
		}
	}

	browserItem = obs_scene_add(scene, browserSrc);
	if (!browserItem)
		return false;

	browserUuid = obs_source_get_uuid(browserSrc);
	saveConfig();

	applyDefaultBrowserLayout();

	blog(LOG_INFO, "[VirtuEdit] Browser source added to scene");
	return true;
}

bool SceneManager::resetPlayerControls(obs_source_t *browserSrc)
{
	if (!scene || !browserSrc)
		return false;

	PlayerControlsItemCtx ctx;
	obs_scene_enum_items(scene, collectPlayerControlsItems, &ctx);

	for (obs_sceneitem_t *item : ctx.items) {
		if (item == browserItem)
			browserItem = nullptr;
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}

	browserItem = obs_scene_add(scene, browserSrc);
	if (!browserItem)
		return false;

	obs_source_set_name(browserSrc, BROWSER_SOURCE_NAME);
	browserUuid = obs_source_get_uuid(browserSrc);
	obs_sceneitem_set_visible(browserItem, true);
	saveConfig();
	applyDefaultBrowserLayout();

	blog(LOG_INFO,
	     "[VirtuEdit] Player Controls reset in reaction scene (removed=%zu)",
	     ctx.items.size());
	return true;
}

void SceneManager::applyDefaultBrowserLayout()
{
	if (!browserItem)
		return;

	auto canvas = virtuedit_get_canvas_size();
	uint32_t canvasW = canvas.width;

	float pipW = canvasW * DEFAULT_PIP_SIZE;
	float pipH = pipHeightForWidth(pipW);
	struct vec2 pos = {canvasW - pipW - canvasW * DEFAULT_PIP_RIGHT_MARGIN,
			   canvas.height * DEFAULT_PIP_TOP};
	obs_sceneitem_set_pos(browserItem, &pos);

	struct vec2 bounds = {pipW, pipH};
	obs_sceneitem_set_bounds_type(browserItem, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(browserItem, &bounds);

	// YouTube on top of webcam (PiP overlay)
	obs_sceneitem_set_order(browserItem, OBS_ORDER_MOVE_TOP);
	syncBrowserOverlayTransform();
}

bool SceneManager::resetBrowserLayout()
{
	if (!browserItem)
		return false;

	applyDefaultBrowserLayout();
	blog(LOG_INFO, "[VirtuEdit] YouTube PiP reset to default layout");
	return true;
}

void SceneManager::connectSceneSignals()
{
	if (!sceneSrc || sceneSignalsConnected)
		return;

	signal_handler_t *handler = obs_source_get_signal_handler(sceneSrc);
	if (!handler)
		return;

	signal_handler_connect(handler, "item_transform",
			       onSceneItemTransformed, this);
	signal_handler_connect(handler, "item_remove", onSceneItemRemoved,
			       this);
	sceneSignalsConnected = true;
}

void SceneManager::disconnectSceneSignals()
{
	if (!sceneSrc || !sceneSignalsConnected)
		return;

	signal_handler_t *handler = obs_source_get_signal_handler(sceneSrc);
	if (handler) {
		signal_handler_disconnect(handler, "item_transform",
					  onSceneItemTransformed, this);
		signal_handler_disconnect(handler, "item_remove",
					  onSceneItemRemoved, this);
	}
	sceneSignalsConnected = false;
}

void SceneManager::onSceneItemTransformed(void *data, calldata_t *params)
{
	auto *self = static_cast<SceneManager *>(data);
	auto *item =
		static_cast<obs_sceneitem_t *>(calldata_ptr(params, "item"));
	if (self && item && item == self->browserItem)
		self->syncBrowserOverlayTransform();
}

void SceneManager::onSceneItemRemoved(void *data, calldata_t *params)
{
	auto *self = static_cast<SceneManager *>(data);
	auto *item =
		static_cast<obs_sceneitem_t *>(calldata_ptr(params, "item"));
	if (!self || !item)
		return;
	if (item == self->browserItem)
		self->browserItem = nullptr;
	if (item == self->webcamItem)
		self->webcamItem = nullptr;
	if (item == self->micItem)
		self->micItem = nullptr;
}

bool SceneManager::syncBrowserOverlayTransform()
{
	if (!browserItem)
		return false;
	if (updatingBrowserTransform)
		return false;

	auto canvas = virtuedit_get_canvas_size();
	const float canvasW = static_cast<float>(canvas.width);
	const float canvasH = static_cast<float>(canvas.height);
	if (canvasW <= 0.0f || canvasH <= 0.0f)
		return false;

	struct vec2 pos;
	obs_sceneitem_get_pos(browserItem, &pos);

	struct vec2 bounds;
	obs_sceneitem_get_bounds(browserItem, &bounds);
	if (bounds.x <= 0.0f && bounds.y <= 0.0f)
		return false;

	if (bounds.x <= 0.0f)
		bounds.x = bounds.y * PIP_ASPECT;
	if (bounds.y <= 0.0f)
		bounds.y = pipHeightForWidth(bounds.x);

	bool changed = false;
	struct vec2 nextBounds = {bounds.x, pipHeightForWidth(bounds.x)};
	if (nextBounds.x > canvasW) {
		nextBounds.x = canvasW;
		nextBounds.y = pipHeightForWidth(nextBounds.x);
	}
	if (nextBounds.y > canvasH) {
		nextBounds.y = canvasH;
		nextBounds.x = nextBounds.y * PIP_ASPECT;
	}

	const float maxX = std::max(0.0f, canvasW - nextBounds.x);
	const float maxY = std::max(0.0f, canvasH - nextBounds.y);
	struct vec2 nextPos = {clampFloat(pos.x, 0.0f, maxX),
			       clampFloat(pos.y, 0.0f, maxY)};

	updatingBrowserTransform = true;
	obs_sceneitem_set_bounds_type(browserItem, OBS_BOUNDS_SCALE_INNER);
	if (!nearlyEqual(bounds.x, nextBounds.x) ||
	    !nearlyEqual(bounds.y, nextBounds.y)) {
		obs_sceneitem_set_bounds(browserItem, &nextBounds);
		changed = true;
	}
	if (!nearlyEqual(pos.x, nextPos.x) || !nearlyEqual(pos.y, nextPos.y)) {
		obs_sceneitem_set_pos(browserItem, &nextPos);
		changed = true;
	}

	updatingBrowserTransform = false;

	return changed;
}

bool SceneManager::addWebcamSource(const std::string &deviceId)
{
	if (!scene)
		return false;

	// Find our managed webcam by UUID first, then by name. If the previous
	// selection was an external OBS source, do not treat that UUID as ours.
	obs_source_t *existing = nullptr;
	if (!webcamUsesExistingSource)
		existing = findSourceByUuidOrName(webcamUuid, WEBCAM_SOURCE_NAME);
	if (!existing)
		existing = obs_get_source_by_name(WEBCAM_SOURCE_NAME);
	if (!existing)
		existing = obs_get_source_by_name(LEGACY_WEBCAM_SOURCE_NAME);
	if (existing) {
		obs_source_t *previous = webcamSrc;
		if (previous && previous != existing)
			removeSceneItemsForSource(previous, nullptr);
		adoptSourceRef(webcamSrc, existing);
		webcamUuid = obs_source_get_uuid(existing);
		webcamUsesExistingSource = false;
		obs_source_set_name(webcamSrc, WEBCAM_SOURCE_NAME);

		// Find scene item (name may have changed)
		webcamItem = obs_scene_find_source_recursive(
			scene, obs_source_get_name(webcamSrc));
		if (!webcamItem)
			webcamItem = obs_scene_add(scene, existing);
		removeSceneItemsForSource(webcamSrc, webcamItem);

		// Update device
		obs_data_t *settings = obs_source_get_settings(webcamSrc);
#ifdef __APPLE__
		obs_data_set_string(settings, "device", deviceId.c_str());
#else
		obs_data_set_string(settings, "video_device_id",
				    deviceId.c_str());
#endif
		obs_source_update(webcamSrc, settings);
		obs_data_release(settings);
		saveConfig();
		blog(LOG_INFO,
		     "[VirtuEdit] Webcam source reused (device: %s)",
		     deviceId.c_str());
		return true;
	}

	removeSceneItemsForSource(webcamSrc, nullptr);
	releaseSourceRef(webcamSrc);

	obs_data_t *settings = obs_data_create();
#ifdef __APPLE__
	obs_data_set_string(settings, "device", deviceId.c_str());
	webcamSrc = obs_source_create("av_capture_input",
				      WEBCAM_SOURCE_NAME, settings, nullptr);
#else
	obs_data_set_string(settings, "video_device_id", deviceId.c_str());
	webcamSrc = obs_source_create("dshow_input", WEBCAM_SOURCE_NAME,
				      settings, nullptr);
#endif
	obs_data_release(settings);

	if (!webcamSrc) {
		blog(LOG_ERROR, "[VirtuEdit] Failed to create webcam source");
		return false;
	}

	webcamItem = obs_scene_add(scene, webcamSrc);
	if (!webcamItem) {
		releaseSourceRef(webcamSrc);
		return false;
	}

	webcamUuid = obs_source_get_uuid(webcamSrc);
	webcamUsesExistingSource = false;
	saveConfig();

	// Webcam fullscreen (background)
	struct vec2 pos = {0.0f, 0.0f};
	obs_sceneitem_set_pos(webcamItem, &pos);

	auto canvas = virtuedit_get_canvas_size();
	struct vec2 bounds = {(float)canvas.width, (float)canvas.height};
	obs_sceneitem_set_bounds_type(webcamItem, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(webcamItem, &bounds);

	// Webcam behind YouTube PiP
	obs_sceneitem_set_order(webcamItem, OBS_ORDER_MOVE_BOTTOM);

	blog(LOG_INFO, "[VirtuEdit] Webcam source added (device: %s)",
	     deviceId.c_str());
	return true;
}

bool SceneManager::addExistingWebcamSource(const std::string &sourceUuid)
{
	if (!scene || sourceUuid.empty())
		return false;

	obs_source_t *existing = obs_get_source_by_uuid(sourceUuid.c_str());
	if (!existing) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Existing webcam source not found: %s",
		     sourceUuid.c_str());
		return false;
	}

	uint32_t flags = obs_source_get_output_flags(existing);
	if (!(flags & OBS_SOURCE_VIDEO) ||
	    obs_source_get_type(existing) != OBS_SOURCE_TYPE_INPUT) {
		obs_source_release(existing);
		blog(LOG_ERROR,
		     "[VirtuEdit] Selected webcam source is not a video input");
		return false;
	}

	obs_source_t *previous = webcamSrc;
	if (previous && previous != existing)
		removeSceneItemsForSource(previous, nullptr);
	adoptSourceRef(webcamSrc, existing);
	webcamUuid = obs_source_get_uuid(webcamSrc);
	webcamUsesExistingSource = true;

	removeSceneItemsForSource(webcamSrc, nullptr);
	webcamItem = obs_scene_find_source_recursive(
		scene, obs_source_get_name(webcamSrc));
	if (!webcamItem)
		webcamItem = obs_scene_add(scene, webcamSrc);
	if (!webcamItem)
		return false;
	removeSceneItemsForSource(webcamSrc, webcamItem);

	struct vec2 pos = {0.0f, 0.0f};
	obs_sceneitem_set_pos(webcamItem, &pos);

	auto canvas = virtuedit_get_canvas_size();
	struct vec2 bounds = {(float)canvas.width, (float)canvas.height};
	obs_sceneitem_set_bounds_type(webcamItem, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(webcamItem, &bounds);
	obs_sceneitem_set_order(webcamItem, OBS_ORDER_MOVE_BOTTOM);

	saveConfig();
	blog(LOG_INFO, "[VirtuEdit] Existing webcam source selected: %s",
	     obs_source_get_name(webcamSrc));
	return true;
}

bool SceneManager::addMicSource(const std::string &deviceId)
{
	if (!scene)
		return false;

	// Find our managed mic by UUID first, then by name. If the previous
	// selection was an external OBS source, do not treat that UUID as ours.
	obs_source_t *existing = nullptr;
	if (!micUsesExistingSource)
		existing = findSourceByUuidOrName(micUuid, MIC_SOURCE_NAME);
	if (!existing)
		existing = obs_get_source_by_name(MIC_SOURCE_NAME);
	if (!existing)
		existing = obs_get_source_by_name(LEGACY_MIC_SOURCE_NAME);
	if (existing) {
		obs_source_t *previous = micSrc;
		if (previous && previous != existing)
			removeSceneItemsForSource(previous, nullptr);
		adoptSourceRef(micSrc, existing);
		micUuid = obs_source_get_uuid(existing);
		micUsesExistingSource = false;
		obs_source_set_name(micSrc, MIC_SOURCE_NAME);
		obs_data_t *settings = obs_source_get_settings(micSrc);
		obs_data_set_string(settings, "device_id", deviceId.c_str());
		obs_source_update(micSrc, settings);
		obs_data_release(settings);

		// Ensure it's in the scene so OBS persists it across restarts
		micItem = obs_scene_find_source_recursive(
			scene, obs_source_get_name(micSrc));
		if (!micItem)
			micItem = obs_scene_add(scene, micSrc);
		removeSceneItemsForSource(micSrc, micItem);

		saveConfig();
		blog(LOG_INFO,
		     "[VirtuEdit] Mic source reused (device: %s)",
		     deviceId.c_str());
		return true;
	}

	removeSceneItemsForSource(micSrc, nullptr);
	releaseSourceRef(micSrc);

	obs_data_t *settings = obs_data_create();
#ifdef __APPLE__
	obs_data_set_string(settings, "device_id", deviceId.c_str());
	micSrc = obs_source_create("coreaudio_input_capture",
				   MIC_SOURCE_NAME, settings, nullptr);
#else
	obs_data_set_string(settings, "device_id", deviceId.c_str());
	micSrc = obs_source_create("wasapi_input_capture", MIC_SOURCE_NAME,
				   settings, nullptr);
#endif
	obs_data_release(settings);

	if (!micSrc) {
		blog(LOG_ERROR, "[VirtuEdit] Failed to create mic source");
		return false;
	}

	micUuid = obs_source_get_uuid(micSrc);
	micUsesExistingSource = false;
	reactionAudioUuids = {micUuid};

	// Add to scene (audio-only, renders nothing) so OBS persists it across
	// restarts; otherwise an unreferenced source is dropped on reload.
	micItem = obs_scene_add(scene, micSrc);
	if (!micItem) {
		releaseSourceRef(micSrc);
		return false;
	}

	saveConfig();

	blog(LOG_INFO, "[VirtuEdit] Mic source added (device: %s)",
	     deviceId.c_str());
	return true;
}

bool SceneManager::addExistingMicSource(const std::string &sourceUuid)
{
	if (!scene || sourceUuid.empty())
		return false;

	obs_source_t *existing = obs_get_source_by_uuid(sourceUuid.c_str());
	if (!existing) {
		blog(LOG_ERROR, "[VirtuEdit] Existing mic source not found: %s",
		     sourceUuid.c_str());
		return false;
	}

	uint32_t flags = obs_source_get_output_flags(existing);
	if (!(flags & OBS_SOURCE_AUDIO) ||
	    obs_source_get_type(existing) != OBS_SOURCE_TYPE_INPUT) {
		obs_source_release(existing);
		blog(LOG_ERROR,
		     "[VirtuEdit] Selected mic source is not an audio input");
		return false;
	}

	obs_source_t *previous = micSrc;
	if (previous && previous != existing && !micUsesExistingSource)
		removeSceneItemsForSource(previous, nullptr);
	adoptSourceRef(micSrc, existing);
	micUuid = obs_source_get_uuid(micSrc);
	micUsesExistingSource = true;
	reactionAudioUuids = {micUuid};
	micItem = obs_scene_find_source_recursive(scene,
						 obs_source_get_name(micSrc));

	saveConfig();
	blog(LOG_INFO, "[VirtuEdit] Existing mic source selected: %s",
	     obs_source_get_name(micSrc));
	return true;
}

bool SceneManager::setReactionAudioSources(
	const std::vector<std::string> &sourceUuids)
{
	if (sourceUuids.empty()) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Reaction audio selection failed: no sources selected");
		return false;
	}

	std::vector<std::string> validUuids;
	obs_source_t *firstSrc = nullptr;
	for (const std::string &uuid : sourceUuids) {
		if (uuid.empty() ||
		    std::find(validUuids.begin(), validUuids.end(), uuid) !=
			    validUuids.end())
			continue;

		obs_source_t *src = obs_get_source_by_uuid(uuid.c_str());
		if (!src)
			continue;

		uint32_t flags = obs_source_get_output_flags(src);
		if ((flags & OBS_SOURCE_AUDIO) &&
		    obs_source_get_type(src) == OBS_SOURCE_TYPE_INPUT) {
			validUuids.push_back(uuid);
			if (!firstSrc) {
				obs_source_get_ref(src);
				firstSrc = src;
			}
		}
		obs_source_release(src);
	}

	if (validUuids.empty()) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Reaction audio selection failed: selected sources are unavailable");
		return false;
	}

	obs_source_t *previousMicSrc = micSrc;
	if (previousMicSrc && previousMicSrc != firstSrc &&
	    !micUsesExistingSource)
		removeSceneItemsForSource(previousMicSrc, nullptr);

	adoptSourceRef(micSrc, firstSrc);
	micUuid = validUuids.front();
	micUsesExistingSource = true;
	reactionAudioUuids = validUuids;
	micItem = micSrc ? obs_scene_find_source_recursive(
				   scene, obs_source_get_name(micSrc))
			 : nullptr;
	saveConfig();

	blog(LOG_INFO,
	     "[VirtuEdit] Reaction audio sources selected: %zu",
	     reactionAudioUuids.size());
	return true;
}

void SceneManager::cleanupCaptureSceneItems()
{
	if (!scene)
		return;

	obs_source_t *browserSrc =
		browserItem ? obs_sceneitem_get_source(browserItem) : nullptr;

	struct CleanupCtx {
		SceneManager *self;
		obs_source_t *browserSrc;
		std::vector<obs_sceneitem_t *> items;
	};

	CleanupCtx ctx = {this, browserSrc, {}};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *ctx = static_cast<CleanupCtx *>(param);
			SceneManager *self = ctx->self;
			if (!self || !item)
				return true;
			if (item == self->browserItem ||
			    item == self->webcamItem || item == self->micItem)
				return true;

			obs_source_t *src = obs_sceneitem_get_source(item);
			if (!src || src == self->webcamSrc ||
			    src == self->micSrc || src == ctx->browserSrc)
				return true;

			const char *id = obs_source_get_id(src);
			if (isBrowserLikeSource(id))
				return true;

			const char *name = obs_source_get_name(src);
			if (!isCaptureDeviceSource(id) &&
			    !isManagedCaptureSourceName(name))
				return true;

			obs_sceneitem_addref(item);
			ctx->items.push_back(item);
			return true;
		},
		&ctx);

	for (obs_sceneitem_t *item : ctx.items) {
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}

	if (!ctx.items.empty()) {
		blog(LOG_INFO,
		     "[VirtuEdit] Removed %zu stale capture source item(s)",
		     ctx.items.size());
	}
}

void SceneManager::activateScene()
{
	if (!sceneSrc)
		return;
	obs_frontend_set_current_scene(sceneSrc);
	blog(LOG_INFO, "[VirtuEdit] Switched to reaction scene: %s",
	     obs_source_get_name(sceneSrc));
}

// --- Scene item locking ---

void SceneManager::lockSceneItems(bool locked)
{
	if (locked) {
		if (!sceneItemLockBackupActive) {
			browserItemWasLocked =
				browserItem && obs_sceneitem_locked(browserItem);
			webcamItemWasLocked =
				webcamItem && obs_sceneitem_locked(webcamItem);
			sceneItemLockBackupActive = true;
		}
		if (browserItem)
			obs_sceneitem_set_locked(browserItem, true);
		if (webcamItem)
			obs_sceneitem_set_locked(webcamItem, true);
		saveConfig();
	} else if (sceneItemLockBackupActive) {
		if (browserItem)
			obs_sceneitem_set_locked(browserItem,
						 browserItemWasLocked);
		if (webcamItem)
			obs_sceneitem_set_locked(webcamItem, webcamItemWasLocked);
		sceneItemLockBackupActive = false;
		saveConfig();
	}

	blog(LOG_INFO, "[VirtuEdit] Scene items %s",
	     locked ? "locked" : "unlocked");
}

// --- Audio track separation ---

bool SceneManager::setupAudioTracks()
{
	audioSetupError.clear();
	if (trackSetupActive)
		return true;
	if (audioRestorePending)
		restorePendingAudioTracks();

	std::vector<std::string> activeAudioUuids = reactionAudioUuids;
	if (activeAudioUuids.empty() && !micUuid.empty())
		activeAudioUuids.push_back(micUuid);

	if (activeAudioUuids.empty()) {
		separatedTrack = -1;
		audioSetupError =
			"Select at least one audio source for the Reaction Video.";
		blog(LOG_ERROR,
		     "[VirtuEdit] Audio track setup failed: no reaction audio sources selected");
		return false;
	}

	mixerBackups.clear();
	auto isSelectedAudioSource =
		[&activeAudioUuids](const std::string &uuid) -> bool {
		return !uuid.empty() &&
		       std::find(activeAudioUuids.begin(),
				 activeAudioUuids.end(),
				 uuid) != activeAudioUuids.end();
	};

	struct EnumCtx {
		SceneManager *self;
	} ctx{this};

	auto enumCb = [](void *param, obs_source_t *src) -> bool {
		auto *ctx = static_cast<EnumCtx *>(param);
		uint32_t flags = obs_source_get_output_flags(src);
		if (!(flags & OBS_SOURCE_AUDIO))
			return true;

		uint32_t mixers = obs_source_get_audio_mixers(src);

		const char *uuid = obs_source_get_uuid(src);
		const char *name = obs_source_get_name(src);
		ctx->self->mixerBackups.push_back(
			{uuid ? uuid : "", name ? name : "", mixers});
		return true;
	};
	obs_enum_sources(enumCb, &ctx);

	size_t selectedAvailable = 0;
	for (const auto &backup : mixerBackups) {
		if (isSelectedAudioSource(backup.uuid))
			selectedAvailable++;
	}
	if (selectedAvailable == 0) {
		mixerBackups.clear();
		separatedTrack = -1;
		audioSetupError =
			"The selected Reaction Video audio sources are unavailable.";
		blog(LOG_ERROR,
		     "[VirtuEdit] Audio track setup failed: selected reaction audio sources are unavailable");
		return false;
	}

	const uint32_t protectedMask = protectedObsAudioMixerMask();
	separatedTrack =
		virtuedit_select_reaction_audio_track(protectedMask);
	if (separatedTrack < 0) {
		mixerBackups.clear();
		audioSetupError =
			"All six OBS audio tracks are reserved by the current output settings. Free one audio track in OBS Settings > Output, then try again.";
		blog(LOG_ERROR,
		     "[VirtuEdit] Audio track setup failed: no mixer is available outside OBS recording/streaming tracks (protected mask: 0x%02x)",
		     protectedMask);
		return false;
	}

	uint32_t separatedBit = 1u << separatedTrack;
	size_t selectedSources = 0;
	size_t isolatedSources = 0;

	trackSetupActive = true;
	saveConfig();

	for (const auto &backup : mixerBackups) {
		obs_source_t *src = findSourceByUuidOrName(
			backup.uuid, backup.name.empty() ? nullptr
							: backup.name.c_str());
		if (!src)
			continue;

		const char *srcUuidC = obs_source_get_uuid(src);
		std::string srcUuid = srcUuidC ? srcUuidC : "";
		bool isSelected = isSelectedAudioSource(srcUuid);
		if (isSelected)
			selectedSources++;

		uint32_t newMixers =
			isSelected ? (backup.mixers | separatedBit)
				   : (backup.mixers & ~separatedBit);
		if (!isSelected && (backup.mixers & separatedBit))
			isolatedSources++;
		obs_source_set_audio_mixers(src, newMixers);
		obs_source_release(src);
	}

	blog(LOG_INFO,
	     "[VirtuEdit] Audio tracks isolated outside OBS outputs (reaction track: %d, protected: 0x%02x, selected: %zu, backed up: %zu, stripped: %zu)",
	     separatedTrack + 1, protectedMask, selectedSources, mixerBackups.size(),
	     isolatedSources);
	return true;
}

void SceneManager::restoreAudioTracks()
{
	if (!trackSetupActive)
		return;

	for (const auto &backup : mixerBackups) {
		obs_source_t *src = findSourceByUuidOrName(
			backup.uuid, backup.name.empty() ? nullptr
							: backup.name.c_str());
		if (!src)
			continue;

		obs_source_set_audio_mixers(src, backup.mixers);
		obs_source_release(src);
	}

	mixerBackups.clear();
	trackSetupActive = false;
	separatedTrack = -1;
	saveConfig();
	blog(LOG_INFO, "[VirtuEdit] Audio tracks restored to original");
}

bool SceneManager::restorePendingAudioTracks()
{
	if (!audioRestorePending || trackSetupActive)
		return false;

	size_t restored = 0;
	for (const auto &backup : pendingMixerBackups) {
		obs_source_t *src = findSourceByUuidOrName(
			backup.uuid, backup.name.empty() ? nullptr
							: backup.name.c_str());
		if (!src)
			continue;

		obs_source_set_audio_mixers(src, backup.mixers);
		obs_source_release(src);
		restored++;
	}

	blog(LOG_INFO,
	     "[VirtuEdit] Pending audio track restore applied (track: %d, restored: %zu/%zu)",
	     pendingSeparatedTrack + 1, restored, pendingMixerBackups.size());

	pendingMixerBackups.clear();
	audioRestorePending = false;
	pendingSeparatedTrack = -1;
	saveConfig();
	return restored > 0;
}

SceneManager::OverlayPos SceneManager::readOverlayPosition()
{
	// Default: YouTube PiP at top-right, matching the web recorder width
	// while preserving a 16:9 pixel aspect ratio.
	OverlayPos result = defaultOverlayPosition();

	if (!browserItem || !sceneSrc)
		return result;

	auto canvas = virtuedit_get_canvas_size();
	uint32_t canvasW = canvas.width;
	uint32_t canvasH = canvas.height;
	if (canvasW == 0 || canvasH == 0)
		return result;

	struct vec2 pos;
	obs_sceneitem_get_pos(browserItem, &pos);

	struct vec2 bounds;
	obs_sceneitem_get_bounds(browserItem, &bounds);

	float width = clampFloat(bounds.x, 0.0f, static_cast<float>(canvasW));
	float height = clampFloat(bounds.y, 0.0f, static_cast<float>(canvasH));
	float maxX = std::max(0.0f, static_cast<float>(canvasW) - width);
	float maxY = std::max(0.0f, static_cast<float>(canvasH) - height);

	result.x = clampFloat(pos.x, 0.0f, maxX) / canvasW;
	result.y = clampFloat(pos.y, 0.0f, maxY) / canvasH;
	result.w = width / canvasW;
	result.h = height / canvasH;

	return result;
}

// --- Device enumeration ---

std::vector<DeviceInfo> SceneManager::listWebcams()
{
	std::vector<DeviceInfo> devices;
	obs_properties_t *props = nullptr;

#ifdef __APPLE__
	obs_source_t *tmp = obs_source_create("av_capture_input",
					      "__tmp_enum", nullptr, nullptr);
#else
	obs_source_t *tmp = obs_source_create("dshow_input", "__tmp_enum",
					      nullptr, nullptr);
#endif
	if (!tmp)
		return devices;

	props = obs_source_properties(tmp);
	if (props) {
#ifdef __APPLE__
		obs_property_t *prop = obs_properties_get(props, "device");
#else
		obs_property_t *prop =
			obs_properties_get(props, "video_device_id");
#endif
		if (prop && obs_property_get_type(prop) ==
				    OBS_PROPERTY_LIST) {
			size_t count = obs_property_list_item_count(prop);
			for (size_t i = 0; i < count; i++) {
				const char *n =
					obs_property_list_item_name(prop, i);
				const char *v =
					obs_property_list_item_string(prop, i);
				if (n && v)
					devices.push_back({v, n});
			}
		}
		obs_properties_destroy(props);
	}
	obs_source_release(tmp);
	return devices;
}

std::vector<DeviceInfo> SceneManager::listMicrophones()
{
	std::vector<DeviceInfo> devices;
	obs_properties_t *props = nullptr;

#ifdef __APPLE__
	obs_source_t *tmp =
		obs_source_create("coreaudio_input_capture", "__tmp_enum_mic",
				  nullptr, nullptr);
#else
	obs_source_t *tmp = obs_source_create("wasapi_input_capture",
					      "__tmp_enum_mic", nullptr,
					      nullptr);
#endif
	if (!tmp)
		return devices;

	props = obs_source_properties(tmp);
	if (props) {
		obs_property_t *prop =
			obs_properties_get(props, "device_id");
		if (prop && obs_property_get_type(prop) ==
				    OBS_PROPERTY_LIST) {
			size_t count = obs_property_list_item_count(prop);
			for (size_t i = 0; i < count; i++) {
				const char *n =
					obs_property_list_item_name(prop, i);
				const char *v =
					obs_property_list_item_string(prop, i);
				if (n && v)
					devices.push_back({v, n});
			}
		}
		obs_properties_destroy(props);
	}
	obs_source_release(tmp);
	return devices;
}

std::vector<DeviceInfo> SceneManager::listScenes()
{
	std::vector<DeviceInfo> scenesOut;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *src = scenes.sources.array[i];
		if (!src)
			continue;
		const char *uuid = obs_source_get_uuid(src);
		const char *name = obs_source_get_name(src);
		if (uuid && name)
			scenesOut.push_back({uuid, name});
	}
	obs_frontend_source_list_free(&scenes);
	return scenesOut;
}

DeviceInfo SceneManager::currentFrontendScene()
{
	DeviceInfo result;
	obs_source_t *current = obs_frontend_get_current_scene();
	if (!current)
		return result;

	const char *uuid = obs_source_get_uuid(current);
	const char *name = obs_source_get_name(current);
	if (uuid)
		result.id = uuid;
	if (name)
		result.name = name;
	obs_source_release(current);
	return result;
}

std::vector<DeviceInfo> SceneManager::listExistingVideoSources()
{
	std::vector<DeviceInfo> sources;
	auto enumCb = [](void *param, obs_source_t *src) -> bool {
		auto *sources = static_cast<std::vector<DeviceInfo> *>(param);
		if (!src || obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT)
			return true;

		const char *name = obs_source_get_name(src);
		const char *uuid = obs_source_get_uuid(src);
		const char *id = obs_source_get_unversioned_id(src);
		uint32_t flags = obs_source_get_output_flags(src);
		if (!name || !uuid || !(flags & OBS_SOURCE_VIDEO) ||
		    isManagedVirtualRecorderSource(name) ||
		    isBrowserLikeSource(id))
			return true;

		sources->push_back({uuid, name});
		return true;
	};
	obs_enum_sources(enumCb, &sources);
	return sources;
}

std::vector<DeviceInfo> SceneManager::listExistingAudioSources()
{
	std::vector<DeviceInfo> sources;
	auto enumCb = [](void *param, obs_source_t *src) -> bool {
		auto *sources = static_cast<std::vector<DeviceInfo> *>(param);
		if (!src || obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT)
			return true;

		const char *name = obs_source_get_name(src);
		const char *uuid = obs_source_get_uuid(src);
		uint32_t flags = obs_source_get_output_flags(src);
		if (!name || !uuid || !(flags & OBS_SOURCE_AUDIO) ||
		    isManagedVirtualRecorderSource(name) ||
		    isManagedPlayerControlsName(name) ||
		    hasManagedPlayerControlsMarker(src))
			return true;

		sources->push_back({uuid, name});
		return true;
	};
	obs_enum_sources(enumCb, &sources);
	return sources;
}

static bool selectableAudioSource(obs_source_t *src)
{
	if (!src || obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT)
		return false;

	const char *name = obs_source_get_name(src);
	const char *uuid = obs_source_get_uuid(src);
	uint32_t flags = obs_source_get_output_flags(src);
	return name && uuid && (flags & OBS_SOURCE_AUDIO) &&
	       !isManagedVirtualRecorderSource(name) &&
	       !isManagedPlayerControlsName(name) &&
	       !hasManagedPlayerControlsMarker(src);
}

static bool addAudioSourceIfMissing(std::vector<DeviceInfo> &sources,
				    obs_source_t *src)
{
	if (!selectableAudioSource(src))
		return false;

	const char *uuid = obs_source_get_uuid(src);
	const char *name = obs_source_get_name(src);
	for (const DeviceInfo &source : sources) {
		if (source.id == uuid)
			return false;
	}

	sources.push_back({uuid, name});
	return true;
}

static void addGlobalAudioSources(std::vector<DeviceInfo> &sources)
{
	for (uint32_t channel = 0; channel < 64; channel++) {
		obs_source_t *src = obs_get_output_source(channel);
		if (!src)
			continue;
		addAudioSourceIfMissing(sources, src);
		obs_source_release(src);
	}
}

static void addSceneAudioSources(obs_scene_t *scene,
				 std::vector<DeviceInfo> &sources,
				 std::vector<std::string> &visitedSceneUuids)
{
	if (!scene)
		return;

	obs_source_t *sceneSource = obs_scene_get_source(scene);
	const char *sceneUuid = sceneSource ? obs_source_get_uuid(sceneSource) : nullptr;
	if (sceneUuid) {
		if (std::find(visitedSceneUuids.begin(), visitedSceneUuids.end(),
			      sceneUuid) != visitedSceneUuids.end())
			return;
		visitedSceneUuids.push_back(sceneUuid);
	}

	struct EnumCtx {
		std::vector<DeviceInfo> *sources;
		std::vector<std::string> *visitedSceneUuids;
	};
	EnumCtx ctx{&sources, &visitedSceneUuids};

	auto enumCb = [](obs_scene_t *, obs_sceneitem_t *item,
			 void *param) -> bool {
		auto *ctx = static_cast<EnumCtx *>(param);
		if (!ctx || !item)
			return true;

		obs_source_t *src = obs_sceneitem_get_source(item);
		addAudioSourceIfMissing(*ctx->sources, src);

		obs_scene_t *nestedScene = obs_scene_from_source(src);
		if (nestedScene)
			addSceneAudioSources(nestedScene, *ctx->sources,
					     *ctx->visitedSceneUuids);
		return true;
	};

	obs_scene_enum_items(scene, enumCb, &ctx);
}

std::vector<DeviceInfo> SceneManager::listCurrentSceneAudioSources()
{
	std::vector<DeviceInfo> sources;
	addGlobalAudioSources(sources);

	obs_source_t *current = obs_frontend_get_current_scene();
	if (!current)
		return sources;

	obs_scene_t *scene = obs_scene_from_source(current);
	std::vector<std::string> visitedSceneUuids;
	addSceneAudioSources(scene, sources, visitedSceneUuids);
	obs_source_release(current);
	return sources;
}

bool SceneManager::restoreFromConfig()
{
	if (sceneUuid.empty())
		return false;

	// Restore scene
	obs_source_t *sceneSrcFound =
		obs_get_source_by_uuid(sceneUuid.c_str());
	if (!sceneSrcFound)
		return false;

	obs_scene_t *restoredScene = obs_scene_from_source(sceneSrcFound);
	if (!restoredScene) {
		obs_source_release(sceneSrcFound);
		return false;
	}
	releaseSceneRef();
	scene = restoredScene;
	sceneSrc = sceneSrcFound;
	connectSceneSignals();

	// Restore webcam
	if (!webcamUuid.empty()) {
		adoptSourceRef(webcamSrc,
			       obs_get_source_by_uuid(webcamUuid.c_str()));
		if (webcamSrc) {
			if (!webcamUsesExistingSource)
				obs_source_set_name(webcamSrc, WEBCAM_SOURCE_NAME);
			webcamItem = obs_scene_find_source_recursive(
				scene, obs_source_get_name(webcamSrc));
		}
	}

	// Restore mic
	if (!micUuid.empty()) {
		adoptSourceRef(micSrc,
			       obs_get_source_by_uuid(micUuid.c_str()));
		if (micSrc) {
			if (!micUsesExistingSource)
				obs_source_set_name(micSrc, MIC_SOURCE_NAME);
			micItem = obs_scene_find_source_recursive(
				scene, obs_source_get_name(micSrc));
		}
	}

	// Restore browser item reference
	if (!browserUuid.empty()) {
		obs_source_t *bSrc =
			obs_get_source_by_uuid(browserUuid.c_str());
		if (bSrc) {
			browserItem = obs_scene_find_source_recursive(
				scene, obs_source_get_name(bSrc));
			obs_source_release(bSrc);
		}
	}

	if (sceneItemLockBackupActive) {
		blog(LOG_WARNING,
		     "[VirtuEdit] Restoring scene item lock state left by an interrupted recording");
		lockSceneItems(false);
	}

	blog(LOG_INFO,
	     "[VirtuEdit] Restored from config: scene=%s webcam=%s reaction_audio=%zu",
	     sceneSrc ? "ok" : "no", webcamSrc ? "ok" : "no",
	     reactionAudioUuids.size());

	// Do not re-apply default layout here. OBS persists scene item
	// transforms, and reactors may have moved/resized the YouTube PiP.

	// Scene is enough to consider restore successful
	// (webcam/mic might have been removed or renamed)
	return scene != nullptr;
}

bool SceneManager::isSetupDone() const
{
	return scene != nullptr;
}

obs_source_t *SceneManager::getBrowserSource() const
{
	return browserItem ? obs_sceneitem_get_source(browserItem) : nullptr;
}

SceneManager::PlayerControlsState SceneManager::getPlayerControlsState() const
{
	PlayerControlsEnumCtx ctx;
	if (scene)
		obs_scene_enum_items(scene, enumPlayerControlsItems, &ctx);
	return ctx.state;
}

std::string SceneManager::getCurrentSceneId() const
{
	if (sceneUuid.empty())
		return "";
	return "source:" + sceneUuid;
}

std::string SceneManager::getCurrentSceneName() const
{
	if (!sceneSrc)
		return "";
	const char *name = obs_source_get_name(sceneSrc);
	return name ? name : "";
}

std::string SceneManager::getCurrentWebcamDeviceId() const
{
	if (webcamUsesExistingSource && !webcamUuid.empty())
		return "source:" + webcamUuid;
	if (!webcamSrc)
		return "";
	obs_data_t *s = obs_source_get_settings(webcamSrc);
#ifdef __APPLE__
	std::string id = obs_data_get_string(s, "device");
#else
	std::string id = obs_data_get_string(s, "video_device_id");
#endif
	obs_data_release(s);
	return id;
}

std::string SceneManager::getCurrentMicDeviceId() const
{
	if (micUsesExistingSource && !micUuid.empty())
		return "source:" + micUuid;
	if (!micSrc)
		return "";
	obs_data_t *s = obs_source_get_settings(micSrc);
	std::string id = obs_data_get_string(s, "device_id");
	obs_data_release(s);
	return id;
}

std::vector<std::string> SceneManager::getCurrentReactionAudioSourceIds() const
{
	std::vector<std::string> ids;
	if (!reactionAudioUuids.empty()) {
		for (const std::string &uuid : reactionAudioUuids) {
			if (!uuid.empty())
				ids.push_back("source:" + uuid);
		}
	} else if (!micUuid.empty()) {
		ids.push_back("source:" + micUuid);
	}
	return ids;
}
