#pragma once

#include <obs.h>
#include "reaction-encoder-settings.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

class WebcamRecorder {
public:
	// (success, finalPath) — finalPath is the selected final container on success
	using FinalizeCallback =
		std::function<void(bool success, const std::string &finalPath)>;

	WebcamRecorder() = default;
	~WebcamRecorder();

	// Start recording a reaction scene to a separate .mkv file.
	// excludeSrc is hidden in the private recording scene, so the reaction
	// video captures the user's OBS scene without YouTube Player Controls.
	// reactionAudioMixerIdx: audio mixer index for reaction audio (0-based)
	bool start(obs_scene_t *reactionScene, obs_source_t *excludeSrc,
		   const std::string &mkvPath, int reactionAudioMixerIdx,
		   const ReactionEncoderCustomSettings *customSettings);

	static ReactionEncoderSettings
	getEncoderSettings(int reactionAudioMixerIdx,
			   const ReactionEncoderCustomSettings *customSettings);

	// Stop recording, wait for finalize, then remux to the selected format
	// when needed (async).
	// onFinalize fires (on a background thread) with the resulting path.
	void stop(const std::string &finalFormat, FinalizeCallback onFinalize);

	bool isActive() const { return active; }
	bool isFinalizing() const { return finalizing; }
	std::string getOutputPath() const { return outPath; }

private:
	void cleanup();
	void joinFinalizeThread();

	std::thread finalizeThread;
	obs_scene_t *scene = nullptr;
	obs_view_t *view = nullptr;
	video_t *videoOut = nullptr;
	obs_encoder_t *venc = nullptr;
	obs_encoder_t *aenc = nullptr;
	obs_output_t *fileOutput = nullptr;
	std::atomic<bool> active{false};
	std::atomic<bool> finalizing{false};
	std::string outPath;
};
