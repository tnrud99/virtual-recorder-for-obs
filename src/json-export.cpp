#include "json-export.hpp"
#include <cmath>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <QByteArray>
#include <QFile>
#include <QString>

static std::string escapeJson(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += c;
		}
	}
	return out;
}

static std::string nowISO8601()
{
	time_t now = time(nullptr);
	struct tm t;
#ifdef _WIN32
	gmtime_s(&t, &now);
#else
	gmtime_r(&now, &t);
#endif
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &t);
	return buf;
}

static std::string doubleOrNull(double v)
{
	if (v < 0)
		return "null";
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(3) << v;
	return ss.str();
}

static bool isValidSyncTime(double v)
{
	return std::isfinite(v) && v >= 0.0;
}

static double round3Seconds(double v)
{
	return std::round(v * 1000.0) / 1000.0;
}

static std::vector<SyncPoint>
normalizeSyncPointsForExport(const std::vector<SyncPoint> &syncPoints)
{
	static constexpr double kMaxClockDriftToNormalizeSec = 0.150;

	std::vector<SyncPoint> normalized = syncPoints;
	for (size_t i = 0; i + 1 < normalized.size(); i++) {
		SyncPoint &play = normalized[i];
		SyncPoint &pause = normalized[i + 1];
		if (play.event != "youtube_play" ||
		    pause.event != "youtube_pause") {
			continue;
		}
		if (!isValidSyncTime(play.reaction_time) ||
		    !isValidSyncTime(pause.reaction_time) ||
		    !isValidSyncTime(play.youtube_time) ||
		    !isValidSyncTime(pause.youtube_time)) {
			continue;
		}

		const double playReactionTime =
			round3Seconds(play.reaction_time);
		const double pauseReactionTime =
			round3Seconds(pause.reaction_time);
		const double playYoutubeTime =
			round3Seconds(play.youtube_time);
		const double pauseYoutubeTime =
			round3Seconds(pause.youtube_time);
		const double reactionDelta =
			pauseReactionTime - playReactionTime;
		const double youtubeDelta = pauseYoutubeTime - playYoutubeTime;
		if (reactionDelta <= 0.0 || youtubeDelta < 0.0)
			continue;

		const double drift = youtubeDelta - reactionDelta;
		if (std::abs(drift) > kMaxClockDriftToNormalizeSec)
			continue;

		const double normalizedYoutubeTime =
			round3Seconds(playYoutubeTime + reactionDelta);
		pause.youtube_time = normalizedYoutubeTime;
		pause.relative_youtube_time = normalizedYoutubeTime;
	}
	return normalized;
}

static std::string overlayPresetFallback(
	const SceneManager::OverlayPos &overlay)
{
	if (!std::isfinite(overlay.x) || !std::isfinite(overlay.y) ||
	    !std::isfinite(overlay.w) || !std::isfinite(overlay.h) ||
	    overlay.w <= 0.0f || overlay.h <= 0.0f) {
		return "top-right";
	}

	const double centerX = overlay.x + overlay.w / 2.0;
	const double centerY = overlay.y + overlay.h / 2.0;
	const double left = std::abs(static_cast<double>(overlay.x));
	const double right =
		std::abs(1.0 - static_cast<double>(overlay.x + overlay.w));
	const double top = std::abs(static_cast<double>(overlay.y));
	const double bottom =
		std::abs(1.0 - static_cast<double>(overlay.y + overlay.h));
	const double centerHorizontal = std::abs(centerX - 0.5);
	const double centerVertical = std::abs(centerY - 0.5);

	struct Candidate {
		const char *name;
		double score;
	};

	const Candidate candidates[] = {
		{"top-left", left + top},
		{"top-center", centerHorizontal + top},
		{"top-right", right + top},
		{"middle-left", left + centerVertical},
		{"middle-right", right + centerVertical},
		{"bottom-left", left + bottom},
		{"bottom-center", centerHorizontal + bottom},
		{"bottom-right", right + bottom},
	};

	const Candidate *best = &candidates[0];
	for (const Candidate &candidate : candidates) {
		if (candidate.score < best->score)
			best = &candidate;
	}
	return best->name;
}

std::string JsonExport::buildJson(const std::string &videoId,
				  const std::string &title, double duration,
				  const std::string &reactionVideo,
				  const std::string &compositeVideo,
				  const std::vector<SyncPoint> &syncPoints,
				  const SceneManager::OverlayPos &overlay,
				  int youtubeVolume)
{
	std::ostringstream j;
	j << std::fixed << std::setprecision(3);
	const std::vector<SyncPoint> normalizedSyncPoints =
		normalizeSyncPointsForExport(syncPoints);
	const std::string presetFallback = overlayPresetFallback(overlay);

	j << "{\n";
	j << "  \"schema_version\": 2,\n";
	j << "  \"youtube_video_id\": \"" << escapeJson(videoId) << "\",\n";
	j << "  \"youtube_title\": \"" << escapeJson(title) << "\",\n";
	j << "  \"reaction_video\": \"" << escapeJson(reactionVideo) << "\",\n";
	j << "  \"composite_video\": \"" << escapeJson(compositeVideo) << "\",\n";
	j << "  \"created_at\": \"" << nowISO8601() << "\",\n";
	j << "  \"source\": \"obs-plugin\",\n";
	j << "  \"layout\": {\n";
	j << "    \"mode\": \"overlay\",\n";
	j << "    \"view_mode\": \"overlay\",\n";
	j << "    \"overlay_target\": \"original\",\n";
	j << "    \"overlay_position\": \"custom\",\n";
	j << "    \"overlay_preset_fallback\": \"" << presetFallback << "\",\n";
	j << "    \"overlay_size\": " << overlay.w * 100.0f << ",\n";
	j << "    \"overlay_x\": " << overlay.x << ",\n";
	j << "    \"overlay_y\": " << overlay.y << ",\n";
	j << "    \"overlay_width\": " << overlay.w << ",\n";
	j << "    \"overlay_height\": " << overlay.h << ",\n";
	j << "    \"youtube_volume\": " << youtubeVolume << ",\n";
	j << "    \"hide_overlay\": false,\n";
	j << "    \"youtube_captions\": false,\n";
	j << "    \"youtube_caption_language\": \"auto\"\n";
	j << "  },\n";

	j << "  \"sync_points\": [\n";
	for (size_t i = 0; i < normalizedSyncPoints.size(); i++) {
		const auto &sp = normalizedSyncPoints[i];
		j << "    {\n";
		j << "      \"reaction_time\": " << doubleOrNull(sp.reaction_time) << ",\n";
		j << "      \"youtube_time\": " << doubleOrNull(sp.youtube_time) << ",\n";
		j << "      \"relative_youtube_time\": " << doubleOrNull(sp.relative_youtube_time) << ",\n";
		j << "      \"event\": \"" << escapeJson(sp.event) << "\",\n";
		j << "      \"youtube_first_play_time\": " << doubleOrNull(sp.youtube_first_play_time) << "\n";
		j << "    }";
		if (i + 1 < normalizedSyncPoints.size())
			j << ",";
		j << "\n";
	}
	j << "  ]\n";
	j << "}\n";

	return j.str();
}

static std::string replaceExtension(const std::string &path,
				    const std::string &suffix,
				    const std::string &newExt)
{
	auto dot = path.rfind('.');
	if (dot == std::string::npos)
		return path + suffix + newExt;
	return path.substr(0, dot) + suffix + newExt;
}

std::string JsonExport::saveNextToRecording(const std::string &recordingPath,
					    const std::string &jsonContent)
{
	std::string jsonPath =
		replaceExtension(recordingPath, "_sync", ".json");
	return saveToPath(jsonPath, jsonContent);
}

std::string JsonExport::saveToPath(const std::string &jsonPath,
				   const std::string &jsonContent)
{
	QFile file(QString::fromUtf8(jsonPath.c_str(),
				     static_cast<int>(jsonPath.size())));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate |
		       QIODevice::Text)) {
		blog(LOG_ERROR, "[VirtuEdit] Failed to write JSON: %s",
		     jsonPath.c_str());
		return "";
	}

	const QByteArray bytes = QByteArray::fromStdString(jsonContent);
	if (file.write(bytes) != bytes.size()) {
		blog(LOG_ERROR, "[VirtuEdit] Failed to write full JSON: %s",
		     jsonPath.c_str());
		return "";
	}
	file.close();

	blog(LOG_INFO, "[VirtuEdit] JSON saved: %s", jsonPath.c_str());
	return jsonPath;
}

std::string
JsonExport::getReactVideoPath(const std::string &recordingPath)
{
	auto dot = recordingPath.rfind('.');
	std::string ext = (dot != std::string::npos)
				  ? recordingPath.substr(dot)
				  : ".mkv";
	return replaceExtension(recordingPath, "_reaction", ext);
}
