#pragma once

#include <obs.h>
#include <obs-module.h>
#include "reaction-encoder-settings.hpp"
#include <string>
#include <vector>

struct DeviceInfo {
	std::string id;
	std::string name;
};

class SceneManager {
public:
	SceneManager();
	~SceneManager();

	// Scene + source setup (called once from Setup page)
	bool createScene();
	bool selectReactionScene(const std::string &sourceUuid);
	bool addBrowserSource(obs_source_t *browserSrc);
	bool resetPlayerControls(obs_source_t *browserSrc);
	bool resetBrowserLayout();
	bool addWebcamSource(const std::string &deviceId);
	bool addExistingWebcamSource(const std::string &sourceUuid);
	bool addMicSource(const std::string &deviceId);
	bool addExistingMicSource(const std::string &sourceUuid);
	bool setReactionAudioSources(
		const std::vector<std::string> &sourceUuids);
	void cleanupCaptureSceneItems();

	// Switch to VirtuEdit scene
	void activateScene();

	// Lock scene items during recording (prevent drag/resize)
	void lockSceneItems(bool locked);

	// Audio track setup (called before each recording)
	bool setupAudioTracks();
	void restoreAudioTracks();
	bool restorePendingAudioTracks();
	int getSeparatedTrackIndex() const { return separatedTrack; }
	std::string getLastAudioSetupError() const { return audioSetupError; }

	// Webcam overlay position (normalized 0~1)
	struct OverlayPos {
		float x, y, w, h;
	};
	OverlayPos readOverlayPosition();

	// Player Controls items in the selected reaction scene.
	struct PlayerControlsState {
		int count = 0;
		int visibleCount = 0;
		bool hasHidden = false;
		bool exactlyOneVisible() const
		{
			return count == 1 && visibleCount == 1;
		}
	};
	PlayerControlsState getPlayerControlsState() const;

	// Device enumeration
	static std::vector<DeviceInfo> listWebcams();
	static std::vector<DeviceInfo> listMicrophones();
	static std::vector<DeviceInfo> listScenes();
	static DeviceInfo currentFrontendScene();
	static std::vector<DeviceInfo> listExistingVideoSources();
	static std::vector<DeviceInfo> listExistingAudioSources();
	static std::vector<DeviceInfo> listCurrentSceneAudioSources();

	// Restore previous session's sources from saved UUIDs
	bool restoreFromConfig();

	// State
	bool isSetupDone() const;
	bool isOnboardingSeen() const { return onboardingSeen; }
	void setOnboardingSeen(bool seen);
	bool isInitialDockPlacementShown() const
	{
		return initialDockFloatShown;
	}
	void setInitialDockPlacementShown(bool shown);
	bool isPlayerControlsSizeInitialized() const
	{
		return playerControlsSizeInitialized;
	}
	void setPlayerControlsSizeInitialized(bool initialized);
	std::string getCurrentWebcamDeviceId() const;
	std::string getCurrentMicDeviceId() const;
	std::vector<std::string> getCurrentReactionAudioSourceIds() const;
	std::string getCurrentSceneId() const;
	std::string getCurrentSceneName() const;

	// Persisted YouTube volume (relative volume default), 0-100
	int getSavedVolume() const { return savedVolume; }
	void saveVolume(int v);
	ReactionEncoderCustomSettings getReactionEncoderCustomSettings() const;
	void saveReactionEncoderCustomSettings(
		const ReactionEncoderCustomSettings &settings);
	void clearReactionEncoderCustomSettings();
	std::string getReactionOutputFormat() const;
	void saveReactionOutputFormat(const std::string &format);
	obs_scene_t *getScene() const { return scene; }
	obs_source_t *getBrowserSource() const;
	obs_source_t *getWebcamSource() const { return webcamSrc; }
	obs_source_t *getMicSource() const { return micSrc; }

private:
	struct MixerBackup {
		std::string uuid;
		std::string name;
		uint32_t mixers = 0;
	};

	obs_scene_t *scene = nullptr;
	obs_source_t *sceneSrc = nullptr;
	obs_source_t *webcamSrc = nullptr;
	obs_source_t *micSrc = nullptr;
	obs_sceneitem_t *browserItem = nullptr;
	obs_sceneitem_t *webcamItem = nullptr;
	obs_sceneitem_t *micItem = nullptr;

	int separatedTrack = -1;
	int pendingSeparatedTrack = -1;
	bool trackSetupActive = false;
	bool audioRestorePending = false;
	std::string audioSetupError;
	std::vector<MixerBackup> mixerBackups;
	std::vector<MixerBackup> pendingMixerBackups;
	bool sceneItemLockBackupActive = false;
	bool browserItemWasLocked = false;
	bool webcamItemWasLocked = false;

	// UUID-based source tracking (survives renames)
	std::string sceneUuid;
	std::string webcamUuid;
	std::string micUuid;
	std::vector<std::string> reactionAudioUuids;
	std::string browserUuid;
	bool webcamUsesExistingSource = false;
	bool micUsesExistingSource = false;
	int savedVolume = 50; // default YouTube volume, 0-100
	ReactionEncoderCustomSettings reactionEncoderCustom;
	std::string reactionOutputFormat;
	bool onboardingSeen = false;
	bool initialDockFloatShown = false;
	bool playerControlsSizeInitialized = false;
	bool sceneSignalsConnected = false;
	bool updatingBrowserTransform = false;
	void saveConfig();
	void loadConfig();
	void writeAudioRestoreBackup(obs_data_t *data) const;
	void loadAudioRestoreBackup(obs_data_t *data);
	void releaseSourceRef(obs_source_t *&target);
	void adoptSourceRef(obs_source_t *&target, obs_source_t *next);
	void releaseSceneRef();
	int removeSceneItemsForSource(obs_source_t *source,
				      obs_sceneitem_t *keepItem);
	void applyDefaultBrowserLayout();
	bool syncBrowserOverlayTransform();
	void connectSceneSignals();
	void disconnectSceneSignals();
	obs_source_t *findSourceByUuidOrName(const std::string &uuid,
					     const char *fallbackName);
	static void onSceneItemTransformed(void *data, calldata_t *params);
	static void onSceneItemRemoved(void *data, calldata_t *params);
};
