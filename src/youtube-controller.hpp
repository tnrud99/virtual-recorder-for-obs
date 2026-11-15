#pragma once

#include "websocket-server.hpp"
#include <obs.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct SyncPoint {
	double reaction_time;
	double youtube_time;   // -1 means null
	double relative_youtube_time;
	std::string event;
	double youtube_first_play_time; // -1 means null
};

struct YouTubeInfo {
	std::string title;
	double duration = 0;
	int state = -1;
	double currentTime = 0;
};

class YouTubeController {
public:
	using StateCallback = std::function<void(const YouTubeInfo &)>;
	using TimestampCallback = std::function<void(const SyncPoint &)>;
	using VolumeCallback = std::function<void(int)>;
	using ErrorCallback = std::function<void(int)>;
	using RecordingStoppedCallback = std::function<void()>;

	YouTubeController();
	~YouTubeController();

	bool init();
	void shutdown();

	// Browser source management
	void createBrowserSource(const std::string &videoId);
	void destroyBrowserSource();
	void setBrowserSourceFromScene(obs_source_t *src);
	obs_source_t *getBrowserSource() const;
	bool repairBrowserSourceSettings(const std::string &videoId);

	// Playback control (sends commands to obs-player.html)
	void loadVideo(const std::string &videoId);
	void clearVideo(); // reset browser source to blank player
	void play();
	void pause();
	void seek(double timeSec);
	void setVolume(int vol);

	// Recording control
	void startRecording();
	void stopRecording();
	void clearSyncPoints();

	// Callbacks for dock UI updates
	void setOnStateChange(StateCallback cb);
	void setOnTimestamp(TimestampCallback cb);
	void setOnVolumeChange(VolumeCallback cb);
	void setOnError(ErrorCallback cb);
	void setOnRecordingStopped(RecordingStoppedCallback cb);

	// State
	YouTubeInfo getInfo() const
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		return info;
	}
	std::string getCurrentVideoId() const
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		return currentVideoId;
	}
	std::vector<SyncPoint> getSyncPoints() const
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		return syncPoints;
	}
	bool isRecording() const
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		return recording;
	}
	int getWsPort() const;

private:
	void sendCommand(const std::string &json);
	bool onWsMessage(const std::string &msg);
	void injectJsCommand(const std::string &json);
	bool acceptsVideoMessage(const std::string &messageVideoId) const;
	bool acceptsRecordingMessage(int messageRecordingId) const;
	bool acceptsProducerMessage(const std::string &messageProducerId);
	bool acceptsToken(const std::string &messageToken) const;
	std::string buildPlayerUrl(const std::string &videoId) const;

	WebSocketServer wsServer;
	obs_source_t *browserSource = nullptr;
	std::string currentVideoId;
	std::string sessionToken;
	bool playerVersionWarningLogged = false;
	bool recording = false;
	bool stopRequested = false;
	int recordingGeneration = 0;
	std::string activeProducerId;

	YouTubeInfo info;
	std::vector<SyncPoint> syncPoints;

	StateCallback onStateChange;
	TimestampCallback onTimestamp;
	VolumeCallback onVolumeChange;
	ErrorCallback onError;
	RecordingStoppedCallback onRecordingStopped;
	mutable std::mutex cbMtx;
};
