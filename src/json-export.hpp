#pragma once

#include "youtube-controller.hpp"
#include "scene-manager.hpp"
#include <string>

class JsonExport {
public:
	// Build the full JSON string (editor-compatible format)
	static std::string buildJson(
		const std::string &videoId,
		const std::string &title,
		double duration,
		const std::string &reactionVideo,
		const std::string &compositeVideo,
		const std::vector<SyncPoint> &syncPoints,
		const SceneManager::OverlayPos &overlay,
		int youtubeVolume);

	// Save JSON next to the recording file: recording_sync.json
	static std::string saveNextToRecording(
		const std::string &recordingPath,
		const std::string &jsonContent);

	static std::string saveToPath(
		const std::string &jsonPath,
		const std::string &jsonContent);

	// Get the _reaction.mkv path from recording path
	static std::string getReactVideoPath(
		const std::string &recordingPath);
};
