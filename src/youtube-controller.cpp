#include "youtube-controller.hpp"
#include "obs-utils.hpp"
#include <obs.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

static const char *BROWSER_SOURCE_NAME = "Virtual Recorder Player Controls";
static const char *MANAGED_MARKER_KEY = "virtuedit_managed";
static const char *PLAYER_VERSION = "2026-06-24-recording-session-v1";
static const size_t MAX_SYNC_POINTS = 20000;

static void markManagedPlayerControls(obs_data_t *settings)
{
	if (settings)
		obs_data_set_bool(settings, MANAGED_MARKER_KEY, true);
}

static bool hasManagedPlayerControlsMarker(obs_source_t *source)
{
	if (!source)
		return false;
	obs_data_t *settings = obs_source_get_settings(source);
	bool managed = settings &&
		       obs_data_get_bool(settings, MANAGED_MARKER_KEY);
	if (settings)
		obs_data_release(settings);
	return managed;
}

// Simple JSON value extraction (avoids external JSON library dependency for now)
static std::string jsonString(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\"";
	auto pos = json.find(search);
	if (pos == std::string::npos)
		return "";
	pos = json.find(':', pos + search.size());
	if (pos == std::string::npos)
		return "";
	pos++;
	while (pos < json.size() && json[pos] == ' ')
		pos++;
	if (pos >= json.size())
		return "";
	if (json[pos] == '"') {
		std::string result;
		size_t i = pos + 1;
		while (i < json.size()) {
			if (json[i] == '\\' && i + 1 < json.size()) {
				char next = json[i + 1];
				switch (next) {
				case '"':
					result += '"';
					break;
				case '\\':
					result += '\\';
					break;
				case 'n':
					result += '\n';
					break;
				case 'r':
					result += '\r';
					break;
				case 't':
					result += '\t';
					break;
				case '/':
					result += '/';
					break;
				default:
					result += '\\';
					result += next;
					break;
				}
				i += 2;
			} else if (json[i] == '"') {
				break;
			} else {
				result += json[i];
				i++;
			}
		}
		return result;
	}
	// unquoted value (number, null, bool)
	auto end = json.find_first_of(",}", pos);
	if (end == std::string::npos)
		end = json.size();
	std::string val = json.substr(pos, end - pos);
	// trim whitespace
	while (!val.empty() && (val.back() == ' ' || val.back() == '\n'))
		val.pop_back();
	return val;
}

static double jsonDouble(const std::string &json, const std::string &key)
{
	std::string val = jsonString(json, key);
	if (val.empty() || val == "null")
		return -1.0;
	try {
		return std::stod(val);
	} catch (...) {
		return -1.0;
	}
}

static int jsonInt(const std::string &json, const std::string &key)
{
	std::string val = jsonString(json, key);
	if (val.empty() || val == "null")
		return -1;
	try {
		return std::stoi(val);
	} catch (...) {
		return -1;
	}
}

static bool isFiniteNumber(double value)
{
	return std::isfinite(value);
}

static bool isNullOrNonNegative(double value)
{
	return value < 0.0 || (isFiniteNumber(value) && value >= 0.0);
}

static bool isAllowedPlayerState(int state)
{
	return state == -1 || state == 0 || state == 1 || state == 2 ||
	       state == 3 || state == 5;
}

static std::string queryParamValue(const std::string &url,
				   const std::string &key)
{
	const std::string prefix = key + "=";
	size_t query = url.find('?');
	if (query == std::string::npos)
		return "";

	size_t pos = query + 1;
	while (pos < url.size()) {
		size_t end = url.find_first_of("&#", pos);
		if (end == std::string::npos)
			end = url.size();
		const std::string part = url.substr(pos, end - pos);
		if (part.rfind(prefix, 0) == 0)
			return part.substr(prefix.size());
		if (end >= url.size() || url[end] == '#')
			break;
		pos = end + 1;
	}
	return "";
}

static bool isAllowedSyncEvent(const std::string &event)
{
	return event == "start" || event == "youtube_play" ||
	       event == "youtube_pause" || event == "youtube_ended" ||
	       event == "end";
}

static std::string makeSessionToken()
{
	std::random_device rd;
	std::ostringstream out;
	out << std::hex << std::setfill('0');
	for (int i = 0; i < 4; ++i)
		out << std::setw(8) << rd();
	return out.str();
}

static void setBrowserPlayerSize(obs_data_t *settings)
{
	auto player = virtuedit_get_16x9_player_size();
	obs_data_set_int(settings, "width", player.width);
	obs_data_set_int(settings, "height", player.height);
}

bool YouTubeController::acceptsVideoMessage(
	const std::string &messageVideoId) const
{
	// Older hosted players did not include videoId on every message. Accept
	// those only while a video is expected; after New Video clears the current
	// id, late anonymous messages must not mark the blank player as loaded.
	if (messageVideoId.empty())
		return !currentVideoId.empty();
	return !currentVideoId.empty() && messageVideoId == currentVideoId;
}

bool YouTubeController::acceptsRecordingMessage(int messageRecordingId) const
{
	// Legacy hosted players did not include recording_id. Keep accepting them
	// so release order between the plugin and hosted player is not brittle.
	return messageRecordingId < 0 || messageRecordingId == recordingGeneration;
}

bool YouTubeController::acceptsProducerMessage(
	const std::string &messageProducerId)
{
	// Legacy players cannot be distinguished. New players claim a single active
	// timestamp producer per recording so duplicate OBS/browser views cannot
	// both append sync points.
	if (messageProducerId.empty())
		return true;
	if (activeProducerId.empty()) {
		activeProducerId = messageProducerId;
		blog(LOG_INFO, "[VirtuEdit] Timestamp producer selected: %s",
		     activeProducerId.c_str());
		return true;
	}
	return messageProducerId == activeProducerId;
}

bool YouTubeController::acceptsToken(const std::string &messageToken) const
{
	return !sessionToken.empty() && messageToken == sessionToken;
}

std::string YouTubeController::buildPlayerUrl(
	const std::string &videoId) const
{
	std::ostringstream url;
	url << "https://virtuedit.com/obs-player.html"
	    << "?wsPort=" << wsServer.getPort()
	    << "&token=" << sessionToken
	    << "&playerVersion=" << PLAYER_VERSION;
	if (!videoId.empty())
		url << "&v=" << videoId;
	return url.str();
}

// --- YouTubeController ---

YouTubeController::YouTubeController()
	: sessionToken(makeSessionToken())
{
}

YouTubeController::~YouTubeController()
{
	shutdown();
}

bool YouTubeController::init()
{
	wsServer.setOnMessage(
		[this](const std::string &msg) { return onWsMessage(msg); });
	return wsServer.start();
}

void YouTubeController::shutdown()
{
	// Stop inbound callbacks before any UI object that they reference is
	// destroyed. Clearing the callback waits for an in-flight callback to leave
	// the WebSocket server's callback mutex.
	wsServer.setOnMessage({});
	wsServer.stop();
	destroyBrowserSource();
}

void YouTubeController::createBrowserSource(const std::string &videoId)
{
	// Reuse if already held
	if (browserSource) {
		{
			std::lock_guard<std::mutex> lock(cbMtx);
			currentVideoId = videoId;
			info = YouTubeInfo{};
			recording = false;
			stopRequested = false;
			activeProducerId.clear();
			syncPoints.clear();
		}
		obs_source_set_name(browserSource, BROWSER_SOURCE_NAME);
		obs_source_set_monitoring_type(browserSource,
					       OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
		obs_data_t *settings = obs_source_get_settings(browserSource);
		const std::string url = buildPlayerUrl(videoId);
		obs_data_set_string(settings, "url", url.c_str());
		markManagedPlayerControls(settings);
		setBrowserPlayerSize(settings);
		obs_source_update(browserSource, settings);
		obs_data_release(settings);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(cbMtx);
		currentVideoId = videoId;
		info = YouTubeInfo{};
		recording = false;
		stopRequested = false;
		activeProducerId.clear();
		syncPoints.clear();
	}
	int wsPort = wsServer.getPort();

	obs_data_t *settings = obs_data_create();
	const std::string url = buildPlayerUrl(videoId);
	obs_data_set_string(settings, "url", url.c_str());
	markManagedPlayerControls(settings);
	setBrowserPlayerSize(settings);
	obs_data_set_bool(settings, "shutdown", false);
	obs_data_set_bool(settings, "reroute_audio", true);

	browserSource = obs_source_create("browser_source",
					  BROWSER_SOURCE_NAME, settings,
					  nullptr);
	obs_data_release(settings);

	if (!browserSource) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Failed to create browser source");
		return;
	}

	// Enable audio monitoring so the user hears YouTube through speakers
	obs_source_set_monitoring_type(browserSource,
				       OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);

	blog(LOG_INFO,
	     "[VirtuEdit] Browser source created: videoId=%s wsPort=%d",
	     videoId.c_str(), wsPort);
}

void YouTubeController::destroyBrowserSource()
{
	if (browserSource) {
		obs_source_release(browserSource);
		browserSource = nullptr;
	}
}

void YouTubeController::setBrowserSourceFromScene(obs_source_t *src)
{
	if (!src || browserSource)
		return;
	if (!hasManagedPlayerControlsMarker(src)) {
		blog(LOG_INFO,
		     "[VirtuEdit] Ignored unmarked browser source during startup restore");
		return;
	}
	obs_source_get_ref(src);
	browserSource = src;

	std::string restoredVideoId;
	obs_data_t *settings = obs_source_get_settings(src);
	if (settings) {
		const char *url = obs_data_get_string(settings, "url");
		if (url && *url)
			restoredVideoId = queryParamValue(url, "v");
		obs_data_release(settings);
	}

	// Startup restore must be passive. OBS sources can be shared across
	// scenes, so changing URL/size here can blank a user's preview before
	// they interact with Virtual Recorder.
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		currentVideoId = restoredVideoId;
		info = YouTubeInfo{};
		recording = false;
		stopRequested = false;
		activeProducerId.clear();
		syncPoints.clear();
	}

	if (restoredVideoId.empty()) {
		blog(LOG_INFO,
		     "[VirtuEdit] Browser source reference restored without changing settings");
	} else {
		blog(LOG_INFO,
		     "[VirtuEdit] Browser source reference restored without changing settings (videoId=%s)",
		     restoredVideoId.c_str());
	}
}

obs_source_t *YouTubeController::getBrowserSource() const
{
	return browserSource;
}

bool YouTubeController::repairBrowserSourceSettings(
	const std::string &videoId)
{
	if (!browserSource)
		return false;

	const std::string expectedUrl = buildPlayerUrl(videoId);
	auto player = virtuedit_get_16x9_player_size();
	obs_data_t *settings = obs_source_get_settings(browserSource);
	if (!settings)
		return false;

	const char *currentUrl = obs_data_get_string(settings, "url");
	const char *localFile = obs_data_get_string(settings, "local_file");
	const bool isLocalFile = obs_data_get_bool(settings, "is_local_file");
	const bool managed = obs_data_get_bool(settings, MANAGED_MARKER_KEY);
	const long long width = obs_data_get_int(settings, "width");
	const long long height = obs_data_get_int(settings, "height");
	const bool needsRepair =
		!managed || isLocalFile || (localFile && *localFile) ||
		expectedUrl != (currentUrl ? currentUrl : "") ||
		width != static_cast<long long>(player.width) ||
		height != static_cast<long long>(player.height);

	obs_source_set_name(browserSource, BROWSER_SOURCE_NAME);
	obs_source_set_monitoring_type(browserSource,
				       OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);

	if (needsRepair) {
		obs_data_set_bool(settings, "is_local_file", false);
		obs_data_set_string(settings, "local_file", "");
		obs_data_set_string(settings, "url", expectedUrl.c_str());
		markManagedPlayerControls(settings);
		setBrowserPlayerSize(settings);
		obs_data_set_bool(settings, "shutdown", false);
		obs_data_set_bool(settings, "reroute_audio", true);
		obs_source_update(browserSource, settings);
		blog(LOG_WARNING,
		     "[VirtuEdit] Repaired YouTube Controls browser source settings");
	}

	obs_data_release(settings);
	return needsRepair;
}

void YouTubeController::loadVideo(const std::string &videoId)
{
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		currentVideoId = videoId;
		info = YouTubeInfo{};
		recording = false;
		stopRequested = false;
		activeProducerId.clear();
		syncPoints.clear();
	}

	// Update browser source URL so CEF reloads with new video
	if (browserSource) {
		const std::string url = buildPlayerUrl(videoId);

		obs_source_set_monitoring_type(browserSource,
					       OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
		obs_data_t *settings = obs_source_get_settings(browserSource);
		obs_data_set_string(settings, "url", url.c_str());
		markManagedPlayerControls(settings);
		setBrowserPlayerSize(settings);
		obs_source_update(browserSource, settings);
		obs_data_release(settings);

		blog(LOG_INFO, "[VirtuEdit] Browser source URL updated: v=%s",
		     videoId.c_str());
	}
}

void YouTubeController::clearVideo()
{
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		currentVideoId.clear();
		info = YouTubeInfo{};
		recording = false;
		stopRequested = false;
		activeProducerId.clear();
		syncPoints.clear();
	}
	if (!browserSource)
		return;

	const std::string url = buildPlayerUrl("");

	obs_data_t *settings = obs_source_get_settings(browserSource);
	obs_data_set_string(settings, "url", url.c_str());
	markManagedPlayerControls(settings);
	setBrowserPlayerSize(settings);
	obs_source_update(browserSource, settings);
	obs_data_release(settings);

	blog(LOG_INFO, "[VirtuEdit] Browser source cleared to blank");
}

void YouTubeController::play()
{
	sendCommand("{\"action\":\"play\"}");
}

void YouTubeController::pause()
{
	sendCommand("{\"action\":\"pause\"}");
}

void YouTubeController::seek(double timeSec)
{
	std::ostringstream cmd;
	cmd << "{\"action\":\"seek\",\"time\":" << timeSec << "}";
	sendCommand(cmd.str());
}

void YouTubeController::setVolume(int vol)
{
	std::ostringstream cmd;
	cmd << "{\"action\":\"setVolume\",\"volume\":" << vol << "}";
	sendCommand(cmd.str());
}

void YouTubeController::startRecording()
{
	int generation = 0;
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		recording = true;
		stopRequested = false;
		recordingGeneration++;
		generation = recordingGeneration;
		activeProducerId.clear();
		syncPoints.clear();
	}
	std::ostringstream cmd;
	cmd << "{\"action\":\"startRecording\",\"recording_id\":"
	    << generation << "}";
	sendCommand(cmd.str());
	blog(LOG_INFO, "[VirtuEdit] Timestamp recording started");
}

void YouTubeController::stopRecording()
{
	bool shouldStop = false;
	int generation = 0;
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		if (recording && !stopRequested) {
			stopRequested = true;
			shouldStop = true;
			generation = recordingGeneration;
		}
	}
	if (!shouldStop)
		return;

	std::ostringstream cmd;
	cmd << "{\"action\":\"stopRecording\",\"recording_id\":"
	    << generation << "}";
	sendCommand(cmd.str());
	blog(LOG_INFO, "[VirtuEdit] Timestamp recording stop requested");
}

void YouTubeController::clearSyncPoints()
{
	int generation = 0;
	{
		std::lock_guard<std::mutex> lock(cbMtx);
		recording = false;
		stopRequested = false;
		recordingGeneration++;
		generation = recordingGeneration;
		activeProducerId.clear();
		syncPoints.clear();
	}
	std::ostringstream cmd;
	cmd << "{\"action\":\"resetRecording\",\"recording_id\":"
	    << generation << "}";
	sendCommand(cmd.str());
}

void YouTubeController::setOnStateChange(StateCallback cb)
{
	std::lock_guard<std::mutex> lock(cbMtx);
	onStateChange = std::move(cb);
}

void YouTubeController::setOnTimestamp(TimestampCallback cb)
{
	std::lock_guard<std::mutex> lock(cbMtx);
	onTimestamp = std::move(cb);
}

void YouTubeController::setOnVolumeChange(VolumeCallback cb)
{
	std::lock_guard<std::mutex> lock(cbMtx);
	onVolumeChange = std::move(cb);
}

void YouTubeController::setOnError(ErrorCallback cb)
{
	std::lock_guard<std::mutex> lock(cbMtx);
	onError = std::move(cb);
}

void YouTubeController::setOnRecordingStopped(RecordingStoppedCallback cb)
{
	std::lock_guard<std::mutex> lock(cbMtx);
	onRecordingStopped = std::move(cb);
}

int YouTubeController::getWsPort() const
{
	return wsServer.getPort();
}

void YouTubeController::sendCommand(const std::string &json)
{
	wsServer.sendMessage(json);

	// Fallback: also inject via proc_handler if browser source exists
	injectJsCommand(json);
}

bool YouTubeController::onWsMessage(const std::string &msg)
{
	std::string event = jsonString(msg, "event");
	std::string messageToken = jsonString(msg, "token");
	if (!acceptsToken(messageToken)) {
		blog(LOG_WARNING,
		     "[VirtuEdit] Closing obs-player client with invalid session token");
		return false;
	}

	if (event == "connected") {
		std::string version = jsonString(msg, "player_version");
		if (!version.empty() && version != PLAYER_VERSION &&
		    !playerVersionWarningLogged) {
			playerVersionWarningLogged = true;
			blog(LOG_WARNING,
			     "[VirtuEdit] obs-player version mismatch: got %s expected %s",
			     version.c_str(), PLAYER_VERSION);
		}
		blog(LOG_INFO,
		     "[VirtuEdit] obs-player.html connected (v=%s player=%s)",
		     jsonString(msg, "videoId").c_str(),
		     version.empty() ? "legacy" : version.c_str());
		return true;
	}

	if (event == "player_ready") {
		std::lock_guard<std::mutex> lock(cbMtx);
		if (!acceptsVideoMessage(jsonString(msg, "videoId"))) {
			blog(LOG_INFO,
			     "[VirtuEdit] Ignored stale YouTube ready message");
			return true;
		}
		double duration = jsonDouble(msg, "duration");
		if (!isFiniteNumber(duration) || duration < 0.0) {
			blog(LOG_WARNING,
			     "[VirtuEdit] Ignored invalid YouTube ready duration");
			return true;
		}
		info.title = jsonString(msg, "title");
		info.duration = duration;
		blog(LOG_INFO,
		     "[VirtuEdit] YouTube ready: \"%s\" (%.1fs)",
		     info.title.c_str(), info.duration);
		if (onStateChange)
			onStateChange(info);
		return true;
	}

	if (event == "state_change") {
		std::lock_guard<std::mutex> lock(cbMtx);
		if (!acceptsVideoMessage(jsonString(msg, "videoId")))
			return true;
		int state = jsonInt(msg, "state");
		double currentTime = jsonDouble(msg, "youtube_time");
		double duration = jsonDouble(msg, "duration");
		if (!isAllowedPlayerState(state) ||
		    !isFiniteNumber(currentTime) || currentTime < 0.0 ||
		    !isFiniteNumber(duration) || duration < 0.0) {
			blog(LOG_WARNING,
			     "[VirtuEdit] Ignored invalid YouTube state message");
			return true;
		}
		info.state = state;
		info.currentTime = currentTime;
		info.duration = duration;
		if (onStateChange)
			onStateChange(info);
		return true;
	}

	if (event == "player_error") {
		int code = jsonInt(msg, "code");
		blog(LOG_WARNING, "[VirtuEdit] YouTube player error: %d", code);
		std::lock_guard<std::mutex> lock(cbMtx);
		if (!acceptsVideoMessage(jsonString(msg, "videoId")))
			return true;
		if (code < 0 || code > 999)
			code = 0;
		if (onError)
			onError(code);
		return true;
	}

	if (event == "volume_change") {
		int vol = jsonInt(msg, "volume");
		VolumeCallback volumeCb;
		{
			std::lock_guard<std::mutex> lock(cbMtx);
			if (!acceptsVideoMessage(jsonString(msg, "videoId")))
				return true;
			if (vol >= 0) {
				vol = std::min(100, std::max(0, vol));
				volumeCb = onVolumeChange;
			}
		}
		if (volumeCb)
			volumeCb(vol);
		return true;
	}

	if (event == "recording_stopped") {
		const int messageRecordingId = jsonInt(msg, "recording_id");
		const std::string messageProducerId =
			jsonString(msg, "producer_id");
		RecordingStoppedCallback stoppedCb;
		size_t pointCount = 0;
		{
			std::lock_guard<std::mutex> lock(cbMtx);
			if (!acceptsRecordingMessage(messageRecordingId) ||
			    !acceptsProducerMessage(messageProducerId)) {
				blog(LOG_INFO,
				     "[VirtuEdit] Ignored stale recording_stopped message");
				return true;
			}
			const bool wasRecording = recording || stopRequested;
			recording = false;
			stopRequested = false;
			pointCount = syncPoints.size();
			if (wasRecording)
				stoppedCb = onRecordingStopped;
		}
		blog(LOG_INFO,
		     "[VirtuEdit] obs-player confirmed recording stopped (%zu points)",
		     pointCount);
		if (stoppedCb)
			stoppedCb();
		return true;
	}

	if (event == "recording_started") {
		const int messageRecordingId = jsonInt(msg, "recording_id");
		std::lock_guard<std::mutex> lock(cbMtx);
		if (!acceptsRecordingMessage(messageRecordingId))
			return true;
		blog(LOG_INFO,
		     "[VirtuEdit] obs-player confirmed recording started");
		return true;
	}

	if (event == "timestamp") {
		const int messageRecordingId = jsonInt(msg, "recording_id");
		const std::string messageProducerId =
			jsonString(msg, "producer_id");
		SyncPoint sp;
		sp.reaction_time = jsonDouble(msg, "reaction_time");
		sp.youtube_time = jsonDouble(msg, "youtube_time");
		sp.relative_youtube_time =
			jsonDouble(msg, "relative_youtube_time");
		sp.event = jsonString(msg, "sync_event");
		sp.youtube_first_play_time =
			jsonDouble(msg, "youtube_first_play_time");

			TimestampCallback timestampCb;
			RecordingStoppedCallback stoppedCb;
			size_t pointCount = 0;
			bool stoppedAfterEnd = false;
			{
			std::lock_guard<std::mutex> lock(cbMtx);
			if (!acceptsVideoMessage(jsonString(msg, "videoId")))
				return true;
			if (!acceptsRecordingMessage(messageRecordingId))
				return true;
			if (!recording)
				return true;
			if (!acceptsProducerMessage(messageProducerId)) {
				blog(LOG_INFO,
				     "[VirtuEdit] Ignored timestamp from inactive producer");
				return true;
			}
			if (syncPoints.size() >= MAX_SYNC_POINTS) {
				blog(LOG_WARNING,
				     "[VirtuEdit] Ignored sync point after max count");
				return true;
			}

			if (!isAllowedSyncEvent(sp.event) ||
			    !isFiniteNumber(sp.reaction_time) ||
			    sp.reaction_time < 0.0 ||
			    !isNullOrNonNegative(sp.youtube_time) ||
			    !isNullOrNonNegative(sp.relative_youtube_time) ||
			    !isNullOrNonNegative(sp.youtube_first_play_time)) {
				blog(LOG_WARNING,
				     "[VirtuEdit] Ignored invalid sync point");
				return true;
			}
			if (!syncPoints.empty() &&
			    sp.reaction_time + 0.000001 <
				    syncPoints.back().reaction_time) {
				blog(LOG_WARNING,
				     "[VirtuEdit] Ignored out-of-order sync point");
				return true;
			}

			syncPoints.push_back(sp);
			pointCount = syncPoints.size();
			timestampCb = onTimestamp;
				if (sp.event == "end" && stopRequested) {
					recording = false;
					stopRequested = false;
					stoppedAfterEnd = true;
					stoppedCb = onRecordingStopped;
				}
			}

		blog(LOG_INFO,
		     "[VirtuEdit] sync_point #%zu: %s rt=%.3f yt=%.3f",
		     pointCount, sp.event.c_str(), sp.reaction_time,
		     sp.youtube_time);

		if (timestampCb)
			timestampCb(sp);
			if (stoppedAfterEnd) {
				blog(LOG_INFO,
				     "[VirtuEdit] Timestamp recording stopped (%zu points)",
				     pointCount);
				if (stoppedCb)
					stoppedCb();
			}
			return true;
		}

	return true;
}

void YouTubeController::injectJsCommand(const std::string &json)
{
	if (!browserSource)
		return;

	proc_handler_t *ph = obs_source_get_proc_handler(browserSource);
	if (!ph)
		return;

	// obs-browser's "javascript_event" dispatches a CustomEvent
	// with name=eventName and detail=jsonString
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "eventName", "vtCommand");
	calldata_set_string(&cd, "jsonString", json.c_str());
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
}
