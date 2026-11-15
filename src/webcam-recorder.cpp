#include "webcam-recorder.hpp"
#include "ffmpeg-extract.hpp"
#include "obs-utils.hpp"
#include <obs.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

WebcamRecorder::~WebcamRecorder()
{
	// Synchronous teardown — no async remux (object is being destroyed)
	if (active && fileOutput && obs_output_active(fileOutput)) {
		obs_output_stop(fileOutput);
		for (int i = 0; i < 100; i++) {
			if (!obs_output_active(fileOutput))
				break;
			os_sleep_ms(50);
		}
	}
	joinFinalizeThread();
	active = false;
	finalizing = false;
	cleanup();
}

static bool sourceMatches(obs_source_t *src, obs_source_t *target,
			  const char *targetUuid)
{
	if (!src || !target)
		return false;
	if (src == target)
		return true;
	const char *uuid = obs_source_get_uuid(src);
	return uuid && targetUuid && std::strcmp(uuid, targetUuid) == 0;
}

struct HideCtx {
	obs_source_t *source;
	const char *uuid;
	int hidden = 0;
};

static int clampInt(int value, int minValue, int maxValue)
{
	if (value < minValue)
		return minValue;
	if (value > maxValue)
		return maxValue;
	return value;
}

static const char *configStringOr(config_t *config, const char *section,
				  const char *name, const char *fallback)
{
	const char *value = config ? config_get_string(config, section, name) : nullptr;
	return (value && *value) ? value : fallback;
}

static int configIntOr(config_t *config, const char *section, const char *name,
		       int fallback)
{
	if (!config)
		return fallback;
	const long long value = config_get_int(config, section, name);
	return value > 0 ? (int)value : fallback;
}

static int simpleOutputCrf(config_t *config, int baseCrf, bool lowCpuX264)
{
	const unsigned int cx =
		(unsigned int)config_get_uint(config, "Video", "OutputCX");
	const unsigned int cy =
		(unsigned int)config_get_uint(config, "Video", "OutputCY");
	const double outputCx = cx > 0 ? (double)cx : 1280.0;
	const double outputCy = cy > 0 ? (double)cy : 720.0;

	int crf = baseCrf;
	if (lowCpuX264)
		crf -= 2;

	const double crossDist = std::sqrt(outputCx * outputCx + outputCy * outputCy);
	const double cutoff = 2000.0;
	const double reducedDist = std::fmin(cutoff, crossDist) / cutoff;
	const double crfReduction = (1.0 - reducedDist) * 10.0;
	return clampInt(crf - (int)crfReduction, 0, 51);
}

static int advancedAudioBitrate(config_t *config, int reactionAudioMixerIdx)
{
	if (!config)
		return 192;

	int track = reactionAudioMixerIdx >= 0 ? reactionAudioMixerIdx + 1 : 1;
	track = clampInt(track, 1, 6);
	char key[32];
	snprintf(key, sizeof(key), "Track%dBitrate", track);
	return configIntOr(config, "AdvOut", key, 192);
}

ReactionEncoderSettings
WebcamRecorder::getEncoderSettings(int reactionAudioMixerIdx,
				   const ReactionEncoderCustomSettings *customSettings)
{
	ReactionEncoderSettings settings;
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return settings;

	const char *mode = configStringOr(config, "Output", "Mode", "Simple");
	settings.outputMode = mode;
	if (std::strcmp(mode, "Simple") == 0) {
		const char *quality =
			configStringOr(config, "SimpleOutput", "RecQuality", "Stream");
		const char *recEncoder =
			configStringOr(config, "SimpleOutput", "RecEncoder", "x264");
		const bool lowCpuX264 = std::strcmp(recEncoder, "x264_lowcpu") == 0;
		settings.sourceDescription = "Simple Output";

		if (std::strcmp(quality, "Stream") == 0) {
			settings.rateControl = "CBR";
			settings.videoBitrate =
				configIntOr(config, "SimpleOutput", "VBitrate", 6000);
			settings.audioBitrate =
				configIntOr(config, "SimpleOutput", "ABitrate", 160);
			settings.preset =
				configStringOr(config, "SimpleOutput", "Preset",
					       lowCpuX264 ? "ultrafast" : "veryfast");
		} else {
			const bool hq = std::strcmp(quality, "HQ") == 0;
			const bool lossless = std::strcmp(quality, "Lossless") == 0;
			settings.rateControl = "CRF";
			settings.crf = simpleOutputCrf(config, lossless ? 14 : (hq ? 16 : 23),
						      lowCpuX264);
			settings.audioBitrate = 192;
			settings.preset = lowCpuX264 ? "ultrafast" : "veryfast";
		}
	} else {
		settings.sourceDescription = "Advanced Output";
		settings.audioBitrate =
			advancedAudioBitrate(config, reactionAudioMixerIdx);
	}

	if (customSettings && customSettings->enabled) {
		if (settings.rateControl == "CBR")
			settings.videoBitrate = customSettings->videoBitrate;
		else
			settings.crf = customSettings->crf;
		settings.audioBitrate = customSettings->audioBitrate;
	}

	settings.videoBitrate = clampInt(settings.videoBitrate, 500, 100000);
	settings.crf = clampInt(settings.crf, 0, 51);
	settings.audioBitrate = clampInt(settings.audioBitrate, 64, 320);
	return settings;
}

static bool hideExcludedSourceItems(obs_scene_t *, obs_sceneitem_t *item,
				    void *param)
{
	auto *ctx = static_cast<HideCtx *>(param);
	if (!item || !ctx)
		return true;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (sourceMatches(src, ctx->source, ctx->uuid)) {
		obs_sceneitem_set_visible(item, false);
		ctx->hidden++;
		return true;
	}

	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, hideExcludedSourceItems,
					       param);

	return true;
}

bool WebcamRecorder::start(obs_scene_t *reactionScene, obs_source_t *excludeSrc,
			   const std::string &outputPath,
			   int reactionAudioMixerIdx,
			   const ReactionEncoderCustomSettings *customSettings)
{
	if (active || finalizing || !reactionScene || !excludeSrc)
		return false;

	joinFinalizeThread();
	cleanup();
	outPath = outputPath;

	// 1. Duplicate the selected reaction scene, then hide Player Controls.
	// This preserves the user's OBS layout, filters, images, and groups while
	// keeping the reaction-only output free of the YouTube overlay.
	scene = obs_scene_duplicate(reactionScene,
				    "Virtual Recorder Reaction Record",
				    OBS_SCENE_DUP_PRIVATE_REFS);
	if (!scene) {
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: failed to duplicate reaction scene");
		cleanup();
		return false;
	}

	HideCtx ctx = {excludeSrc, obs_source_get_uuid(excludeSrc), 0};
	obs_scene_enum_items(scene, hideExcludedSourceItems, &ctx);
	blog(LOG_INFO,
	     "[VirtuEdit] ReactionRecorder: hidden %d Player Controls item(s)",
	     ctx.hidden);
	if (ctx.hidden <= 0) {
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: Player Controls not found in reaction scene");
		cleanup();
		return false;
	}

	// 2. Create view and get video output
	view = obs_view_create();
	if (!view) {
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: failed to create view");
		cleanup();
		return false;
	}

	obs_source_t *sceneSrc = obs_scene_get_source(scene);
	obs_view_set_source(view, 0, sceneSrc);
	videoOut = obs_view_add(view);
	if (!videoOut) {
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: failed to add view");
		cleanup();
		return false;
	}

	ReactionEncoderSettings encoderSettings =
		getEncoderSettings(reactionAudioMixerIdx, customSettings);

	// 3. Create video encoder. Use x264 for the separate Reaction Video, but
	// mirror the current OBS recording quality/bitrate where it is safe.
	obs_data_t *venc_s = obs_data_create();
	obs_data_set_string(venc_s, "rate_control",
			    encoderSettings.rateControl.c_str());
	obs_data_set_string(venc_s, "preset", encoderSettings.preset.c_str());
	obs_data_set_string(venc_s, "profile", encoderSettings.profile.c_str());
	if (encoderSettings.rateControl == "CBR") {
		obs_data_set_int(venc_s, "bitrate",
				 encoderSettings.videoBitrate);
		obs_data_set_int(venc_s, "buffer_size",
				 encoderSettings.videoBitrate);
		obs_data_set_bool(venc_s, "use_bufsize", true);
	} else {
		obs_data_set_int(venc_s, "crf", encoderSettings.crf);
		obs_data_set_bool(venc_s, "use_bufsize", true);
	}
	venc = obs_video_encoder_create("obs_x264",
					"Virtual Recorder Reaction VEnc", venc_s,
					nullptr);
	obs_data_release(venc_s);
	if (!venc) {
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: failed to create venc");
		cleanup();
		return false;
	}
	obs_encoder_set_video(venc, videoOut);

	// 4. Create audio encoder.
	//    mixer_idx = reactionAudioMixerIdx to encode only reaction audio.
	size_t mixIdx = (reactionAudioMixerIdx >= 0)
				? (size_t)reactionAudioMixerIdx
				: 0;
	obs_data_t *aenc_s = obs_data_create();
	obs_data_set_int(aenc_s, "bitrate", encoderSettings.audioBitrate);
	aenc = obs_audio_encoder_create("ffmpeg_aac",
					"Virtual Recorder Reaction AEnc", aenc_s,
					mixIdx, nullptr);
	obs_data_release(aenc_s);
	if (!aenc) {
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: failed to create aenc");
		cleanup();
		return false;
	}
	obs_encoder_set_audio(aenc, obs_get_audio());

	// 5. Create file output (MKV for crash resilience)
	obs_data_t *out_s = obs_data_create();
	obs_data_set_string(out_s, "path", outputPath.c_str());
	fileOutput = obs_output_create("ffmpeg_muxer",
				       "Virtual Recorder Reaction Output", out_s,
				       nullptr);
	obs_data_release(out_s);
	if (!fileOutput) {
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: failed to create output");
		cleanup();
		return false;
	}

	obs_output_set_video_encoder(fileOutput, venc);
	obs_output_set_audio_encoder(fileOutput, aenc, 0);

	// Only record reaction audio (separated track)
	if (reactionAudioMixerIdx >= 0)
		obs_output_set_mixers(fileOutput,
				      1u << (unsigned)reactionAudioMixerIdx);

	// 6. Start
	if (!obs_output_start(fileOutput)) {
		const char *err = obs_output_get_last_error(fileOutput);
		blog(LOG_ERROR,
		     "[VirtuEdit] ReactionRecorder: start failed: %s",
		     err ? err : "unknown");
		cleanup();
		return false;
	}

	active = true;
	blog(LOG_INFO,
	     "[VirtuEdit] Reaction scene recorder started: %s video=%s bitrate=%d crf=%d preset=%s audio=%d",
	     outputPath.c_str(), encoderSettings.rateControl.c_str(),
	     encoderSettings.videoBitrate, encoderSettings.crf,
	     encoderSettings.preset.c_str(), encoderSettings.audioBitrate);
	return true;
}

static std::string sanitizeFinalFormat(std::string format)
{
	std::transform(format.begin(), format.end(), format.begin(),
		       [](unsigned char ch) {
			       return static_cast<char>(std::tolower(ch));
		       });
	if (format == "mp4" || format == "mov" || format == "mkv")
		return format;
	return "mp4";
}

static std::string mkvToFormat(const std::string &path,
			       const std::string &format)
{
	auto dot = path.rfind('.');
	if (dot == std::string::npos)
		return path + "." + format;
	return path.substr(0, dot) + "." + format;
}

void WebcamRecorder::stop(const std::string &finalFormat,
			  FinalizeCallback onFinalize)
{
	if (!active) {
		if (onFinalize)
			onFinalize(false, "");
		return;
	}

	active = false;
	finalizing = true;

	if (fileOutput && obs_output_active(fileOutput))
		obs_output_stop(fileOutput);

	std::string mkvPath = outPath;
	std::string targetFormat = sanitizeFinalFormat(finalFormat);
	std::string finalPath = mkvToFormat(outPath, targetFormat);

	// Wait for the muxer to finalize (write trailer/index), then remux.
	// Done on a background thread so the UI isn't blocked.
	finalizeThread = std::thread([this, mkvPath, targetFormat, finalPath,
				      onFinalize]() {
		// Poll until the output is fully stopped (max ~5s)
		for (int i = 0; i < 100; i++) {
			if (!fileOutput || !obs_output_active(fileOutput))
				break;
			os_sleep_ms(50);
		}

		cleanup();

		blog(LOG_INFO,
		     "[VirtuEdit] ReactionRecorder stopped, finalizing %s as %s",
		     mkvPath.c_str(), targetFormat.c_str());

		if (targetFormat == "mkv") {
			finalizing = false;
			if (onFinalize)
				onFinalize(true, mkvPath);
			return;
		}

		// Remux mkv -> selected container (no re-encode), delete the mkv
		// on success.
		bool ok = FfmpegExtract::remuxBlocking(mkvPath, finalPath, true);
		finalizing = false;
		if (onFinalize)
			onFinalize(ok, ok ? finalPath : "");
	});
}

void WebcamRecorder::joinFinalizeThread()
{
	if (finalizeThread.joinable())
		finalizeThread.join();
}

void WebcamRecorder::cleanup()
{
	if (fileOutput) {
		obs_output_release(fileOutput);
		fileOutput = nullptr;
	}
	if (venc) {
		obs_encoder_release(venc);
		venc = nullptr;
	}
	if (aenc) {
		obs_encoder_release(aenc);
		aenc = nullptr;
	}
	if (view) {
		if (videoOut) {
			obs_view_remove(view);
			videoOut = nullptr;
		}
		obs_view_destroy(view);
		view = nullptr;
	}
	if (scene) {
		obs_scene_release(scene);
		scene = nullptr;
	}
}
