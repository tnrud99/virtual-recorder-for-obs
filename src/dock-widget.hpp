#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QBoxLayout>
#include <QToolButton>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QResizeEvent>
#include <vector>
#include "webcam-recorder.hpp"
#include "scene-manager.hpp"

class YouTubeController;

class VirtuEditDock : public QWidget {
	Q_OBJECT

public:
	explicit VirtuEditDock(QWidget *parent,
			       YouTubeController *controller,
			       SceneManager *sceneMgr);
	~VirtuEditDock() override;

	// Called when previous session is restored after scenes load
	void onSessionRestored();
	void onFrontendSceneChanged();
	void onCompositeRecordingStarted();
	void onCompositeRecordingReady(const std::string &path);

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	enum class CompletionAttention { Normal, NeedsAttention };

	void onSetupComplete();
	void onOpenSettings();
	void onBackStep();
	void onNextStep();
	void onSkipToRecord();
	void onUrlSubmit();
	void onCancelVideoChange();
	void onStartRecording();
	void onOpenProjector();
	void onVolumeChanged(int value);
	void onStopRecording();
	void onRecordAnotherTake();
	void onNewVideo();
	void onOpenOutputFolder();
	void updateRecordingTimer();
	void onYouTubeStateChanged();
	void onTimestampReceived();
	void onFixPlayerControls();

private:
	void setupSetupPage();
	void setupReadyPage();
	void setupRecordingPage();
	void setupCompletePage();
	void updateReleaseNoticeVisibility();
	void showPostSetupPage();
	void populateDeviceLists();
	void updateSetupAudioWarning();
	void setVolumeUI(int value);          // update sliders/labels only
	void onPlayerVolumeChanged(int value); // volume changed inside the player
	void onVideoError(int code);          // YouTube load/playback error
	void onLoadTimeout();                 // video never reported ready
	void saveSyncJson();
	bool canSaveSyncJsonNow() const;
	void saveSyncJsonIfOutputReady();
	void setRecordingState(bool starting, bool active, bool stopping,
			       const char *context = nullptr);
	void restoreRecordingSceneState();
	void clearRecordingOutputState();
	void stopActiveOutputsForCleanup();
	bool isRecordingFlowBusy() const;
	bool isCompletionProcessing() const;
	void updateCompletionActions();
	int beginRecordingSession();
	bool isCurrentRecordingSession(int session) const;
	void scheduleCompositeFinalizeWatchdogs(int session);
	void finishCompositeRecording(int session, const std::string &path);
	void setReactOutputStatus(const QString &text, const char *style,
				  CompletionAttention attention,
				  const QString &detailTooltip = QString());
	void setJsonOutputStatus(const QString &text, const char *style,
				 CompletionAttention attention,
				 const QString &detailTooltip = QString());
	void setCompositeOutputStatus(const QString &text, const char *style,
				      CompletionAttention attention,
				      const QString &detailTooltip = QString());
	void updateOpenFolderButton();
	std::string currentOutputFolder() const;
	std::string selectedReactionOutputFormat() const;
	void abortRecordingStart(const QString &message);
	void updateReadiness();
	void updateVideoControls();
	void updateReactionSettingsUI();
	void onReactionSettingsEdited();
	void onReactionFormatChanged();
	void onReactionSettingsReset();
	void updateResponsiveLayout();
	void setReadyDetailsExpanded(bool expanded);
	bool ensureYouTubePlayerReady(const std::string &videoId);
	bool openPlayerControlsWindow(bool requireVideoLoaded);
	void clearCompletionOutputState();
	bool canKeepCurrentLoadedVideo() const;
	QString currentVideoSummary() const;
	QString currentLoadedVideoLink() const;
	bool restorePreviousVideoAfterLoadFailure(const QString &reason);
	std::string getRecordingOutputDir() const;
	std::string extractVideoId(const QString &url);
	std::string basename(const std::string &path) const;

	YouTubeController *yt = nullptr;
	SceneManager *sceneMgr = nullptr;

	QStackedWidget *pages = nullptr;
	QLabel *releaseNotice = nullptr;
	QUrl releaseDownloadUrl;

	// Setup page
	QWidget *setupPage = nullptr;
	QWidget *sourcePickerWidget = nullptr;
	QLabel *currentSceneLabel = nullptr;
	QListWidget *audioSourceList = nullptr;
	QToolButton *rescanSourcesBtn = nullptr;
	QPushButton *setupCompleteBtn = nullptr;
	QLabel *setupStatusLabel = nullptr;
	QLabel *setupSourceHintLabel = nullptr;

	// Ready page
	QWidget *readyPage = nullptr;
	QLabel *readyTitleLabel = nullptr;
	QLabel *readyCaptionLabel = nullptr;
	QWidget *readyNavWidget = nullptr;
	QPushButton *backStepBtn = nullptr;
	QPushButton *nextStepBtn = nullptr;
	QWidget *skipToRecordWidget = nullptr;
	QPushButton *skipToRecordBtn = nullptr;
	QWidget *videoStepWidget = nullptr;
	QWidget *urlEditorWidget = nullptr;
	QLineEdit *urlInput = nullptr;
	QPushButton *loadVideoBtn = nullptr;
	QLabel *savedDeviceNoteLabel = nullptr;
	QWidget *loadedVideoChoiceWidget = nullptr;
	QLabel *loadedVideoTitleLabel = nullptr;
	QLabel *loadedVideoMetaLabel = nullptr;
	QPushButton *changeVideoBtn = nullptr;
	QWidget *videoSummaryWidget = nullptr;
	QWidget *captureSettingsWidget = nullptr;
	QWidget *devicePromptWidget = nullptr;
	QLabel *devicePromptLabel = nullptr;
	QPushButton *devicePromptBtn = nullptr;
	QLabel *cameraDeviceLabel = nullptr;
	QLabel *micDeviceLabel = nullptr;
	QLabel *loadedVideoLinkLabel = nullptr;
	QWidget *sceneLayoutWidget = nullptr;
	QLabel *sceneLayoutLabel = nullptr;
	QLabel *sceneLayoutNoteLabel = nullptr;
	QWidget *reactionSettingsWidget = nullptr;
	QLabel *reactionSettingsImportedLabel = nullptr;
	QLabel *reactionSettingsResultLabel = nullptr;
	QLabel *reactionVideoSettingLabel = nullptr;
	QComboBox *reactionVideoSettingCombo = nullptr;
	QComboBox *reactionAudioBitrateCombo = nullptr;
	QComboBox *reactionFormatCombo = nullptr;
	QPushButton *reactionResetObsBtn = nullptr;
	QWidget *postLoadControlsWidget = nullptr;
	QLabel *recordFlowLabel = nullptr;
	QPushButton *fixPlayerControlsBtn = nullptr;
	QLabel *playerControlsRepairHintLabel = nullptr;
	QPushButton *openPlayerReadyBtn = nullptr;
	QWidget *readyVolumeWidget = nullptr;
	QLabel *audioBleedWarningLabel = nullptr;
	QLabel *audioBleedNoteLabel = nullptr;
	QLabel *videoInfoLabel = nullptr;
	QWidget *readyStatusWidget = nullptr;
	QLabel *readySummaryLabel = nullptr;
	QToolButton *readyDetailsBtn = nullptr;
	QWidget *readyDetailsWidget = nullptr;
	QLabel *deviceReadyLabel = nullptr;
	QLabel *youtubeReadyLabel = nullptr;
	QLabel *controlsReadyLabel = nullptr;
	QLabel *canvasReadyLabel = nullptr;
	QLabel *diskReadyLabel = nullptr;
	QLabel *obsReadyLabel = nullptr;
	QPushButton *startRecBtn = nullptr;
	QSlider *volSliderReady = nullptr;
	QLabel *volValReady = nullptr;

	// Recording page
	QWidget *recordingPage = nullptr;
	QLabel *playbackLabel = nullptr;
	QLabel *recTimeLabel = nullptr;
	QLabel *syncCountLabel = nullptr;
	QPushButton *stopRecBtn = nullptr;
	QSlider *volSliderRec = nullptr;
	QLabel *volValRec = nullptr;
	QTimer *recTimer = nullptr;
	QTimer *loadTimer = nullptr;
	qint64 recStartMs = 0;

	// Complete page
	QWidget *completePage = nullptr;
	QLabel *completeTitleLabel = nullptr;
	QLabel *completeInfoLabel = nullptr;
	QLabel *completeSyncLabel = nullptr;
	QLabel *compositeStatusLabel = nullptr;
	QLabel *reactStatusLabel = nullptr;
	QLabel *jsonStatusLabel = nullptr;
	QPushButton *openFolderBtn = nullptr;
	QPushButton *recordAnotherBtn = nullptr;
	QPushButton *newVideoBtn = nullptr;
	QBoxLayout *nextTakeLayout = nullptr;

	WebcamRecorder webcamRecorder;
	int relativeVolume = 50; // YouTube volume 0-100
	bool volumeDragging = false; // user is dragging a volume slider
	bool videoLoaded = false;
	bool videoLoading = false;
	bool editingVideoUrl = true;
	int readyWorkflowStep = 2;
	bool playerControlsOpened = false;
	bool recordingStarting = false;
	bool recordingActive = false;
	bool stoppingRecording = false;
	bool readyDetailsExpanded = false;
	bool hasLastOverlay = false;
	bool waitingForComposite = false;
	bool compositePathReported = false;
	bool reactFinalizeFailed = false;
	bool reactOutputNeedsAttention = false;
	bool jsonOutputNeedsAttention = false;
	bool compositeOutputNeedsAttention = false;
	bool restoringRecordingScene = false;
	int recordingSessionId = 0;
	SceneManager::OverlayPos lastOverlay = {0.52f, 0.05f, 0.45f, 0.45f};
	QString loadedVideoUrl;
	QString pendingVideoUrl;
	QString fallbackVideoUrl;
	QString recordingSceneName;
	std::string recordingSceneUuid;
	std::string loadedVideoId;
	std::string pendingVideoId;
	std::string fallbackVideoId;
	std::string lastRecordingPath;
	std::string lastReactBase; // shared base for react + sync json
	std::string lastReactExpectedPath;
	std::string lastReactionOutputFormat;
	std::string lastReactPath;
	std::string lastJsonPath;
};
