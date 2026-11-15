#include "dock-widget.hpp"
#include "youtube-controller.hpp"
#include "scene-manager.hpp"
#include "json-export.hpp"
#include "ffmpeg-extract.hpp"
#include "obs-utils.hpp"
#include "recorder-update.hpp"
#include "youtube-url.hpp"
#include <obs.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QApplication>
#include <QPalette>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QIcon>
#include <QListWidgetItem>
#include <QPixmap>
#include <QSizePolicy>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

// Free space (in GB) on the volume containing `dir`; -1 on failure.
static double freeDiskGB(const std::string &dir)
{
#ifdef _WIN32
	ULARGE_INTEGER freeBytes;
	if (GetDiskFreeSpaceExA(dir.c_str(), &freeBytes, nullptr, nullptr))
		return (double)freeBytes.QuadPart / 1e9;
	return -1;
#else
	struct statvfs st;
	if (statvfs(dir.c_str(), &st) == 0)
		return (double)st.f_bavail * (double)st.f_frsize / 1e9;
	return -1;
#endif
}

static QString videoSizeText(const char *label, const VirtuEditCanvasSize &size)
{
	return QString::fromUtf8(label) + QStringLiteral(" ") +
	       QString::number(size.width) + QStringLiteral("x") +
	       QString::number(size.height);
}

static std::string qPathToStd(const QString &path)
{
	QByteArray bytes = path.toUtf8();
	return std::string(bytes.constData(), bytes.size());
}

static QString stdPathToQString(const std::string &path)
{
	return QString::fromUtf8(path.c_str(), static_cast<int>(path.size()));
}

static bool utf8PathExists(const std::string &path)
{
	return !path.empty() && QFileInfo::exists(stdPathToQString(path));
}

static bool fileLooksSaved(const std::string &path)
{
	if (path.empty())
		return false;
	QFileInfo info(stdPathToQString(path));
	return info.exists() && info.isFile() && info.size() > 0;
}

static std::string existingPath(const std::string &preferred,
				const std::string &fallback)
{
	if (fileLooksSaved(preferred))
		return preferred;
	if (fileLooksSaved(fallback))
		return fallback;
	return "";
}

static QSize preferredPlayerControlsWindowSize(QWidget *anchor)
{
	QSize size(960, 540);
	QScreen *screen = anchor ? anchor->screen() : QApplication::primaryScreen();
	if (!screen)
		return size;

	const QRect available = screen->availableGeometry();
	const int maxWidth = available.width() - 80;
	const int maxHeight = available.height() - 120;
	if (maxWidth <= 400 || maxHeight <= 400)
		return size;

	if (size.width() > maxWidth) {
		size.setWidth(maxWidth);
		size.setHeight((size.width() * 9 + 8) / 16);
	}
	if (size.height() > maxHeight) {
		size.setHeight(maxHeight);
		size.setWidth((size.height() * 16 + 4) / 9);
	}
	if (size.height() <= 400) {
		size.setWidth(720);
		size.setHeight(405);
	}
	return size;
}

static bool initializePlayerControlsWindowSize(QWidget *anchor)
{
	config_t *config = obs_frontend_get_app_config();
	if (!config)
		return false;

	const QSize size = preferredPlayerControlsWindowSize(anchor);
	config_set_int(config, "InteractionWindow", "cx", size.width());
	config_set_int(config, "InteractionWindow", "cy", size.height());
	return true;
}

#ifdef _WIN32
static bool enableInteractionWindowMaximizeButton(const QString &sourceName)
{
	if (sourceName.isEmpty())
		return false;

	for (QWidget *widget : QApplication::topLevelWidgets()) {
		if (!widget || widget->windowTitle().isEmpty())
			continue;
		if (!widget->windowTitle().contains(sourceName))
			continue;

		const Qt::WindowFlags oldFlags = widget->windowFlags();
		const Qt::WindowFlags newFlags =
			oldFlags | Qt::WindowMaximizeButtonHint;
		if (oldFlags == newFlags)
			return true;

		const bool wasVisible = widget->isVisible();
		widget->setWindowFlags(newFlags);
		if (wasVisible)
			widget->show();

		blog(LOG_INFO,
		     "[VirtuEdit] Enabled maximize button for YouTube Controls interaction window");
		return true;
	}

	return false;
}
#endif

static QString trimSlugSeparator(QString slug)
{
	while (slug.endsWith('-'))
		slug.chop(1);
	while (slug.startsWith('-'))
		slug.remove(0, 1);
	return slug;
}

static QString recordingTitleSlug(const std::string &title,
				  const std::string &fallback)
{
	QString input = QString::fromUtf8(title.c_str()).trimmed();
	if (input.isEmpty())
		input = QString::fromUtf8(fallback.c_str()).trimmed();

	QString slug;
	bool lastWasSeparator = false;
	const int maxChars = 48;
	for (int i = 0; i < input.size() && slug.size() < maxChars; i++) {
		const QChar ch = input.at(i);
		if (ch.isLetterOrNumber()) {
			slug.append(ch.toLower());
			lastWasSeparator = false;
		} else if (!lastWasSeparator && !slug.isEmpty()) {
			slug.append('-');
			lastWasSeparator = true;
		}
	}

	slug = trimSlugSeparator(slug);
	if (slug.isEmpty())
		slug = QStringLiteral("youtube-video");
	return slug;
}

static QString uniqueFilePath(const QString &desiredPath)
{
	if (!QFileInfo::exists(desiredPath))
		return desiredPath;

	QFileInfo info(desiredPath);
	const QString dir = info.absolutePath();
	const QString baseName = info.completeBaseName();
	const QString suffix = info.suffix();
	const QString ext =
		suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix;

	for (int i = 2; i < 1000; i++) {
		QString candidate = dir + QStringLiteral("/") + baseName +
				    QStringLiteral("_") + QString::number(i) + ext;
		if (!QFileInfo::exists(candidate))
			return candidate;
	}

	return desiredPath;
}

static std::string fileContainerFormat(const std::string &path)
{
	if (path.empty())
		return "";
	const QString suffix =
		QFileInfo(stdPathToQString(path)).suffix().toLower();
	const QByteArray bytes = suffix.toUtf8();
	return std::string(bytes.constData(), bytes.size());
}

static std::string normalizeReactionOutputFormat(const QString &value)
{
	const QString lower = value.trimmed().toLower();
	if (lower.contains(QStringLiteral("mp4")))
		return "mp4";
	if (lower.contains(QStringLiteral("mov")))
		return "mov";
	if (lower.contains(QStringLiteral("mkv")))
		return "mkv";
	return "";
}

static std::string normalizeReactionOutputFormat(const std::string &value)
{
	return normalizeReactionOutputFormat(
		QString::fromUtf8(value.c_str(), static_cast<int>(value.size())));
}

static std::string obsRecordingOutputFormat()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return "mp4";

	const char *mode = config_get_string(config, "Output", "Mode");
	const bool advanced = mode && std::strcmp(mode, "Advanced") == 0;
	const char *section = advanced ? "AdvOut" : "SimpleOutput";
	const char *raw = config_get_string(config, section, "RecFormat2");
	if (!raw || !*raw)
		raw = config_get_string(config, section, "RecFormat");

	const std::string format = raw ? normalizeReactionOutputFormat(
						 std::string(raw))
				       : "";
	return format.empty() ? "mp4" : format;
}

static QString reactionFormatDisplayName(const std::string &format)
{
	const std::string normalized = normalizeReactionOutputFormat(format);
	if (normalized == "mov")
		return QStringLiteral("MOV");
	if (normalized == "mkv")
		return QStringLiteral("MKV");
	return QStringLiteral("MP4");
}

static std::string reactionMkvPathForBase(const std::string &base)
{
	return base.empty() ? std::string() : base + "_reaction.mkv";
}

static std::string reactionOutputPathForBase(const std::string &base,
					     const std::string &format)
{
	const std::string normalized = normalizeReactionOutputFormat(format);
	return base.empty() ? std::string()
			    : base + "_reaction." +
				      (normalized.empty() ? "mp4" : normalized);
}

static QString pathToQString(const std::filesystem::path &path)
{
	const std::string value = path.u8string();
	return QString::fromUtf8(value.c_str(), (int)value.size());
}

static QString createRecordingSessionBase(const std::string &dir,
					  const QString &dateCode,
					  const QString &slug)
{
	namespace fs = std::filesystem;

	std::error_code ec;
	const fs::path outputDir = fs::u8path(dir);
	const fs::path sessionsDir = outputDir / "Virtual Recorder";
	fs::create_directories(sessionsDir, ec);
	if (ec)
		return QString();

	const QString rootName = dateCode + QStringLiteral("_") + slug;

	for (int i = 1; i < 1000; i++) {
		const QString sessionName =
			i == 1 ? rootName : rootName + QStringLiteral("_") +
						  QString::number(i);

		const fs::path sessionPath =
			sessionsDir / fs::u8path(qPathToStd(sessionName));
		ec.clear();
		if (fs::exists(sessionPath, ec))
			continue;
		if (ec)
			return QString();

		ec.clear();
		if (!fs::create_directory(sessionPath, ec)) {
			if (!ec)
				continue;
			return QString();
		}

		return pathToQString(sessionPath /
				     fs::u8path(qPathToStd(sessionName)));
	}

	return QString();
}

static std::string renameCompositeToRecordingSet(const std::string &path,
						 const std::string &base)
{
	if (path.empty() || base.empty())
		return path;

	const QString srcPath = stdPathToQString(path);
	QFileInfo srcInfo(srcPath);
	if (!srcInfo.exists())
		return path;

	QString suffix = srcInfo.suffix();
	QString ext = suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix;
	QString desiredPath =
		stdPathToQString(base) + QStringLiteral("_combined") + ext;
	if (QFileInfo(desiredPath).absoluteFilePath() ==
	    srcInfo.absoluteFilePath())
		return path;

	desiredPath = uniqueFilePath(desiredPath);
	if (QFile::rename(srcPath, desiredPath)) {
		blog(LOG_INFO, "[VirtuEdit] Composite renamed: %s",
		     qPathToStd(desiredPath).c_str());
		return qPathToStd(desiredPath);
	}

	blog(LOG_WARNING, "[VirtuEdit] Composite rename failed: %s",
	     path.c_str());
	return path;
}

// --- OBS-native style helpers ---

static const char *STYLE_BRAND_TITLE =
	"font-weight: 700; font-size: 14px; color: #F3F5F8;";

#ifdef _WIN32
// Use fonts already supplied by Windows. Windows 11 prefers Segoe UI
// Variable Text, Windows 10 falls back to Segoe UI, and Malgun Gothic covers
// Korean glyphs. No font files are bundled or redistributed by the plugin.
static const char *STYLE_WINDOWS_FONT =
	"font-family: 'Segoe UI Variable Text', 'Segoe UI', 'Malgun Gothic', sans-serif;";
#endif

static const char *STYLE_SUBTLE =
	"color: #AEB4C0; font-size: 12px;";

static const char *STYLE_FIELD_LABEL =
	"color: #E7EAF0; font-size: 12px; font-weight: 700;";

static const char *STYLE_CARD_TITLE =
	"color: #E7EAF0; font-size: 13px; font-weight: 700;";

static const char *STYLE_COMPLETE_SECTION_TITLE =
	"color: #F3F5F8; font-size: 14px; font-weight: 700;";

static const char *STYLE_BODY_LABEL =
	"color: #DDE1E8; font-size: 12px;";

static const char *STYLE_STEP_ACTION =
	"color: #F3F5F8; font-size: 13px; font-weight: 700;";

static const char *STYLE_STEP_ACTION_ACCENT =
	"color: #F0C5C2; font-size: 13px; font-weight: 700;";

static const char *STYLE_STEP_NOTE =
	"color: #AEB4C0; font-size: 12px;";

static const char *STYLE_SECTION_PANEL =
	"QFrame#SectionPanel {"
	"  background-color: #20242D;"
	"  border: 1px solid #303645;"
	"  border-radius: 5px;"
	"}";

static const char *STYLE_SECTION_PANEL_ACTIVE =
	"QFrame#SectionPanel {"
	"  background-color: #29232B;"
	"  border: 1px solid #B65A5A;"
	"  border-radius: 5px;"
	"}";

static const char *STYLE_SECTION_PANEL_COMPLETE =
	"QFrame#SectionPanel {"
	"  background-color: #20292A;"
	"  border: 1px solid #4F9561;"
	"  border-radius: 5px;"
	"}";

static const char *STYLE_SOURCE_PICKER =
	"QFrame#SourcePicker {"
	"  background-color: #20242D;"
	"  border: 1px solid #303645;"
	"  border-radius: 5px;"
	"}";

static const char *STYLE_SOURCE_PICKER_ACTIVE =
	"QFrame#SourcePicker {"
	"  background-color: #29232B;"
	"  border: 1px solid #B65A5A;"
	"  border-radius: 5px;"
	"}";

static const char *STYLE_SOURCE_PICKER_COMPLETE =
	"QFrame#SourcePicker {"
	"  background-color: #20292A;"
	"  border: 1px solid #4F9561;"
	"  border-radius: 5px;"
	"}";

static const char *STYLE_ACTIVE_HINT =
	"color: #F0C5C2; font-size: 12px;";

static const char *STYLE_COMPLETE_TITLE =
	"font-weight: 700; font-size: 18px; color: #72D783;";

static const char *STYLE_COMPLETE_WARNING_TITLE =
	"font-weight: 700; font-size: 18px; color: #D6BD6E;";

static const char *STYLE_COMPLETE_INFO =
	"font-weight: 700; font-size: 14px; color: #F3F5F8;";

static const char *STYLE_COMPLETE_STATUS_SUCCESS =
	"color: #72D783; font-size: 13px; font-weight: 700;";

static const char *STYLE_COMPLETE_STATUS_ERROR =
	"color: #FF8A83; font-size: 13px; font-weight: 700;";

static const char *STYLE_COMPLETE_STATUS_WARNING =
	"color: #D6BD6E; font-size: 13px; font-weight: 700;";

static const char *STYLE_COMPLETE_STATUS_SUBTLE =
	"color: #AEB4C0; font-size: 13px; font-weight: 700;";

static const char *STYLE_SUCCESS =
	"color: #72D783; font-size: 12px;";

static const char *STYLE_SUCCESS_STATUS =
	"color: #72D783; font-size: 11px; font-weight: 700;";

static const char *STYLE_ERROR =
	"color: #FF8A83; font-size: 12px;";

static const char *STYLE_ERROR_TITLE =
	"color: #FF8A83; font-size: 12px; font-weight: 700;";

static const char *STYLE_WARNING =
	"color: #D6BD6E; font-size: 12px;";

static const char *STYLE_REC_BADGE =
	"color: #FF8A83; font-weight: 700; font-size: 13px;";

static const char *STYLE_INPUT =
	"QLineEdit {"
	"  background-color: #343947;"
	"  color: #F3F5F8;"
	"  border: 1px solid #4B5262;"
	"  border-radius: 4px;"
	"  min-height: 32px;"
	"  padding: 0 10px;"
	"  selection-background-color: #65708A;"
	"}"
	"QLineEdit:focus { border-color: #7A8296; }"
	"QLineEdit:read-only { color: #C5CBD5; background-color: #2F3441; }";

static const char *STYLE_COMBOBOX =
	"QComboBox {"
	"  background-color: #343947;"
	"  color: #F3F5F8;"
	"  border: 1px solid #4B5262;"
	"  border-radius: 4px;"
	"  min-height: 30px;"
	"  padding: 0 8px;"
	"}"
	"QComboBox:focus { border-color: #7A8296; }"
	"QComboBox QAbstractItemView {"
	"  background-color: #2F3441;"
	"  color: #F3F5F8;"
	"  selection-background-color: #4A5366;"
	"  border: 1px solid #4B5262;"
	"}";

static const char *STYLE_AUDIO_SOURCE_LIST =
	"QListWidget {"
	"  background-color: #343947;"
	"  color: #F3F5F8;"
	"  border: 1px solid #4B5262;"
	"  border-radius: 4px;"
	"  padding: 3px;"
	"}"
	"QListWidget:disabled { color: #858B96; background-color: #303541; }"
	"QListWidget::item { min-height: 26px; padding: 2px 4px; }"
	"QListWidget::item:selected { background-color: transparent; }";

static const char *STYLE_SLIDER =
	"QSlider::groove:horizontal {"
	"  height: 4px;"
	"  background: #3A4050;"
	"  border-radius: 2px;"
	"}"
	"QSlider::sub-page:horizontal {"
	"  background: #68758F;"
	"  border-radius: 2px;"
	"}"
	"QSlider::handle:horizontal {"
	"  background: #F2F4F8;"
	"  border: 0;"
	"  width: 16px;"
	"  height: 16px;"
	"  margin: -6px 0;"
	"  border-radius: 8px;"
	"}";

static const char *STYLE_PROMPT_BOX =
	"QWidget#DevicePrompt {"
	"  background-color: transparent;"
	"  border: none;"
	"}"
	"QWidget#DevicePrompt QLabel {"
	"  background: transparent;"
	"  border: none;"
	"}";

static const char *STYLE_PRIMARY_BTN =
	"QPushButton {"
	"  background-color: #3E4656;"
	"  color: #F3F5F8;"
	"  border: 1px solid #5B6476;"
	"  border-radius: 4px;"
	"  min-height: 32px;"
	"  max-height: 32px;"
	"  padding: 0 12px;"
	"  font-weight: 600;"
	"}"
	"QPushButton:hover { background-color: #485162; border-color: #6A7386; }"
	"QPushButton:pressed { background-color: #363D4C; }"
	"QPushButton:disabled {"
	"  background-color: #303541;"
	"  border-color: #424957;"
	"  color: #858B96;"
	"}";

static const char *STYLE_PRIMARY_BTN_ACTIVE =
	"QPushButton {"
	"  background-color: #44323A;"
	"  color: #F3F5F8;"
	"  border: 1px solid #B65A5A;"
	"  border-radius: 4px;"
	"  min-height: 32px;"
	"  max-height: 32px;"
	"  padding: 0 12px;"
	"  font-weight: 700;"
	"}"
	"QPushButton:hover { background-color: #4D3942; border-color: #D47575; }"
	"QPushButton:pressed { background-color: #3B2B33; }"
	"QPushButton:disabled {"
	"  background-color: #303541;"
	"  border-color: #424957;"
	"  color: #858B96;"
	"}";

static const char *STYLE_DANGER_BTN =
	"QPushButton {"
	"  background-color: #4B3038;"
	"  color: #FFE7E4;"
	"  border: 1px solid #D47575;"
	"  border-radius: 5px;"
	"  min-height: 42px;"
	"  max-height: 42px;"
	"  padding: 0 14px;"
	"  font-weight: 700;"
	"  font-size: 13px;"
	"}"
	"QPushButton:hover { background-color: #5A3841; border-color: #EA8585; }"
	"QPushButton:pressed { background-color: #3F2930; }"
	"QPushButton:disabled {"
	"  background-color: #303541;"
	"  border-color: #424957;"
	"  color: #858B96;"
	"}";

static const char *STYLE_FLAT_BTN =
	"QPushButton {"
	"  background-color: #343947;"
	"  color: #E7EAF0;"
	"  border: 1px solid #454C5A;"
	"  border-radius: 4px;"
	"  min-height: 30px;"
	"  max-height: 30px;"
	"  padding: 0 10px;"
	"}"
	"QPushButton:hover { background-color: #3D4350; border-color: #596171; }"
	"QPushButton:pressed { background-color: #2E3340; }"
	"QPushButton:disabled {"
	"  background-color: #303541;"
	"  border-color: #3F4653;"
	"  color: #858B96;"
	"}";

static const char *STYLE_SMALL_BTN =
	"QPushButton {"
	"  background-color: #343947;"
	"  color: #E7EAF0;"
	"  border: 1px solid #454C5A;"
	"  border-radius: 4px;"
	"  min-height: 28px;"
	"  max-height: 28px;"
	"  padding: 0 9px;"
	"  font-size: 12px;"
	"}"
	"QPushButton:hover { background-color: #3D4350; border-color: #596171; }"
	"QPushButton:disabled {"
	"  background-color: #303541;"
	"  border-color: #3F4653;"
	"  color: #858B96;"
	"}";

static const char *STYLE_SMALL_BTN_ACTIVE =
	"QPushButton {"
	"  background-color: #3D3038;"
	"  color: #F3F5F8;"
	"  border: 1px solid #B65A5A;"
	"  border-radius: 4px;"
	"  min-height: 28px;"
	"  max-height: 28px;"
	"  padding: 0 9px;"
	"  font-size: 12px;"
	"}"
	"QPushButton:hover { background-color: #4A3842; border-color: #D47575; }"
	"QPushButton:disabled {"
	"  background-color: #303541;"
	"  border-color: #3F4653;"
	"  color: #858B96;"
	"}";

static const char *STYLE_BACK_BTN =
	"QPushButton {"
	"  background-color: transparent;"
	"  color: #AEB4C0;"
	"  border: 1px solid #3F4653;"
	"  border-radius: 4px;"
	"  min-height: 26px;"
	"  max-height: 26px;"
	"  padding: 0 8px;"
	"  font-size: 11px;"
	"}"
	"QPushButton:hover { background-color: #2D333E; border-color: #596171; color: #E7EAF0; }"
	"QPushButton:disabled { border-color: #343A46; color: #737986; }";

static const char *STYLE_DISCLOSURE_BTN =
	"QToolButton {"
	"  background-color: transparent;"
	"  color: #8B929F;"
	"  border: none;"
	"  min-height: 20px;"
	"  max-height: 20px;"
	"  padding: 0 2px;"
	"  font-size: 11px;"
	"}"
	"QToolButton:hover { color: #B8BFCC; }"
	"QToolButton:disabled { color: #6F7580; }";

static const char *STYLE_SKIP_BTN =
	"QPushButton {"
	"  background-color: transparent;"
	"  color: #C4CAD4;"
	"  border: 1px solid #4A5160;"
	"  border-radius: 4px;"
	"  min-height: 24px;"
	"  max-height: 24px;"
	"  padding: 0 8px;"
	"  font-size: 11px;"
	"}"
	"QPushButton:hover {"
	"  background-color: #2D333E;"
	"  border-color: #6A7386;"
	"  color: #E7EAF0;"
	"}"
	"QPushButton:disabled {"
	"  background-color: transparent;"
	"  border-color: #343A46;"
	"  color: #737986;"
	"}";

static QFrame *makeSeparator()
{
	QFrame *line = new QFrame();
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Plain);
	line->setFixedHeight(1);
	line->setStyleSheet("color: #343947;");
	return line;
}

static QFrame *makeSectionPanel(QVBoxLayout **contentLayout)
{
	QFrame *panel = new QFrame();
	panel->setObjectName(QStringLiteral("SectionPanel"));
	panel->setStyleSheet(STYLE_SECTION_PANEL);
	panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	QVBoxLayout *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 8, 10, 8);
	layout->setSpacing(6);
	if (contentLayout)
		*contentLayout = layout;
	return panel;
}

static void setSectionPanelState(QWidget *panel, bool active, bool complete)
{
	if (!panel)
		return;
	if (active)
		panel->setStyleSheet(STYLE_SECTION_PANEL_ACTIVE);
	else if (complete)
		panel->setStyleSheet(STYLE_SECTION_PANEL_COMPLETE);
	else
		panel->setStyleSheet(STYLE_SECTION_PANEL);
}

static void setSourcePickerState(QWidget *picker, bool active, bool complete)
{
	if (!picker)
		return;
	if (active)
		picker->setStyleSheet(STYLE_SOURCE_PICKER_ACTIVE);
	else if (complete)
		picker->setStyleSheet(STYLE_SOURCE_PICKER_COMPLETE);
	else
		picker->setStyleSheet(STYLE_SOURCE_PICKER);
}

static void tunePageLayout(QVBoxLayout *layout)
{
	layout->setContentsMargins(14, 12, 14, 12);
	layout->setSpacing(6);
}

static QWidget *makeScrollPage(QVBoxLayout **contentLayout)
{
	QScrollArea *scroll = new QScrollArea();
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setStyleSheet(
		"QScrollArea { border: none; background: transparent; }"
		"QScrollArea > QWidget > QWidget { background: transparent; }");

	QWidget *content = new QWidget();
	content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	QVBoxLayout *layout = new QVBoxLayout(content);
	tunePageLayout(layout);
	scroll->setWidget(content);

	if (contentLayout)
		*contentLayout = layout;
	return scroll;
}

static void tuneLineEdit(QLineEdit *lineEdit)
{
	lineEdit->setStyleSheet(STYLE_INPUT);
	lineEdit->setMinimumHeight(32);
	lineEdit->setMinimumWidth(0);
}

static void tuneSlider(QSlider *slider)
{
	slider->setStyleSheet(STYLE_SLIDER);
}

static QIcon makeExternalLinkIcon()
{
	static const char *const externalLinkXpm[] = {
		"16 16 2 1",
		". c None",
		"# c #E7EAF0",
		"................",
		".........#####..",
		".............#..",
		"............##..",
		"...........#.#..",
		"...######.#..#..",
		"...#....##...#..",
		"...#....#.......",
		"...#....#.......",
		"...#....#.......",
		"...#....#.......",
		"...######.......",
		"................",
		"................",
		"................",
		"................",
	};
	return QIcon(QPixmap(externalLinkXpm));
}

static const char *APP_TITLE = "Virtual Recorder";
static const char *PLAYER_BUTTON_LABEL = "Open YouTube Controls";
static const int READY_STEP_VIDEO = 2;
static const int READY_STEP_LAYOUT = 3;
static const int READY_STEP_REACTION = 4;
static const int READY_STEP_RECORD = 5;

static QString reactionEncoderSummaryText(
	const ReactionEncoderSettings &settings)
{
	QString video = QStringLiteral("x264 ");
	if (settings.rateControl == "CBR") {
		video += QStringLiteral("CBR ") +
			 QString::number(settings.videoBitrate) +
			 QStringLiteral(" kbps");
	} else {
		video += QStringLiteral("CRF ") +
			 QString::number(settings.crf);
	}
	video += QStringLiteral(", AAC ") +
		 QString::number(settings.audioBitrate) +
		 QStringLiteral(" kbps");
	return video;
}

static void populateIntegerCombo(QComboBox *combo, std::vector<int> values,
				 int currentValue, const QString &prefix,
				 const QString &suffix)
{
	if (!combo)
		return;

	if (currentValue > 0 &&
	    std::find(values.begin(), values.end(), currentValue) ==
		    values.end())
		values.push_back(currentValue);
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());

	combo->clear();
	int selectedIndex = -1;
	for (int value : values) {
		combo->addItem(prefix + QString::number(value) + suffix, value);
		if (value == currentValue)
			selectedIndex = combo->count() - 1;
	}
	if (selectedIndex >= 0)
		combo->setCurrentIndex(selectedIndex);
}

static int currentIntegerComboValue(QComboBox *combo)
{
	if (!combo)
		return 0;
	return combo->currentData().toInt();
}

static void setComboCurrentData(QComboBox *combo, const QString &value)
{
	if (!combo)
		return;

	for (int i = 0; i < combo->count(); i++) {
		if (combo->itemData(i).toString() == value) {
			combo->setCurrentIndex(i);
			return;
		}
	}
}

static QString syncTimingCountText(int count)
{
	return QStringLiteral("Sync timing: ") + QString::number(count);
}

static QString syncFileSavedStatusText(int count)
{
	return QStringLiteral("Saved (") + QString::number(count) +
	       QStringLiteral(")");
}

static QString completeStatusSavedText()
{
	return QStringLiteral("Saved");
}

static QString completeStatusSavedAsMkvText()
{
	return QStringLiteral("Saved as MKV");
}

static QString completeStatusSavedForFormatText(const std::string &format)
{
	const std::string normalized = normalizeReactionOutputFormat(format);
	if (normalized.empty() || normalized == "mp4")
		return completeStatusSavedText();
	return QStringLiteral("Saved as ") + reactionFormatDisplayName(normalized);
}

static QString completeStatusFailedText()
{
	return QStringLiteral("Failed");
}

static QString completeStatusNotSavedText()
{
	return QStringLiteral("Not saved");
}

static QString completeStatusNoSyncTimingText()
{
	return QStringLiteral("Saved, no sync");
}

static QString completeStatusFinalizingText()
{
	return QStringLiteral("Finalizing...");
}

static QString reactFinalizingTooltipText()
{
	return QStringLiteral(
		"Waiting for the Reaction Video to finish writing. This can take longer for large recordings.");
}

static QString syncFinalizingTooltipText()
{
	return QStringLiteral(
		"Waiting for YouTube Controls to confirm recording stopped before saving the Sync File.");
}

static QString compositeFinalizingTooltipText()
{
	return QStringLiteral(
		"Waiting for OBS to finish writing the Combined Video using your current OBS recording settings.");
}

static QString reactionSavedAsMkvTooltipText()
{
	return QStringLiteral(
		"Format conversion did not finish.\nThe MKV Reaction Video was saved and can be used.");
}

static QString reactionFailedTooltipText()
{
	return QStringLiteral(
		"Reaction Video was not saved.\nCheck:\n- recording folder\n- disk space\n- file permissions\n- OBS encoder\n- selected audio");
}

static QString syncBlockedByReactionFailedTooltipText()
{
	return QStringLiteral(
		"Sync File was not saved because Reaction Video finalization failed.\nCheck:\n- recording folder\n- disk space\n- file permissions\n- selected audio sources");
}

static QString syncNoTimingTooltipText()
{
	return QStringLiteral(
		"Sync File was saved, but no play/pause/seek timing was recorded.\nUse YouTube Controls during recording.");
}

static QString syncWriteFailedTooltipText()
{
	return QStringLiteral(
		"Sync File was not saved.\nCheck:\n- recording folder\n- disk space\n- file permissions\n- security software blocking writes");
}

static QString combinedFailedTooltipText()
{
	return QStringLiteral(
		"Combined Video was not saved.\nCheck:\n- OBS recording settings\n- recording path\n- disk space\n- file permissions\n- security software");
}

static QString openControlsForSyncText()
{
	return QStringLiteral(
		"Open YouTube Controls to save sync timing while you play, pause, and seek.");
}

static QString playerControlsButtonText(bool opened)
{
	return opened ? QStringLiteral("Reopen YouTube Controls") :
			QStringLiteral("Open YouTube Controls");
}

static QString playerControlsButtonTooltip(bool opened)
{
	return opened ?
		       QStringLiteral(
			       "Bring YouTube Controls back if the window is hidden. Use it for play, pause, and seek during recording.") :
		       QStringLiteral(
			       "Open YouTube Controls for play, pause, and seek. This is required to save sync timing.");
}

static QString recordingControlsHintText()
{
	return QStringLiteral(
		"Use YouTube Controls during recording. Play, pause, and seek there to save sync timing.");
}

static QString speakerAudioBleedNoteText()
{
	return QStringLiteral(
		"If YouTube plays through speakers, it can get picked up by your microphone. Headphones can help.");
}

static QString youtubeLinkWorkflowText()
{
	return QStringLiteral(
		"Paste a YouTube link here. Then use YouTube Controls to play, pause, and seek.");
}

static QString findLinkTooltipText()
{
	return QStringLiteral(
		"Open YouTube to copy a link, then paste it here.");
}

static bool isExistingSourceChoice(const QString &value)
{
	return value.startsWith(QStringLiteral("source:"));
}

static std::string sourceUuidFromChoice(const QString &value)
{
	if (!isExistingSourceChoice(value))
		return "";
	QByteArray bytes = value.mid(7).toUtf8();
	return std::string(bytes.constData(), bytes.size());
}

static std::string sourceUuidFromStoredId(const std::string &id)
{
	static const std::string prefix = "source:";
	if (id.rfind(prefix, 0) == 0)
		return id.substr(prefix.size());
	return id;
}

static bool sourceUuidIsGlobalAudioSource(const std::string &uuid)
{
	if (uuid.empty())
		return false;

	for (uint32_t channel = 0; channel < 64; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		if (!source)
			continue;

		const char *sourceUuid = obs_source_get_uuid(source);
		const bool matches = sourceUuid && uuid == sourceUuid;
		obs_source_release(source);
		if (matches)
			return true;
	}
	return false;
}

static QString audioSourceDisplayLabel(const DeviceInfo &source)
{
	const QString name = QString::fromUtf8(source.name.c_str());
	if (sourceUuidIsGlobalAudioSource(source.id))
		return QStringLiteral("Global - ") + name;
	return QStringLiteral("OBS Source - ") + name;
}

static bool audioChoiceMayIncludeSystemAudio(const QString &label,
					     const QString &value)
{
	if (value.isEmpty())
		return false;

	const QString combined = label.toLower() + QStringLiteral(" ") +
				 value.toLower();
	const QStringList riskyTerms = {
		QStringLiteral("desktop audio"),
		QStringLiteral("system audio"),
		QStringLiteral("application audio"),
		QStringLiteral("screen capture"),
		QStringLiteral("output capture"),
		QStringLiteral("loopback"),
		QStringLiteral("blackhole"),
		QStringLiteral("soundflower"),
		QStringLiteral("vb-cable"),
		QStringLiteral("virtual cable"),
		QStringLiteral("aggregate device"),
		QStringLiteral("multi-output"),
		QStringLiteral("monitor"),
		QStringLiteral("mixed"),
		QStringLiteral("데스크탑 오디오"),
		QStringLiteral("시스템 오디오"),
		QStringLiteral("응용 프로그램 오디오"),
		QStringLiteral("애플리케이션 오디오"),
		QStringLiteral("화면 캡쳐"),
		QStringLiteral("오디오 출력"),
		QStringLiteral("출력 캡쳐"),
		QStringLiteral("루프백"),
		QStringLiteral("모니터")
	};
	for (const QString &term : riskyTerms) {
		if (combined.contains(term))
			return true;
	}
	return false;
}

static QStringList checkedAudioSourceValues(QListWidget *list)
{
	QStringList values;
	if (!list)
		return values;
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *item = list->item(i);
		if (!item || item->checkState() != Qt::Checked)
			continue;
		const QString value = item->data(Qt::UserRole).toString();
		if (!value.isEmpty())
			values.append(value);
	}
	return values;
}

static bool audioSourceListHasSelection(QListWidget *list)
{
	return !checkedAudioSourceValues(list).isEmpty();
}

static std::vector<std::string> checkedAudioSourceUuids(QListWidget *list)
{
	std::vector<std::string> uuids;
	const QStringList values = checkedAudioSourceValues(list);
	for (const QString &value : values) {
		std::string uuid = sourceUuidFromChoice(value);
		if (!uuid.empty())
			uuids.push_back(uuid);
	}
	return uuids;
}

static bool selectedAudioSourcesMayIncludeSystemAudio(QListWidget *list)
{
	if (!list)
		return false;
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *item = list->item(i);
		if (!item || item->checkState() != Qt::Checked)
			continue;
		if (audioChoiceMayIncludeSystemAudio(
			    item->text(), item->data(Qt::UserRole).toString()))
			return true;
	}
	return false;
}

static QString audioSourceNameForValue(
	const std::vector<DeviceInfo> &availableSources, const QString &value)
{
	if (value.isEmpty())
		return QString();
		for (const DeviceInfo &source : availableSources) {
			if (source.id.empty())
				continue;
			const QString sourceValue = QStringLiteral("source:") +
						    QString::fromUtf8(source.id.c_str());
			if (sourceValue == value)
				return audioSourceDisplayLabel(source);
		}
		return QString();
	}

static QString selectedAudioSourceNames(
	const std::vector<DeviceInfo> &availableSources,
	const std::vector<std::string> &ids)
{
	if (ids.empty())
		return QStringLiteral("Not selected");

	QStringList names;
	for (const std::string &id : ids) {
		QString value = QString::fromUtf8(id.c_str());
		QString name = audioSourceNameForValue(availableSources, value);
		if (name.isEmpty() && id.rfind("source:", 0) == 0)
			name = QStringLiteral("OBS source unavailable");
		else if (name.isEmpty())
			name = QStringLiteral("Audio source unavailable");
		names.append(name);
	}

	if (names.size() <= 2)
		return names.join(QStringLiteral(", "));
	return names.at(0) + QStringLiteral(", ") + names.at(1) +
	       QStringLiteral(", +") + QString::number(names.size() - 2);
}

static bool audioSourceIdsAvailable(
	const std::vector<std::string> &ids,
	const std::vector<DeviceInfo> &availableSources)
{
	if (ids.empty())
		return false;

	QStringList available;
	for (const DeviceInfo &source : availableSources) {
		if (!source.id.empty())
			available.append(QStringLiteral("source:") +
					 QString::fromUtf8(source.id.c_str()));
	}

	for (const std::string &id : ids) {
		if (!available.contains(QString::fromUtf8(id.c_str())))
			return false;
	}
	return true;
}

static bool selectedAudioSourceIdsMayIncludeSystemAudio(
	const std::vector<DeviceInfo> &availableSources,
	const std::vector<std::string> &ids)
{
	for (const std::string &id : ids) {
		const QString value = QString::fromUtf8(id.c_str());
		const QString name =
			audioSourceNameForValue(availableSources, value);
		if (audioChoiceMayIncludeSystemAudio(name, value))
			return true;
	}
	return false;
}

static QString reactionAudioWarning()
{
	return QStringLiteral(
		"Audio warning: this selection may include YouTube or system audio. "
		"Use it only if your OBS routing keeps YouTube out of the Reaction Video.");
}

static QString playerControlsBlockingMessage(
	const SceneManager::PlayerControlsState &state)
{
	if (state.count == 0)
		return QStringLiteral(
			"Add the Virtual Recorder YouTube Controls box to this OBS scene before recording.");
	if (state.count > 1)
		return QStringLiteral(
			"Fix duplicate Virtual Recorder YouTube Controls boxes before recording.");
	if (state.visibleCount == 0)
		return QStringLiteral(
			"Show the Virtual Recorder YouTube Controls box in this OBS scene before recording.");
	return QString();
}

static QString playerControlsStateDetailText(
	const SceneManager::PlayerControlsState &state)
{
	if (state.count == 0)
		return QStringLiteral(
			"No Virtual Recorder YouTube Controls box is in this OBS scene.");
	if (state.count > 1)
		return QStringLiteral("Found ") + QString::number(state.count) +
		       QStringLiteral(" YouTube Controls boxes in this OBS scene. ") +
		       QString::number(state.visibleCount) +
		       QStringLiteral(" visible.");
	if (state.visibleCount == 0)
		return QStringLiteral(
			"The YouTube Controls box is in this OBS scene but hidden.");
	return QStringLiteral(
		"Exactly one YouTube Controls box is visible in this OBS scene.");
}

static QString playerControlsStatusText(
	const SceneManager::PlayerControlsState &state)
{
	if (state.count == 0)
		return QStringLiteral("YouTube Controls: Not in this scene");
	if (state.count > 1)
		return QStringLiteral("YouTube Controls: ") +
		       QString::number(state.count) +
		       QStringLiteral(" boxes found");
	if (state.visibleCount == 0)
		return QStringLiteral("YouTube Controls: Hidden");
	return QStringLiteral("YouTube Controls: Ready");
}

static QString playerControlsFixButtonText(
	const SceneManager::PlayerControlsState &state)
{
	if (state.count == 0)
		return QStringLiteral("Add YouTube Controls");
	if (state.count > 1)
		return QStringLiteral("Fix YouTube Controls");
	if (state.visibleCount == 0)
		return QStringLiteral("Show YouTube Controls");
	return QStringLiteral("Fix YouTube Controls");
}

static QString playerControlsRepairHintText(
	const SceneManager::PlayerControlsState &state)
{
	if (state.count == 0)
		return QStringLiteral(
			"Adds Virtual Recorder's YouTube Controls box to the current OBS scene. Other OBS sources stay unchanged.");
	if (state.count > 1)
		return QStringLiteral(
			"Removes duplicate Virtual Recorder YouTube Controls boxes and keeps one visible box. Other OBS sources stay unchanged.");
	if (state.visibleCount == 0)
		return QStringLiteral(
			"Shows the existing Virtual Recorder YouTube Controls box in the current OBS scene. Other OBS sources stay unchanged.");
	return QString();
}

static QString sceneChangedGuidanceText(
	const SceneManager::PlayerControlsState &state)
{
	if (state.count == 0)
		return QStringLiteral(
			"Add YouTube Controls to this scene.");
	if (state.count > 1)
		return QStringLiteral(
			"Fix duplicate YouTube Controls boxes.");
	if (state.visibleCount == 0)
		return QStringLiteral(
			"Show YouTube Controls in this scene.");
	return QStringLiteral(
		"Check YouTube Controls position.");
}

// --- Time formatting ---

static QString formatTime(double sec)
{
	if (sec < 0)
		sec = 0;
	int m = static_cast<int>(sec) / 60;
	int s = static_cast<int>(sec) % 60;
	char buf[16];
	snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
	return QString(buf);
}

static bool hasMeaningfulSyncTiming(const std::vector<SyncPoint> &points)
{
	for (const SyncPoint &point : points) {
		if (point.event != "start" && point.event != "end")
			return true;
	}
	return false;
}

static QString formatRecTime(qint64 ms)
{
	int total = static_cast<int>(ms / 1000);
	int h = total / 3600;
	int m = (total % 3600) / 60;
	int s = total % 60;
	char buf[16];
	snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
	return QString(buf);
}

// --- Constructor ---

VirtuEditDock::VirtuEditDock(QWidget *parent, YouTubeController *controller,
			     SceneManager *mgr)
	: QWidget(parent),
	  yt(controller),
	  sceneMgr(mgr)
{
#ifdef _WIN32
	// A widget-level style sheet cascades to the dock's child controls. Their
	// existing styles still own size, weight, color, and spacing.
	setStyleSheet(STYLE_WINDOWS_FONT);
#endif

	// Restore the persisted relative volume before building pages
	relativeVolume = sceneMgr->getSavedVolume();

	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);
	setMinimumWidth(300);

	pages = new QStackedWidget(this);

	setupSetupPage();
	setupReadyPage();
	setupRecordingPage();
	setupCompletePage();
	updateResponsiveLayout();

	pages->addWidget(setupPage);
	pages->addWidget(readyPage);
	pages->addWidget(recordingPage);
	pages->addWidget(completePage);

	if (sceneMgr->isSetupDone())
		showPostSetupPage();
	else
		pages->setCurrentWidget(setupPage);

	mainLayout->addWidget(pages);

	releaseNotice = new QLabel(this);
	releaseNotice->setTextFormat(Qt::RichText);
	releaseNotice->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
					       Qt::LinksAccessibleByKeyboard);
	releaseNotice->setWordWrap(true);
	releaseNotice->setMargin(8);
	releaseNotice->setStyleSheet(STYLE_SUBTLE);
	releaseNotice->hide();
	mainLayout->addWidget(releaseNotice);
	connect(pages, &QStackedWidget::currentChanged, this,
		&VirtuEditDock::updateReleaseNoticeVisibility);
	connect(releaseNotice, &QLabel::linkActivated, this, [this]() {
		if (releaseNotice->isVisible())
			QDesktopServices::openUrl(releaseDownloadUrl);
	});
	QTimer::singleShot(1500, this, [this]() {
		checkRecorderUpdate(this, QString::fromUtf8(obs_get_version_string()),
			[this](const RecorderUpdate &update) {
				releaseDownloadUrl = update.downloadUrl;
				releaseNotice->setText(
					QStringLiteral("Update %1 available · <a href=\"download\">Download</a>")
						.arg(update.version));
				updateReleaseNoticeVisibility();
			});
	});

	recTimer = new QTimer(this);
	connect(recTimer, &QTimer::timeout, this,
		&VirtuEditDock::updateRecordingTimer);

	// Single-shot watchdog: if a loaded video never reports ready, surface
	// an error instead of hanging on "Loading...".
	loadTimer = new QTimer(this);
	loadTimer->setSingleShot(true);
	connect(loadTimer, &QTimer::timeout, this,
		&VirtuEditDock::onLoadTimeout);

	yt->setOnStateChange([this](const YouTubeInfo &) {
		QMetaObject::invokeMethod(this,
					  &VirtuEditDock::onYouTubeStateChanged,
					  Qt::QueuedConnection);
	});

	yt->setOnTimestamp([this](const SyncPoint &) {
		QMetaObject::invokeMethod(this,
					  &VirtuEditDock::onTimestampReceived,
					  Qt::QueuedConnection);
	});

	yt->setOnRecordingStopped([this]() {
		QMetaObject::invokeMethod(
			this, [this]() { saveSyncJsonIfOutputReady(); },
			Qt::QueuedConnection);
	});

	yt->setOnVolumeChange([this](int v) {
		QMetaObject::invokeMethod(
			this, [this, v]() { onPlayerVolumeChanged(v); },
			Qt::QueuedConnection);
	});

	yt->setOnError([this](int code) {
		QMetaObject::invokeMethod(
			this, [this, code]() { onVideoError(code); },
			Qt::QueuedConnection);
	});
}

VirtuEditDock::~VirtuEditDock()
{
	if (yt) {
		yt->setOnStateChange({});
		yt->setOnTimestamp({});
		yt->setOnRecordingStopped({});
		yt->setOnVolumeChange({});
		yt->setOnError({});
	}
}

void VirtuEditDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	updateResponsiveLayout();
}

void VirtuEditDock::updateReleaseNoticeVisibility()
{
	if (!releaseNotice)
		return;

	const auto *page = pages->currentWidget();
	releaseNotice->setVisible(!releaseDownloadUrl.isEmpty() &&
				  (page == setupPage || page == readyPage) &&
				  !isCompletionProcessing() &&
				  !obs_frontend_recording_active());
}

// --- Setup Page ---

void VirtuEditDock::setupSetupPage()
{
	QVBoxLayout *layout = nullptr;
	setupPage = makeScrollPage(&layout);

	QWidget *setupHeader = new QWidget();
	setupHeader->setMaximumWidth(460);
	QHBoxLayout *setupHeaderRow = new QHBoxLayout(setupHeader);
	setupHeaderRow->setContentsMargins(0, 0, 0, 0);
	setupHeaderRow->setSpacing(8);

	QLabel *title = new QLabel("Step 1 - OBS Scene & Audio");
	title->setStyleSheet(STYLE_BRAND_TITLE);
	title->setWordWrap(true);
	setupHeaderRow->addWidget(title, 1);

	layout->addWidget(setupHeader);

	layout->addSpacing(6);

	QLabel *setupIntro = new QLabel(
		"Choose the voice audio for the Reaction Video. Avoid sources that include YouTube or desktop audio.");
	setupIntro->setStyleSheet(STYLE_BODY_LABEL);
	setupIntro->setWordWrap(true);
	setupIntro->setMaximumWidth(460);
	layout->addWidget(setupIntro);

	layout->addSpacing(6);

	QFrame *sourcePicker = new QFrame();
	sourcePickerWidget = sourcePicker;
	sourcePicker->setObjectName(QStringLiteral("SourcePicker"));
	sourcePicker->setStyleSheet(STYLE_SOURCE_PICKER);
	sourcePicker->setMaximumWidth(460);
	sourcePicker->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	QVBoxLayout *sourceLayout = new QVBoxLayout(sourcePicker);
	sourceLayout->setContentsMargins(10, 8, 10, 8);
	sourceLayout->setSpacing(6);

	QLabel *camLabel = new QLabel("Current scene");
	camLabel->setStyleSheet(STYLE_FIELD_LABEL);
	camLabel->setWordWrap(true);
	sourceLayout->addWidget(camLabel);
	currentSceneLabel = new QLabel("");
	currentSceneLabel->setStyleSheet(STYLE_BODY_LABEL);
	currentSceneLabel->setWordWrap(true);
	sourceLayout->addWidget(currentSceneLabel);
	sourceLayout->addSpacing(4);

	QLabel *micLabel = new QLabel("Audio to save");
	micLabel->setStyleSheet(STYLE_FIELD_LABEL);
	micLabel->setWordWrap(true);
	sourceLayout->addWidget(micLabel);
	audioSourceList = new QListWidget();
	audioSourceList->setStyleSheet(STYLE_AUDIO_SOURCE_LIST);
	audioSourceList->setMinimumHeight(84);
	audioSourceList->setMaximumHeight(132);
	audioSourceList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	audioSourceList->setTextElideMode(Qt::ElideRight);
	audioSourceList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(audioSourceList, &QListWidget::itemChanged, this,
		[this](QListWidgetItem *) { updateSetupAudioWarning(); });
	sourceLayout->addWidget(audioSourceList);

	layout->addWidget(sourcePicker);

	QWidget *setupHintRow = new QWidget();
	setupHintRow->setMaximumWidth(460);
	QHBoxLayout *setupHintLayout = new QHBoxLayout(setupHintRow);
	setupHintLayout->setContentsMargins(0, 0, 0, 0);
	setupHintLayout->setSpacing(8);

	setupSourceHintLabel = new QLabel("");
	setupSourceHintLabel->setStyleSheet(STYLE_SUBTLE);
	setupSourceHintLabel->setWordWrap(true);
	setupSourceHintLabel->setMaximumWidth(460);
	setupHintLayout->addWidget(setupSourceHintLabel, 1);

	rescanSourcesBtn = new QToolButton();
	rescanSourcesBtn->setText("Refresh list");
	rescanSourcesBtn->setStyleSheet(STYLE_DISCLOSURE_BTN);
	rescanSourcesBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
	rescanSourcesBtn->setAutoRaise(true);
	rescanSourcesBtn->setToolTip(
		"Refresh after changing OBS scenes or audio sources.");
	connect(rescanSourcesBtn, &QToolButton::clicked, this,
		&VirtuEditDock::populateDeviceLists);
	setupHintLayout->addWidget(rescanSourcesBtn, 0, Qt::AlignTop);
	layout->addWidget(setupHintRow);

	layout->addSpacing(6);

	setupStatusLabel = new QLabel("");
	setupStatusLabel->setWordWrap(true);
	setupStatusLabel->setStyleSheet(STYLE_ERROR);
	setupStatusLabel->setVisible(false);
	layout->addWidget(setupStatusLabel);

	layout->addSpacing(8);

	QWidget *setupActions = new QWidget();
	setupActions->setMaximumWidth(460);
	QHBoxLayout *setupButtonRow = new QHBoxLayout(setupActions);
	setupButtonRow->setContentsMargins(0, 0, 0, 0);
	setupButtonRow->setSpacing(8);

	setupButtonRow->addStretch();

	setupCompleteBtn = new QPushButton("Add YouTube Controls");
	setupCompleteBtn->setStyleSheet(STYLE_SMALL_BTN_ACTIVE);
	setupCompleteBtn->setMinimumWidth(148);
	connect(setupCompleteBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onSetupComplete);
	setupButtonRow->addWidget(setupCompleteBtn);
	layout->addWidget(setupActions);

	layout->addStretch();

	populateDeviceLists();
}

void VirtuEditDock::populateDeviceLists()
{
	DeviceInfo currentScene = SceneManager::currentFrontendScene();
	if (currentSceneLabel) {
		currentSceneLabel->setText(
			currentScene.name.empty() ?
				QStringLiteral("No OBS scene selected") :
				QString::fromUtf8(currentScene.name.c_str()));
		currentSceneLabel->setStyleSheet(currentScene.id.empty() ?
							 STYLE_ERROR :
							 STYLE_BODY_LABEL);
	}

	QStringList previouslyChecked = checkedAudioSourceValues(audioSourceList);
	QStringList savedChecked;
	if (sceneMgr) {
		for (const std::string &id :
		     sceneMgr->getCurrentReactionAudioSourceIds()) {
			if (!id.empty())
				savedChecked.append(QString::fromUtf8(id.c_str()));
		}
	}
	const QStringList checkedTargets =
		!savedChecked.isEmpty() ? savedChecked : previouslyChecked;

	if (!audioSourceList)
		return;
	audioSourceList->blockSignals(true);
	audioSourceList->clear();

	auto existingAudio = SceneManager::listCurrentSceneAudioSources();
	int audioCount = 0;
	bool checkedAny = false;
	for (auto &d : existingAudio) {
		if (d.id.empty())
			continue;

		const bool globalAudio = sourceUuidIsGlobalAudioSource(d.id);
		const QString label = audioSourceDisplayLabel(d);
		const QString value = QStringLiteral("source:") +
				      QString::fromUtf8(d.id.c_str());
		QListWidgetItem *item = new QListWidgetItem(label);
		item->setToolTip(
			globalAudio ?
				QStringLiteral("Global OBS audio source from the Audio Mixer.") :
				QStringLiteral("OBS audio source."));
		item->setData(Qt::UserRole, value);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		bool shouldCheck = checkedTargets.contains(value);
		item->setCheckState(shouldCheck ? Qt::Checked : Qt::Unchecked);
		audioSourceList->addItem(item);
		checkedAny = checkedAny || shouldCheck;
		audioCount++;
	}

	if (audioCount == 0) {
		QListWidgetItem *item = new QListWidgetItem(
			QStringLiteral("No OBS audio sources found"));
		item->setData(Qt::UserRole, QString());
		item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
		item->setCheckState(Qt::Unchecked);
		audioSourceList->addItem(item);
	}
	audioSourceList->blockSignals(false);

	if (!checkedAny && audioCount > 0 && !checkedTargets.isEmpty())
		blog(LOG_WARNING,
		     "[VirtuEdit] Saved reaction audio sources are unavailable");

	updateSetupAudioWarning();
}

void VirtuEditDock::updateSetupAudioWarning()
{
	// Guide the user if a source/device class is missing (often a macOS
	// permission issue rather than a truly absent device). If sources are
	// present, warn about mixed/virtual audio choices without blocking setup.
	QString message;
	const char *statusStyle = STYLE_ERROR;
	DeviceInfo currentScene = SceneManager::currentFrontendScene();
	bool hasSceneChoice = !currentScene.id.empty();
	bool hasAudioChoice = audioSourceListHasSelection(audioSourceList);
	if (currentSceneLabel) {
		currentSceneLabel->setText(
			currentScene.name.empty() ?
				QStringLiteral("No OBS scene selected") :
				QString::fromUtf8(currentScene.name.c_str()));
		currentSceneLabel->setStyleSheet(hasSceneChoice ? STYLE_BODY_LABEL :
								  STYLE_ERROR);
	}
	if (!hasSceneChoice && !hasAudioChoice)
		message = QStringLiteral(
			"No OBS scene or audio selected. Create an OBS "
			"scene and add your audio sources in OBS, then refresh.");
	else if (!hasSceneChoice)
		message = QStringLiteral(
			"No OBS scene detected. Select or create an OBS scene, then refresh.");
	else if (!hasAudioChoice)
		message = QStringLiteral(
			"Choose the OBS audio source that contains your voice or reaction. This audio is saved into the Reaction Video.");

	bool ok = hasSceneChoice && hasAudioChoice;
	if (ok && selectedAudioSourcesMayIncludeSystemAudio(audioSourceList)) {
		message = reactionAudioWarning();
		statusStyle = STYLE_WARNING;
	}
	const std::string setupSceneUuid =
		sceneMgr ? sourceUuidFromStoredId(sceneMgr->getCurrentSceneId()) :
			   std::string();
	const bool setupSceneMatchesCurrent =
		hasSceneChoice && setupSceneUuid == currentScene.id;
	SceneManager::PlayerControlsState controlsState;
	if (ok && setupSceneMatchesCurrent && sceneMgr && sceneMgr->getScene())
		controlsState = sceneMgr->getPlayerControlsState();
	const bool controlsAlreadyReady =
		ok && setupSceneMatchesCurrent && controlsState.exactlyOneVisible();
	const bool controlsNeedRepair =
		ok && setupSceneMatchesCurrent && !controlsAlreadyReady;

	QString hint;
	if (ok && controlsAlreadyReady) {
		hint = QString();
	} else if (controlsNeedRepair) {
		hint = playerControlsRepairHintText(controlsState);
		if (hint.isEmpty())
			hint = QStringLiteral("Fix YouTube Controls in this OBS scene.");
	} else if (ok) {
		hint = QStringLiteral(
			"Add YouTube Controls to this scene. The selected audio becomes the Reaction Video audio.");
	} else {
		hint = QStringLiteral(
			"Choose the OBS audio source that contains your voice or reaction.");
	}

	setSourcePickerState(sourcePickerWidget, !ok, ok);
	if (setupSourceHintLabel) {
		setupSourceHintLabel->setText(hint);
		setupSourceHintLabel->setVisible(!hint.isEmpty());
	}
	setupStatusLabel->setText(message);
	setupStatusLabel->setStyleSheet(statusStyle);
	setupStatusLabel->setVisible(!message.isEmpty());
	setupCompleteBtn->setEnabled(ok);
	setupCompleteBtn->setStyleSheet(ok ? STYLE_SMALL_BTN_ACTIVE :
					     STYLE_SMALL_BTN);
	setupCompleteBtn->setText(
		controlsAlreadyReady ?
			QStringLiteral("Save Setup") :
			(controlsNeedRepair ? playerControlsFixButtonText(controlsState) :
					      QStringLiteral("Add YouTube Controls")));
	QString setupTip =
		QStringLiteral("Available after an OBS scene and audio are selected.");
	if (ok && controlsAlreadyReady) {
		setupTip = QStringLiteral(
			"Save the current OBS scene and selected audio.");
	} else if (ok && controlsNeedRepair) {
		setupTip = playerControlsRepairHintText(controlsState);
		if (setupTip.isEmpty())
			setupTip = QStringLiteral(
				"Fix YouTube Controls in this OBS scene.");
	} else if (ok) {
		setupTip = QStringLiteral(
			"Add YouTube Controls to the current OBS scene and save the selected audio.");
	}
	setupCompleteBtn->setToolTip(setupTip);
	if (rescanSourcesBtn) {
		rescanSourcesBtn->setStyleSheet(STYLE_DISCLOSURE_BTN);
		rescanSourcesBtn->setToolTip(
			ok ? "Refresh after changing OBS scenes or audio sources."
			   : "Refresh after creating an OBS scene or adding OBS audio sources.");
	}
}

void VirtuEditDock::onSetupComplete()
{
	if (isRecordingFlowBusy() || waitingForComposite)
		return;

	DeviceInfo currentScene = SceneManager::currentFrontendScene();
	std::vector<std::string> audioSourceUuids =
		checkedAudioSourceUuids(audioSourceList);
	if (currentScene.id.empty()) {
		blog(LOG_WARNING,
		     "[VirtuEdit] No current OBS scene selected for setup");
		setupStatusLabel->setText(
			"No OBS scene selected. Select or create an OBS scene, then refresh.");
		setupStatusLabel->setVisible(true);
		setSourcePickerState(sourcePickerWidget, true, false);
		updateReadiness();
		return;
	}

	if (audioSourceUuids.empty()) {
		blog(LOG_WARNING,
		     "[VirtuEdit] No reaction audio sources selected for setup");
		setupStatusLabel->setText(
			"Choose at least one audio source for the Reaction Video.");
		setupStatusLabel->setVisible(true);
		setSourcePickerState(sourcePickerWidget, true, false);
		updateReadiness();
		return;
	}

	if (!sceneMgr->selectReactionScene(currentScene.id)) {
		blog(LOG_ERROR, "[VirtuEdit] Reaction scene selection failed");
		setupStatusLabel->setText(
			"Couldn't set up this OBS scene. Check OBS scenes and try again.");
		setupStatusLabel->setVisible(true);
		return;
	}

	if (!sceneMgr->setReactionAudioSources(audioSourceUuids)) {
		setupStatusLabel->setText(
			"Couldn't set up the selected audio. Check the selected OBS "
			"audio sources, then try again.");
		setupStatusLabel->setVisible(true);
		return;
	}

	if (!yt->getBrowserSource())
		yt->createBrowserSource(loadedVideoId);
	obs_source_t *browserSrc = yt->getBrowserSource();
	SceneManager::PlayerControlsState controlsState =
		sceneMgr->getPlayerControlsState();
	const bool controlsReady = controlsState.exactlyOneVisible();
	const bool controlsOk =
		browserSrc && (controlsReady ? sceneMgr->addBrowserSource(browserSrc) :
					       sceneMgr->resetPlayerControls(browserSrc));
	if (!controlsOk) {
		setupStatusLabel->setText(
			"Couldn't create YouTube Controls. Restart "
			"OBS and try setup again.");
		setupStatusLabel->setVisible(true);
		return;
	}

	sceneMgr->activateScene();

	blog(LOG_INFO,
	     "[VirtuEdit] Setup complete: scene=%s reaction_audio_sources=%zu",
	     currentScene.id.c_str(), audioSourceUuids.size());

	readyWorkflowStep = READY_STEP_VIDEO;
	showPostSetupPage();
}

void VirtuEditDock::showPostSetupPage()
{
	if (!sceneMgr->isSetupDone()) {
		pages->setCurrentWidget(setupPage);
		updateReadiness();
		return;
	}

	if (sceneMgr->getScene() &&
	    !sceneMgr->getPlayerControlsState().exactlyOneVisible()) {
		populateDeviceLists();
		pages->setCurrentWidget(setupPage);
		return;
	}

	if (readyWorkflowStep < READY_STEP_VIDEO ||
	    readyWorkflowStep > READY_STEP_RECORD)
		readyWorkflowStep = READY_STEP_VIDEO;
	pages->setCurrentWidget(readyPage);
	updateReadiness();
}

void VirtuEditDock::onBackStep()
{
	if (isRecordingFlowBusy() || waitingForComposite || videoLoading) {
		updateReadiness();
		return;
	}

	if (readyWorkflowStep == READY_STEP_RECORD) {
		readyWorkflowStep = READY_STEP_REACTION;
		videoInfoLabel->setText(
			"Check the Reaction Video settings before recording.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		updateReadiness();
		return;
	}

	if (readyWorkflowStep == READY_STEP_REACTION) {
		readyWorkflowStep = READY_STEP_LAYOUT;
		videoInfoLabel->setText(
			"Check the YouTube Controls position in OBS Scene Preview.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		updateReadiness();
		return;
	}

	if (readyWorkflowStep == READY_STEP_LAYOUT) {
		readyWorkflowStep = READY_STEP_VIDEO;
		editingVideoUrl = !videoLoaded;
		if (videoLoaded) {
			videoInfoLabel->setText(currentVideoSummary());
			videoInfoLabel->setStyleSheet(STYLE_SUCCESS_STATUS);
		} else {
			videoInfoLabel->setText(
				"Paste a YouTube link here, then press Load.");
			videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		}
		updateReadiness();
		if (editingVideoUrl && urlInput) {
			urlInput->setFocus();
			urlInput->selectAll();
		}
		return;
	}

	onOpenSettings();
}

void VirtuEditDock::onNextStep()
{
	if (isRecordingFlowBusy() || waitingForComposite || videoLoading) {
		updateReadiness();
		return;
	}

	std::vector<std::string> currentAudioIds =
		sceneMgr ? sceneMgr->getCurrentReactionAudioSourceIds()
			 : std::vector<std::string>();
	const bool sceneReady = sceneMgr && sceneMgr->getScene();
	const bool audioReady = sceneMgr && audioSourceIdsAvailable(
					       currentAudioIds,
					       SceneManager::listCurrentSceneAudioSources());
	const bool devicesReady = sceneReady && audioReady;
	const bool canKeepCurrentVideo = canKeepCurrentLoadedVideo();

	if (readyWorkflowStep == READY_STEP_VIDEO) {
		if (canKeepCurrentVideo) {
			onCancelVideoChange();
		}
		if (devicesReady && videoLoaded && !editingVideoUrl)
			readyWorkflowStep = READY_STEP_LAYOUT;
		updateReadiness();
		return;
	}

	if (readyWorkflowStep == READY_STEP_LAYOUT) {
		readyWorkflowStep = READY_STEP_REACTION;
		videoInfoLabel->setText(
			"Check the Reaction Video settings before recording.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		updateReadiness();
		return;
	}

	if (readyWorkflowStep == READY_STEP_REACTION) {
		readyWorkflowStep = READY_STEP_RECORD;
		videoInfoLabel->setText(
			playerControlsOpened ?
				QStringLiteral("YouTube Controls are open. Start recording when ready.") :
				openControlsForSyncText());
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		updateReadiness();
		return;
	}

	updateReadiness();
}

void VirtuEditDock::onSkipToRecord()
{
	if (isRecordingFlowBusy() || waitingForComposite || videoLoading) {
		updateReadiness();
		return;
	}

	std::vector<std::string> currentAudioIds =
		sceneMgr ? sceneMgr->getCurrentReactionAudioSourceIds()
			 : std::vector<std::string>();
	const bool sceneReady = sceneMgr && sceneMgr->getScene();
	const bool audioReady = sceneMgr && audioSourceIdsAvailable(
					       currentAudioIds,
					       SceneManager::listCurrentSceneAudioSources());
	const bool controlsReady =
		sceneReady && sceneMgr->getPlayerControlsState().exactlyOneVisible();
	const bool videoReady = videoLoaded && !editingVideoUrl && !videoLoading;
	if (!sceneReady || !audioReady || !controlsReady || !videoReady) {
		updateReadiness();
		return;
	}

	readyWorkflowStep = READY_STEP_RECORD;
	videoInfoLabel->setText(
		playerControlsOpened ?
			QStringLiteral("YouTube Controls are open. Start recording when ready.") :
			openControlsForSyncText());
	videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	updateReadiness();
}

void VirtuEditDock::onOpenSettings()
{
	if (isRecordingFlowBusy() || waitingForComposite) {
		updateReadiness();
		return;
	}

	populateDeviceLists();
	updateSetupAudioWarning();

	pages->setCurrentWidget(setupPage);
}

void VirtuEditDock::onSessionRestored()
{
	QMetaObject::invokeMethod(
		this,
		[this]() {
			const std::string restoredVideoId =
				yt ? yt->getCurrentVideoId() : std::string();
			if (!restoredVideoId.empty() && !videoLoaded) {
				loadedVideoId = restoredVideoId;
				loadedVideoUrl = QStringLiteral("https://youtu.be/") +
						 QString::fromUtf8(
							 restoredVideoId.c_str());
				if (urlInput)
					urlInput->setText(loadedVideoUrl);
				videoLoading = false;
				videoLoaded = true;
				editingVideoUrl = false;
				playerControlsOpened = false;
				readyWorkflowStep = READY_STEP_VIDEO;
				if (videoInfoLabel) {
					videoInfoLabel->setText(
						currentVideoSummary());
					videoInfoLabel->setStyleSheet(
						STYLE_SUCCESS_STATUS);
				}
			}
			if (sceneMgr->isSetupDone())
				showPostSetupPage();
		},
		Qt::QueuedConnection);
}

void VirtuEditDock::onFrontendSceneChanged()
{
	if (restoringRecordingScene)
		return;

	DeviceInfo currentScene = SceneManager::currentFrontendScene();

	if ((recordingStarting || recordingActive || stoppingRecording) &&
	    !recordingSceneUuid.empty()) {
		if (currentScene.id.empty() ||
		    currentScene.id == recordingSceneUuid)
			return;

		restoringRecordingScene = true;
		if (sceneMgr)
			sceneMgr->activateScene();
		QTimer::singleShot(0, this, [this]() {
			restoringRecordingScene = false;
		});

		QString targetScene = recordingSceneName.isEmpty() ?
					      QStringLiteral("the recording scene") :
					      recordingSceneName;
		if (videoInfoLabel) {
			videoInfoLabel->setText(
				QStringLiteral("OBS scene changes are locked while recording. Switched back to ") +
				targetScene + QStringLiteral("."));
			videoInfoLabel->setStyleSheet(STYLE_WARNING);
		}
		blog(LOG_WARNING,
		     "[VirtuEdit] OBS scene change blocked during recording: %s",
		     currentScene.name.c_str());
		return;
	}

	if (isRecordingFlowBusy() || waitingForComposite || videoLoading)
		return;

	if (currentScene.id.empty()) {
		updateReadiness();
		return;
	}

	const std::string savedSceneUuid = sceneMgr ?
						  sourceUuidFromStoredId(
							  sceneMgr->getCurrentSceneId()) :
						  std::string();
	if (savedSceneUuid != currentScene.id) {
		if (sceneMgr && sceneMgr->selectReactionScene(currentScene.id)) {
			playerControlsOpened = false;
			readyWorkflowStep = READY_STEP_VIDEO;
			SceneManager::PlayerControlsState controlsState =
				sceneMgr->getPlayerControlsState();
			if (videoInfoLabel) {
				videoInfoLabel->setText(
					sceneChangedGuidanceText(controlsState));
				videoInfoLabel->setStyleSheet(
					controlsState.exactlyOneVisible()
						? STYLE_SUBTLE
						: STYLE_WARNING);
			}
			blog(LOG_INFO,
			     "[VirtuEdit] Current OBS scene changed: %s",
			     currentScene.name.c_str());
			if (!controlsState.exactlyOneVisible()) {
				populateDeviceLists();
				pages->setCurrentWidget(setupPage);
				return;
			}
		}
	}

	if (pages && pages->currentWidget() == setupPage)
		populateDeviceLists();
	else
		updateReadiness();
}

// --- Ready Page ---

void VirtuEditDock::setupReadyPage()
{
	QVBoxLayout *layout = nullptr;
	readyPage = makeScrollPage(&layout);

	QHBoxLayout *titleRow = new QHBoxLayout();
	titleRow->setSpacing(8);
	readyTitleLabel = new QLabel("New Recording");
	readyTitleLabel->setStyleSheet(STYLE_BRAND_TITLE);
	titleRow->addWidget(readyTitleLabel);
	titleRow->addStretch();
	layout->addLayout(titleRow);

	readyCaptionLabel = new QLabel("");
	readyCaptionLabel->setStyleSheet(STYLE_SUBTLE);
	readyCaptionLabel->setWordWrap(true);
	readyCaptionLabel->setVisible(false);
	layout->addWidget(readyCaptionLabel);

	layout->addSpacing(4);

	QVBoxLayout *videoPanelLayout = nullptr;
	videoStepWidget = makeSectionPanel(&videoPanelLayout);
	QHBoxLayout *videoTitleRow = new QHBoxLayout();
	videoTitleRow->setContentsMargins(0, 0, 0, 0);
	videoTitleRow->setSpacing(8);
	QLabel *videoSectionTitle = new QLabel("Video Link");
	videoSectionTitle->setStyleSheet(STYLE_CARD_TITLE);
	videoTitleRow->addWidget(videoSectionTitle);
	videoTitleRow->addStretch();
	QPushButton *openYouTubeBtn = new QPushButton("Find Link");
	openYouTubeBtn->setStyleSheet(STYLE_SMALL_BTN);
	openYouTubeBtn->setIcon(makeExternalLinkIcon());
	openYouTubeBtn->setToolTip(findLinkTooltipText());
	connect(openYouTubeBtn, &QPushButton::clicked, this, []() {
		FfmpegExtract::openInBrowser("https://www.youtube.com");
	});
	videoTitleRow->addWidget(openYouTubeBtn);
	videoPanelLayout->addLayout(videoTitleRow);

	savedDeviceNoteLabel = new QLabel(
		youtubeLinkWorkflowText());
	savedDeviceNoteLabel->setStyleSheet(STYLE_SUBTLE);
	savedDeviceNoteLabel->setWordWrap(true);
	videoPanelLayout->addWidget(savedDeviceNoteLabel);

	loadedVideoChoiceWidget = new QWidget();
	loadedVideoChoiceWidget->setObjectName(QStringLiteral("DevicePrompt"));
	loadedVideoChoiceWidget->setStyleSheet(STYLE_PROMPT_BOX);
	QVBoxLayout *loadedVideoLayout =
		new QVBoxLayout(loadedVideoChoiceWidget);
	loadedVideoLayout->setContentsMargins(0, 0, 0, 0);
	loadedVideoLayout->setSpacing(6);

	QHBoxLayout *loadedVideoTopRow = new QHBoxLayout();
	loadedVideoTopRow->setContentsMargins(0, 0, 0, 0);
	loadedVideoTopRow->setSpacing(8);
	QLabel *loadedVideoLabel = new QLabel("Loaded video");
	loadedVideoLabel->setStyleSheet(STYLE_FIELD_LABEL);
	loadedVideoTopRow->addWidget(loadedVideoLabel);
	loadedVideoTopRow->addStretch();
	changeVideoBtn = new QPushButton("Change Video");
	changeVideoBtn->setStyleSheet(STYLE_SMALL_BTN);
	changeVideoBtn->setMinimumWidth(112);
	changeVideoBtn->setToolTip("Open the link field to load a different YouTube video.");
	connect(changeVideoBtn, &QPushButton::clicked, this, [this]() {
		if (editingVideoUrl)
			onCancelVideoChange();
		else
			onUrlSubmit();
	});
	loadedVideoTopRow->addWidget(changeVideoBtn);
	loadedVideoLayout->addLayout(loadedVideoTopRow);

	loadedVideoTitleLabel = new QLabel("");
	loadedVideoTitleLabel->setStyleSheet(STYLE_BODY_LABEL);
	loadedVideoTitleLabel->setWordWrap(true);
	loadedVideoLayout->addWidget(loadedVideoTitleLabel);

	loadedVideoMetaLabel = new QLabel("");
	loadedVideoMetaLabel->setStyleSheet(STYLE_SUBTLE);
	loadedVideoMetaLabel->setWordWrap(true);
	loadedVideoMetaLabel->setTextFormat(Qt::PlainText);
	loadedVideoMetaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	loadedVideoLayout->addWidget(loadedVideoMetaLabel);

	videoPanelLayout->addWidget(loadedVideoChoiceWidget);

	urlEditorWidget = new QWidget();
	QVBoxLayout *urlEditorLayout = new QVBoxLayout(urlEditorWidget);
	urlEditorLayout->setContentsMargins(0, 0, 0, 0);
	urlEditorLayout->setSpacing(7);

	QHBoxLayout *urlRow = new QHBoxLayout();
	urlRow->setSpacing(8);
	urlInput = new QLineEdit();
	urlInput->setPlaceholderText("https://www.youtube.com/watch?v=...");
	tuneLineEdit(urlInput);
	connect(urlInput, &QLineEdit::returnPressed, this,
		&VirtuEditDock::onUrlSubmit);
	connect(urlInput, &QLineEdit::textChanged, this,
		[this]() { updateReadiness(); });
	urlRow->addWidget(urlInput, 1);

	loadVideoBtn = new QPushButton("Load");
	loadVideoBtn->setStyleSheet(STYLE_PRIMARY_BTN);
	loadVideoBtn->setMinimumWidth(72);
	loadVideoBtn->setToolTip("Load this link in Virtual Recorder.");
	connect(loadVideoBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onUrlSubmit);
	urlRow->addWidget(loadVideoBtn);

	urlEditorLayout->addLayout(urlRow);
	videoPanelLayout->addWidget(urlEditorWidget);

	videoSummaryWidget = new QWidget();
	QHBoxLayout *videoSummaryLayout = new QHBoxLayout(videoSummaryWidget);
	videoSummaryLayout->setContentsMargins(0, 0, 0, 0);
	videoSummaryLayout->setSpacing(8);

	videoInfoLabel = new QLabel("No YouTube video loaded.");
	videoInfoLabel->setWordWrap(true);
	videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	videoSummaryLayout->addWidget(videoInfoLabel, 1);

	videoPanelLayout->addWidget(videoSummaryWidget);

	captureSettingsWidget = new QWidget();
	QVBoxLayout *capturePanelLayout = new QVBoxLayout(captureSettingsWidget);
	capturePanelLayout->setContentsMargins(0, 0, 0, 0);
	capturePanelLayout->setSpacing(4);

	devicePromptWidget = new QWidget();
	devicePromptWidget->setObjectName(QStringLiteral("DevicePrompt"));
	devicePromptWidget->setStyleSheet(STYLE_PROMPT_BOX);
	QHBoxLayout *devicePromptLayout = new QHBoxLayout(devicePromptWidget);
	devicePromptLayout->setContentsMargins(0, 0, 0, 0);
	devicePromptLayout->setSpacing(8);
	devicePromptLabel = new QLabel("");
	devicePromptLabel->setStyleSheet(STYLE_SUBTLE);
	devicePromptLabel->setWordWrap(true);
	devicePromptLayout->addWidget(devicePromptLabel, 1);
	devicePromptBtn = new QPushButton("Choose");
	devicePromptBtn->setStyleSheet(STYLE_SMALL_BTN);
	devicePromptBtn->setMinimumWidth(78);
	devicePromptBtn->setToolTip(
		"Choose the OBS scene and audio to save.");
	connect(devicePromptBtn, &QPushButton::clicked, this, [this]() {
		if (sceneMgr && sceneMgr->getScene()) {
			std::vector<std::string> currentAudioIds =
				sceneMgr->getCurrentReactionAudioSourceIds();
			const bool audioReady = audioSourceIdsAvailable(
				currentAudioIds, SceneManager::listCurrentSceneAudioSources());
			SceneManager::PlayerControlsState state =
				sceneMgr->getPlayerControlsState();
			if (audioReady && !state.exactlyOneVisible()) {
				onFixPlayerControls();
				return;
			}
		}
		onOpenSettings();
	});
	devicePromptLayout->addWidget(devicePromptBtn);
	capturePanelLayout->addWidget(devicePromptWidget);

	cameraDeviceLabel = new QLabel("");
	cameraDeviceLabel->setStyleSheet(STYLE_SUBTLE);
	cameraDeviceLabel->setWordWrap(true);
	capturePanelLayout->addWidget(cameraDeviceLabel);

	micDeviceLabel = new QLabel("");
	micDeviceLabel->setStyleSheet(STYLE_SUBTLE);
	micDeviceLabel->setWordWrap(true);
	capturePanelLayout->addWidget(micDeviceLabel);

	loadedVideoLinkLabel = new QLabel("");
	loadedVideoLinkLabel->setStyleSheet(STYLE_SUBTLE);
	loadedVideoLinkLabel->setWordWrap(true);
	loadedVideoLinkLabel->setTextFormat(Qt::PlainText);
	loadedVideoLinkLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	capturePanelLayout->addWidget(loadedVideoLinkLabel);

	postLoadControlsWidget = new QWidget();
	QVBoxLayout *postLoadLayout = new QVBoxLayout(postLoadControlsWidget);
	postLoadLayout->setContentsMargins(0, 0, 0, 0);
	postLoadLayout->setSpacing(0);

	fixPlayerControlsBtn = new QPushButton("Fix YouTube Controls");
	fixPlayerControlsBtn->setStyleSheet(STYLE_SMALL_BTN);
	fixPlayerControlsBtn->setMinimumWidth(0);
	fixPlayerControlsBtn->setToolTip(
		"Reset YouTube Controls in this OBS scene.");
	connect(fixPlayerControlsBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onFixPlayerControls);
	postLoadLayout->addWidget(fixPlayerControlsBtn);

	playerControlsRepairHintLabel = new QLabel("");
	playerControlsRepairHintLabel->setStyleSheet(STYLE_SUBTLE);
	playerControlsRepairHintLabel->setWordWrap(true);
	postLoadLayout->addWidget(playerControlsRepairHintLabel);
	postLoadLayout->addSpacing(10);

	openPlayerReadyBtn = new QPushButton(PLAYER_BUTTON_LABEL);
	openPlayerReadyBtn->setStyleSheet(STYLE_SMALL_BTN);
	openPlayerReadyBtn->setMinimumWidth(0);
	openPlayerReadyBtn->setToolTip(playerControlsButtonTooltip(false));
	connect(openPlayerReadyBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onOpenProjector);
	postLoadLayout->addWidget(openPlayerReadyBtn);
	postLoadLayout->addSpacing(14);

	readyVolumeWidget = new QWidget();
	QHBoxLayout *readyVolumeLayout = new QHBoxLayout(readyVolumeWidget);
	readyVolumeLayout->setContentsMargins(0, 0, 0, 0);
	readyVolumeLayout->setSpacing(7);
	QHBoxLayout *volRow = new QHBoxLayout();
	volRow->setSpacing(7);
	QLabel *volLabel = new QLabel("YouTube Volume");
	volLabel->setStyleSheet(STYLE_BODY_LABEL);
	volLabel->setToolTip(
		"Loudness of YouTube audio compared with the Reaction Video audio.\n"
		"The last value is saved as the YouTube volume for synced playback.");
	volRow->addWidget(volLabel);
	volSliderReady = new QSlider(Qt::Horizontal);
	volSliderReady->setRange(0, 100);
	volSliderReady->setValue(relativeVolume);
	tuneSlider(volSliderReady);
	connect(volSliderReady, &QSlider::valueChanged, this,
		&VirtuEditDock::onVolumeChanged);
	connect(volSliderReady, &QSlider::sliderPressed, this,
		[this]() { volumeDragging = true; });
	connect(volSliderReady, &QSlider::sliderReleased, this, [this]() {
		volumeDragging = false;
		sceneMgr->saveVolume(relativeVolume);
	});
	volRow->addWidget(volSliderReady, 1);
	volValReady = new QLabel(QString::number(relativeVolume) + "%");
	volValReady->setStyleSheet(STYLE_BODY_LABEL);
	volValReady->setMinimumWidth(34);
	volValReady->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	volRow->addWidget(volValReady);
	readyVolumeLayout->addLayout(volRow);
	postLoadLayout->addWidget(readyVolumeWidget);
	postLoadLayout->addSpacing(14);

	audioBleedWarningLabel = new QLabel(
		"Audio check: keep YouTube audio out of the Reaction Video.");
	audioBleedWarningLabel->setStyleSheet(STYLE_ERROR_TITLE);
	audioBleedWarningLabel->setWordWrap(true);
	postLoadLayout->addWidget(audioBleedWarningLabel);
	audioBleedNoteLabel = new QLabel(
		speakerAudioBleedNoteText());
	audioBleedNoteLabel->setStyleSheet(STYLE_SUBTLE);
	audioBleedNoteLabel->setWordWrap(true);
	postLoadLayout->addWidget(audioBleedNoteLabel);

	layout->addWidget(captureSettingsWidget);
	layout->addWidget(videoStepWidget);

	QVBoxLayout *scenePanelLayout = nullptr;
	sceneLayoutWidget = makeSectionPanel(&scenePanelLayout);
	scenePanelLayout->setSpacing(5);
	QLabel *sceneSectionTitle = new QLabel("Position & Size");
	sceneSectionTitle->setStyleSheet(STYLE_CARD_TITLE);
	scenePanelLayout->addWidget(sceneSectionTitle);
	scenePanelLayout->addSpacing(2);

	sceneLayoutLabel = new QLabel(
		"Place the YouTube Controls box in OBS Scene Preview.");
	sceneLayoutLabel->setStyleSheet(STYLE_STEP_ACTION_ACCENT);
	sceneLayoutLabel->setWordWrap(true);
	scenePanelLayout->addWidget(sceneLayoutLabel);

	sceneLayoutNoteLabel = new QLabel(
		"Drag or resize it there if needed. This position is saved with the Sync File.");
	sceneLayoutNoteLabel->setStyleSheet(STYLE_STEP_NOTE);
	sceneLayoutNoteLabel->setWordWrap(true);
	scenePanelLayout->addWidget(sceneLayoutNoteLabel);
	layout->addWidget(sceneLayoutWidget);

	QVBoxLayout *reactionSettingsLayout = nullptr;
	reactionSettingsWidget = makeSectionPanel(&reactionSettingsLayout);
	reactionSettingsLayout->setSpacing(5);

	reactionSettingsImportedLabel = new QLabel("");
	reactionSettingsImportedLabel->setStyleSheet(STYLE_SUBTLE);
	reactionSettingsImportedLabel->setWordWrap(true);
	reactionSettingsLayout->addWidget(reactionSettingsImportedLabel);

	reactionSettingsResultLabel = new QLabel("");
	reactionSettingsResultLabel->setStyleSheet(STYLE_FIELD_LABEL);
	reactionSettingsResultLabel->setWordWrap(true);
	reactionSettingsLayout->addWidget(reactionSettingsResultLabel);

	QGridLayout *reactionEditorGrid = new QGridLayout();
	reactionEditorGrid->setContentsMargins(0, 2, 0, 0);
	reactionEditorGrid->setHorizontalSpacing(8);
	reactionEditorGrid->setVerticalSpacing(6);

	QLabel *reactionFormatLabel = new QLabel("Reaction format");
	reactionFormatLabel->setStyleSheet(STYLE_FIELD_LABEL);
	reactionEditorGrid->addWidget(reactionFormatLabel, 0, 0);

	reactionFormatCombo = new QComboBox();
	reactionFormatCombo->setStyleSheet(STYLE_COMBOBOX);
	reactionFormatCombo->setSizePolicy(QSizePolicy::Expanding,
					   QSizePolicy::Fixed);
	reactionFormatCombo->addItem(QStringLiteral("MP4"), QStringLiteral("mp4"));
	reactionFormatCombo->addItem(QStringLiteral("MOV"), QStringLiteral("mov"));
	reactionFormatCombo->addItem(QStringLiteral("MKV"), QStringLiteral("mkv"));
	reactionEditorGrid->addWidget(reactionFormatCombo, 0, 1);

	reactionVideoSettingLabel = new QLabel("Video quality");
	reactionVideoSettingLabel->setStyleSheet(STYLE_FIELD_LABEL);
	reactionEditorGrid->addWidget(reactionVideoSettingLabel, 1, 0);

	reactionVideoSettingCombo = new QComboBox();
	reactionVideoSettingCombo->setStyleSheet(STYLE_COMBOBOX);
	reactionVideoSettingCombo->setSizePolicy(QSizePolicy::Expanding,
						 QSizePolicy::Fixed);
	reactionEditorGrid->addWidget(reactionVideoSettingCombo, 1, 1);

	QLabel *reactionAudioLabel = new QLabel("Audio bitrate");
	reactionAudioLabel->setStyleSheet(STYLE_FIELD_LABEL);
	reactionEditorGrid->addWidget(reactionAudioLabel, 2, 0);

	reactionAudioBitrateCombo = new QComboBox();
	reactionAudioBitrateCombo->setStyleSheet(STYLE_COMBOBOX);
	reactionAudioBitrateCombo->setSizePolicy(QSizePolicy::Expanding,
						 QSizePolicy::Fixed);
	reactionEditorGrid->addWidget(reactionAudioBitrateCombo, 2, 1);
	reactionEditorGrid->setColumnStretch(1, 1);
	reactionSettingsLayout->addLayout(reactionEditorGrid);

	QLabel *reactionSettingsNote = new QLabel(
		"Combined Video follows OBS.");
	reactionSettingsNote->setStyleSheet(STYLE_STEP_NOTE);
	reactionSettingsNote->setWordWrap(true);
	reactionSettingsLayout->addWidget(reactionSettingsNote);

	reactionResetObsBtn = new QPushButton("Reset to OBS Values");
	reactionResetObsBtn->setStyleSheet(STYLE_SMALL_BTN);
	reactionResetObsBtn->setSizePolicy(QSizePolicy::Expanding,
					   QSizePolicy::Fixed);
	reactionSettingsLayout->addWidget(reactionResetObsBtn);

	connect(reactionVideoSettingCombo,
		QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { onReactionSettingsEdited(); });
	connect(reactionAudioBitrateCombo,
		QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { onReactionSettingsEdited(); });
	connect(reactionFormatCombo,
		QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { onReactionFormatChanged(); });
	connect(reactionResetObsBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onReactionSettingsReset);
	layout->addWidget(reactionSettingsWidget);

	QVBoxLayout *readyPanelLayout = nullptr;
	readyStatusWidget = makeSectionPanel(&readyPanelLayout);
	readyPanelLayout->setSpacing(0);
	QLabel *readySectionTitle = new QLabel("Before Recording");
	readySectionTitle->setStyleSheet(STYLE_CARD_TITLE);
	readyPanelLayout->addWidget(readySectionTitle);
	readyPanelLayout->addSpacing(6);

	QHBoxLayout *readyHeaderRow = new QHBoxLayout();
	readyHeaderRow->setContentsMargins(0, 0, 0, 0);
	readyHeaderRow->setSpacing(8);
	readySummaryLabel = new QLabel("");
	readySummaryLabel->setWordWrap(true);
	readySummaryLabel->setStyleSheet(STYLE_SUBTLE);
	readyHeaderRow->addWidget(readySummaryLabel, 1);
	readyPanelLayout->addLayout(readyHeaderRow);

	startRecBtn = new QPushButton("Start Recording");
	startRecBtn->setStyleSheet(STYLE_PRIMARY_BTN);
	startRecBtn->setEnabled(false);
	startRecBtn->setVisible(false);
	connect(startRecBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onStartRecording);

	readyPanelLayout->addSpacing(12);
	readyPanelLayout->addWidget(postLoadControlsWidget);
	readyPanelLayout->addSpacing(16);
	readyPanelLayout->addWidget(startRecBtn);
	readyPanelLayout->addSpacing(10);

	readyDetailsBtn = new QToolButton();
	readyDetailsBtn->setText("Recording details");
	readyDetailsBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	readyDetailsBtn->setArrowType(Qt::RightArrow);
	readyDetailsBtn->setAutoRaise(true);
	readyDetailsBtn->setCursor(Qt::PointingHandCursor);
	readyDetailsBtn->setStyleSheet(STYLE_DISCLOSURE_BTN);
	connect(readyDetailsBtn, &QToolButton::clicked, this, [this]() {
		setReadyDetailsExpanded(!readyDetailsExpanded);
	});
	readyPanelLayout->addWidget(readyDetailsBtn, 0, Qt::AlignLeft);

	readyDetailsWidget = new QWidget();
	QVBoxLayout *readyDetailsLayout = new QVBoxLayout(readyDetailsWidget);
	readyDetailsLayout->setContentsMargins(0, 0, 0, 0);
	readyDetailsLayout->setSpacing(4);

	deviceReadyLabel = new QLabel("");
	deviceReadyLabel->setStyleSheet(STYLE_SUBTLE);
	readyDetailsLayout->addWidget(deviceReadyLabel);

	youtubeReadyLabel = new QLabel("");
	youtubeReadyLabel->setStyleSheet(STYLE_SUBTLE);
	readyDetailsLayout->addWidget(youtubeReadyLabel);

	controlsReadyLabel = new QLabel("");
	controlsReadyLabel->setStyleSheet(STYLE_SUBTLE);
	controlsReadyLabel->setWordWrap(true);
	readyDetailsLayout->addWidget(controlsReadyLabel);

	canvasReadyLabel = new QLabel("");
	canvasReadyLabel->setStyleSheet(STYLE_SUBTLE);
	canvasReadyLabel->setWordWrap(true);
	readyDetailsLayout->addWidget(canvasReadyLabel);

	diskReadyLabel = new QLabel("");
	diskReadyLabel->setStyleSheet(STYLE_SUBTLE);
	readyDetailsLayout->addWidget(diskReadyLabel);

	obsReadyLabel = new QLabel("");
	obsReadyLabel->setStyleSheet(STYLE_SUBTLE);
	readyDetailsLayout->addWidget(obsReadyLabel);
	readyPanelLayout->addWidget(readyDetailsWidget);

	recordFlowLabel = new QLabel(
		"Outputs: Reaction Video, Sync File, and Combined Video.");
	recordFlowLabel->setStyleSheet(STYLE_SUBTLE);
	recordFlowLabel->setWordWrap(true);
	recordFlowLabel->setToolTip(
		"Reaction Video: selected OBS scene without YouTube Controls, using the selected audio.\n"
		"Sync File: play/pause/seek timing, video box layout, and volume.\n"
		"Combined Video: normal OBS recording with YouTube Controls, using your current OBS recording settings.");
	readyPanelLayout->addWidget(recordFlowLabel);

	layout->addWidget(readyStatusWidget);

	readyNavWidget = new QWidget();
	QHBoxLayout *navRow = new QHBoxLayout(readyNavWidget);
	navRow->setContentsMargins(0, 0, 0, 0);
	navRow->setSpacing(8);
	backStepBtn = new QPushButton("Back");
	backStepBtn->setStyleSheet(STYLE_BACK_BTN);
	backStepBtn->setFixedWidth(54);
	backStepBtn->setToolTip("Go back one setup step.");
	connect(backStepBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onBackStep);
	navRow->addWidget(backStepBtn);
	navRow->addStretch();
	nextStepBtn = new QPushButton("Next");
	nextStepBtn->setStyleSheet(STYLE_SMALL_BTN_ACTIVE);
	nextStepBtn->setMinimumWidth(86);
	nextStepBtn->setToolTip("Available after the current step is complete.");
	connect(nextStepBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onNextStep);

	skipToRecordWidget = new QWidget();
	QVBoxLayout *skipLayout = new QVBoxLayout(skipToRecordWidget);
	skipLayout->setContentsMargins(0, 2, 0, 0);
	skipLayout->setSpacing(2);
	skipToRecordBtn = new QPushButton("Skip to Recording");
	skipToRecordBtn->setStyleSheet(STYLE_SKIP_BTN);
	skipToRecordBtn->setCursor(Qt::PointingHandCursor);
	skipToRecordBtn->setMinimumWidth(118);
	skipToRecordBtn->setToolTip(
		"Jump to Step 5 - Record. Skips Position & Size and Reaction Video settings.");
	connect(skipToRecordBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onSkipToRecord);
	skipLayout->addWidget(skipToRecordBtn, 0, Qt::AlignRight);

	QWidget *rightNavWidget = new QWidget();
	QVBoxLayout *rightNavLayout = new QVBoxLayout(rightNavWidget);
	rightNavLayout->setContentsMargins(0, 0, 0, 0);
	rightNavLayout->setSpacing(4);
	rightNavLayout->addWidget(nextStepBtn, 0, Qt::AlignRight);
	rightNavLayout->addWidget(skipToRecordWidget, 0, Qt::AlignRight);
	navRow->addWidget(rightNavWidget);
	layout->addWidget(readyNavWidget);

	setReadyDetailsExpanded(false);

	layout->addStretch();

	updateReadiness();
}

// --- Recording Page ---

void VirtuEditDock::setupRecordingPage()
{
	QVBoxLayout *layout = nullptr;
	recordingPage = makeScrollPage(&layout);

	recTimeLabel = new QLabel("REC 00:00:00");
	recTimeLabel->setStyleSheet(STYLE_REC_BADGE);
	layout->addWidget(recTimeLabel);

	layout->addWidget(makeSeparator());
	layout->addSpacing(4);

	// Read-only playback status (control happens in the video window)
	playbackLabel = new QLabel("-- 00:00 / 00:00");
	playbackLabel->setStyleSheet(STYLE_BODY_LABEL);
	layout->addWidget(playbackLabel);

	// Reopen / raise the player window (essential surface during recording)
	QPushButton *openPlayerRecBtn =
		new QPushButton(PLAYER_BUTTON_LABEL);
	openPlayerRecBtn->setStyleSheet(STYLE_SMALL_BTN);
	openPlayerRecBtn->setToolTip(
		"Open or raise the YouTube Controls window used for sync timing.");
	connect(openPlayerRecBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onOpenProjector);
	layout->addWidget(openPlayerRecBtn);

	QLabel *controlHint = new QLabel(recordingControlsHintText());
	controlHint->setStyleSheet(STYLE_SUBTLE);
	controlHint->setWordWrap(true);
	layout->addWidget(controlHint);

	QLabel *audioWarning = new QLabel(
		"Audio check: keep YouTube audio out of the Reaction Video.");
	audioWarning->setStyleSheet(STYLE_ERROR_TITLE);
	audioWarning->setWordWrap(true);
	layout->addWidget(audioWarning);

	QLabel *audioWarningNote = new QLabel(
		speakerAudioBleedNoteText());
	audioWarningNote->setStyleSheet(STYLE_SUBTLE);
	audioWarningNote->setWordWrap(true);
	layout->addWidget(audioWarningNote);

	layout->addSpacing(10);

	// YouTube volume (live-adjustable during recording)
	QLabel *volLabelRec = new QLabel("YouTube Volume");
	volLabelRec->setStyleSheet(STYLE_FIELD_LABEL);
	volLabelRec->setToolTip(
		"Loudness of YouTube audio compared with the Reaction Video audio.\n"
		"The last value is saved as the YouTube volume for synced playback.");
	layout->addWidget(volLabelRec);

	QHBoxLayout *volRowRec = new QHBoxLayout();
	volRowRec->setSpacing(6);
	volSliderRec = new QSlider(Qt::Horizontal);
	volSliderRec->setRange(0, 100);
	volSliderRec->setValue(relativeVolume);
	tuneSlider(volSliderRec);
	volValRec = new QLabel(QString::number(relativeVolume) + "%");
	volValRec->setStyleSheet(STYLE_SUBTLE);
	volValRec->setMinimumWidth(34);
	volValRec->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	connect(volSliderRec, &QSlider::valueChanged, this,
		&VirtuEditDock::onVolumeChanged);
	connect(volSliderRec, &QSlider::sliderPressed, this,
		[this]() { volumeDragging = true; });
	connect(volSliderRec, &QSlider::sliderReleased, this, [this]() {
		volumeDragging = false;
		sceneMgr->saveVolume(relativeVolume);
	});
	volRowRec->addWidget(volSliderRec);
	volRowRec->addWidget(volValRec);
	layout->addLayout(volRowRec);

	layout->addWidget(makeSeparator());
	layout->addSpacing(4);

	syncCountLabel = new QLabel(syncTimingCountText(0));
	syncCountLabel->setStyleSheet(STYLE_BODY_LABEL);
	layout->addWidget(syncCountLabel);

	layout->addSpacing(10);

	stopRecBtn = new QPushButton("Stop Recording");
	stopRecBtn->setStyleSheet(STYLE_DANGER_BTN);
	connect(stopRecBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onStopRecording);
	layout->addWidget(stopRecBtn);

	layout->addStretch();
}

// --- Complete Page ---

void VirtuEditDock::setupCompletePage()
{
	QVBoxLayout *layout = nullptr;
	completePage = makeScrollPage(&layout);

	completeTitleLabel = new QLabel("Recording Complete");
	completeTitleLabel->setStyleSheet(STYLE_COMPLETE_TITLE);
	layout->addWidget(completeTitleLabel);

	layout->addSpacing(4);

	completeInfoLabel = new QLabel("");
	completeInfoLabel->setWordWrap(true);
	completeInfoLabel->setMinimumWidth(0);
	completeInfoLabel->setSizePolicy(QSizePolicy::Ignored,
					 QSizePolicy::Preferred);
	completeInfoLabel->setStyleSheet(STYLE_COMPLETE_INFO);
	layout->addWidget(completeInfoLabel);

	openFolderBtn = new QPushButton("Open Folder");
	openFolderBtn->setStyleSheet(STYLE_PRIMARY_BTN);
	openFolderBtn->setToolTip("Open the recording folder.");
	openFolderBtn->setEnabled(false);
	connect(openFolderBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onOpenOutputFolder);
	layout->addWidget(openFolderBtn);

	completeSyncLabel = new QLabel("");
	completeSyncLabel->setStyleSheet(STYLE_SUBTLE);
	completeSyncLabel->setWordWrap(true);
	completeSyncLabel->setMinimumWidth(0);
	completeSyncLabel->setSizePolicy(QSizePolicy::Ignored,
					 QSizePolicy::Preferred);
	layout->addWidget(completeSyncLabel);

	layout->addSpacing(8);
	layout->addWidget(makeSeparator());
	layout->addSpacing(6);

	QLabel *filesLabel = new QLabel("Saved files");
	filesLabel->setStyleSheet(STYLE_FIELD_LABEL);
	layout->addWidget(filesLabel);

	QWidget *fileTableWidget = new QWidget();
	QGridLayout *fileTable = new QGridLayout(fileTableWidget);
	fileTable->setContentsMargins(0, 0, 0, 0);
	fileTable->setHorizontalSpacing(10);
	fileTable->setVerticalSpacing(4);
	fileTable->setColumnStretch(0, 1);

	auto addFileRow = [fileTable](int rowIndex, const QString &name,
				      QLabel **statusLabel) {
		QLabel *nameLabel = new QLabel(name);
		nameLabel->setStyleSheet(STYLE_SUBTLE);
		nameLabel->setWordWrap(false);
		nameLabel->setMinimumWidth(0);
		nameLabel->setSizePolicy(QSizePolicy::Expanding,
					 QSizePolicy::Preferred);

		QLabel *status = new QLabel("Pending");
		status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		status->setStyleSheet(STYLE_COMPLETE_STATUS_SUBTLE);
		status->setWordWrap(false);
		status->setMinimumWidth(0);
		status->setSizePolicy(QSizePolicy::Preferred,
				      QSizePolicy::Preferred);

		fileTable->addWidget(nameLabel, rowIndex, 0);
		fileTable->addWidget(status, rowIndex, 1);
		*statusLabel = status;
	};

	addFileRow(0, "Reaction Video", &reactStatusLabel);
	addFileRow(1, "Sync File", &jsonStatusLabel);
	addFileRow(2, "Combined Video", &compositeStatusLabel);
	layout->addWidget(fileTableWidget);

	layout->addWidget(makeSeparator());
	layout->addSpacing(6);

	QLabel *nextRecordingLabel = new QLabel("Next recording");
	nextRecordingLabel->setStyleSheet(STYLE_COMPLETE_SECTION_TITLE);
	layout->addWidget(nextRecordingLabel);

	nextTakeLayout = new QBoxLayout(QBoxLayout::TopToBottom);
	nextTakeLayout->setSpacing(6);

	recordAnotherBtn = new QPushButton("Record Same Video Again");
	recordAnotherBtn->setStyleSheet(STYLE_PRIMARY_BTN_ACTIVE);
	recordAnotherBtn->setToolTip(
		"Use the same loaded YouTube link. You will return to Position & Size before recording again.");
	connect(recordAnotherBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onRecordAnotherTake);
	nextTakeLayout->addWidget(recordAnotherBtn);

	newVideoBtn = new QPushButton("Record Different Video");
	newVideoBtn->setStyleSheet(STYLE_FLAT_BTN);
	newVideoBtn->setToolTip(
		"Clear the current YouTube link and prepare a different video.");
	connect(newVideoBtn, &QPushButton::clicked, this,
		&VirtuEditDock::onNewVideo);
	nextTakeLayout->addWidget(newVideoBtn);
	layout->addLayout(nextTakeLayout);

	layout->addStretch();
}

// --- Slots ---

std::string VirtuEditDock::extractVideoId(const QString &url)
{
	return virtuedit_extract_youtube_video_id(
		url.toUtf8().constData());
}

std::string VirtuEditDock::basename(const std::string &path) const
{
	if (path.empty())
		return "";
	QByteArray bytes =
		QFileInfo(stdPathToQString(path)).fileName().toUtf8();
	return std::string(bytes.constData(), bytes.size());
}

QString VirtuEditDock::currentVideoSummary() const
{
	const auto &info = yt->getInfo();
	if (info.title.empty() && info.duration <= 0)
		return "YouTube video loaded.";
	if (info.title.empty())
		return "YouTube video loaded. (" + formatTime(info.duration) + ")";

	return QString::fromUtf8(info.title.c_str()) + " (" +
	       formatTime(info.duration) + ")";
}

QString VirtuEditDock::currentLoadedVideoLink() const
{
	QString link = loadedVideoUrl.trimmed();
	if (!link.isEmpty())
		return link;

	std::string videoId = loadedVideoId;
	if (videoId.empty() && yt)
		videoId = yt->getCurrentVideoId();
	if (!videoId.empty())
		return QStringLiteral("https://youtu.be/") +
		       QString::fromUtf8(videoId.c_str());

	return QString();
}

bool VirtuEditDock::canKeepCurrentLoadedVideo() const
{
	const QString loadedLink = currentLoadedVideoLink();
	return videoLoaded && editingVideoUrl && !loadedLink.isEmpty() &&
	       urlInput && urlInput->text().trimmed() == loadedLink;
}

bool VirtuEditDock::restorePreviousVideoAfterLoadFailure(const QString &reason)
{
	if (fallbackVideoId.empty())
		return false;

	QString fallbackUrl = fallbackVideoUrl;
	std::string fallbackId = fallbackVideoId;
	fallbackVideoUrl.clear();
	fallbackVideoId.clear();

	pendingVideoUrl = fallbackUrl;
	pendingVideoId = fallbackId;
	videoLoading = true;
	videoLoaded = false;
	editingVideoUrl = false;

	if (!fallbackUrl.isEmpty())
		urlInput->setText(fallbackUrl);
	videoInfoLabel->setText(reason + "\nRestoring the previous video...");
	videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	updateReadiness();

	yt->loadVideo(fallbackId);
	openPlayerControlsWindow(false);
	loadTimer->start(10000);
	return true;
}

std::string VirtuEditDock::getRecordingOutputDir() const
{
	std::string dir;
	bool outputIsRemote = false;
	config_t *config = obs_frontend_get_profile_config();
	if (config) {
		const char *mode =
			config_get_string(config, "Output", "Mode");
		const char *p = nullptr;
		if (mode && strcmp(mode, "Advanced") == 0) {
			const char *recordingType =
				config_get_string(config, "AdvOut", "RecType");
			if (recordingType && strcmp(recordingType, "FFmpeg") == 0) {
				if (config_get_bool(config, "AdvOut", "FFOutputToFile"))
					p = config_get_string(config, "AdvOut", "FFFilePath");
				else
					outputIsRemote = true;
			} else {
				p = config_get_string(config, "AdvOut", "RecFilePath");
			}
		} else {
			p = config_get_string(config, "SimpleOutput", "FilePath");
		}
		if (p)
			dir = p;
	}
	if (outputIsRemote)
		return "";
	if (dir.empty())
		dir = "/tmp";
	return dir;
}

std::string VirtuEditDock::currentOutputFolder() const
{
	std::string sourcePath = !lastReactBase.empty() ? lastReactBase
				: (!lastReactPath.empty() ? lastReactPath
				: (!lastJsonPath.empty() ? lastJsonPath
				: lastRecordingPath));
	if (sourcePath.empty())
		return "";

	QString folder =
		QFileInfo(stdPathToQString(sourcePath)).absolutePath();
	return qPathToStd(folder);
}

std::string VirtuEditDock::selectedReactionOutputFormat() const
{
	const std::string saved =
		sceneMgr ? normalizeReactionOutputFormat(
				   sceneMgr->getReactionOutputFormat())
			 : std::string();
	if (!saved.empty())
		return saved;

	const std::string obsFormat = obsRecordingOutputFormat();
	return obsFormat.empty() ? std::string("mp4") : obsFormat;
}

void VirtuEditDock::setReadyDetailsExpanded(bool expanded)
{
	readyDetailsExpanded = expanded;
	if (readyDetailsWidget)
		readyDetailsWidget->setVisible(expanded);
	if (readyDetailsBtn) {
		readyDetailsBtn->setArrowType(expanded ? Qt::DownArrow :
							 Qt::RightArrow);
		readyDetailsBtn->setText("Recording details");
		readyDetailsBtn->setToolTip(
			expanded ? "Hide recording details." :
				   "Show recording details.");
	}
}

bool VirtuEditDock::ensureYouTubePlayerReady(const std::string &videoId)
{
	(void)videoId;

	if (!sceneMgr->getScene() && !sceneMgr->selectReactionScene("")) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Failed to select reaction scene before loading YouTube");
		return false;
	}

	if (!yt->getBrowserSource())
		yt->createBrowserSource("");

	obs_source_t *browserSrc = yt->getBrowserSource();
	if (!browserSrc) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Failed to create YouTube browser source before loading");
		return false;
	}

	if (!sceneMgr->addBrowserSource(browserSrc)) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Failed to add YouTube browser source before loading");
		return false;
	}

	sceneMgr->activateScene();
	return true;
}

void VirtuEditDock::updateReadiness()
{
	std::vector<std::string> currentAudioIds =
		sceneMgr ? sceneMgr->getCurrentReactionAudioSourceIds()
			 : std::vector<std::string>();
	std::vector<DeviceInfo> availableAudioSources =
		SceneManager::listCurrentSceneAudioSources();
	bool sceneSourceReady = sceneMgr && sceneMgr->getScene();
	bool sceneReady = sceneSourceReady;
	bool audioReady =
		sceneMgr &&
		audioSourceIdsAvailable(currentAudioIds, availableAudioSources);
	bool devicesReady = sceneReady && audioReady;
	SceneManager::PlayerControlsState controlsState;
	if (sceneSourceReady)
		controlsState = sceneMgr->getPlayerControlsState();
	bool controlsReady = sceneSourceReady && controlsState.exactlyOneVisible();
	QString controlsBlockMessage =
		playerControlsBlockingMessage(controlsState);
	bool audioRisk = audioReady &&
			 selectedAudioSourceIdsMayIncludeSystemAudio(
				 availableAudioSources, currentAudioIds);
	QString missingDevices = QStringLiteral("OBS scene and audio to save");
	if (sceneReady && !audioReady)
		missingDevices = QStringLiteral("audio to save");
	else if (!sceneReady && audioReady)
		missingDevices = QStringLiteral("OBS scene");
	bool obsAlreadyRecording = obs_frontend_recording_active() &&
				   !recordingStarting && !recordingActive &&
				   !stoppingRecording;
	bool recordingBusy = isRecordingFlowBusy();
	bool videoReady = videoLoaded && !editingVideoUrl && !videoLoading;
	bool needsSources = !devicesReady;
	bool readyStep = devicesReady && videoReady;
	if (!devicesReady || !videoReady)
		readyWorkflowStep = READY_STEP_VIDEO;
	else if (readyWorkflowStep < READY_STEP_VIDEO)
		readyWorkflowStep = READY_STEP_VIDEO;
	else if (readyWorkflowStep > READY_STEP_RECORD)
		readyWorkflowStep = READY_STEP_RECORD;

	bool controlsNeedRepair =
		devicesReady && !controlsReady && !recordingBusy;
	if (controlsNeedRepair && pages && pages->currentWidget() == readyPage &&
	    setupPage && !videoLoading && !waitingForComposite) {
		populateDeviceLists();
		pages->setCurrentWidget(setupPage);
		return;
	}
	bool showVideoStep =
		devicesReady && !controlsNeedRepair &&
		readyWorkflowStep == READY_STEP_VIDEO;
	bool showLayoutStep =
		readyStep && !controlsNeedRepair &&
		readyWorkflowStep == READY_STEP_LAYOUT;
	bool showReactionStep =
		readyStep && !controlsNeedRepair &&
		readyWorkflowStep == READY_STEP_REACTION;
	bool showRecordStep =
		readyStep && !controlsNeedRepair &&
		readyWorkflowStep == READY_STEP_RECORD;
	bool showRecordingPanel = showRecordStep || recordingBusy;
	if (deviceReadyLabel) {
		deviceReadyLabel->setText(
			devicesReady ?
				(controlsReady ?
					 QStringLiteral("OBS scene setup: Ready") :
					 QStringLiteral("OBS scene setup: Add YouTube Controls")) :
				QStringLiteral("OBS scene setup: Choose ") +
					missingDevices);
		deviceReadyLabel->setStyleSheet(
			(devicesReady && controlsReady) ? STYLE_SUCCESS :
							 STYLE_ERROR);
	}

	if (youtubeReadyLabel) {
		if (videoLoading) {
			youtubeReadyLabel->setText(
				"Video: Loading in YouTube Controls...");
			youtubeReadyLabel->setStyleSheet(STYLE_SUBTLE);
		} else if (videoLoaded && editingVideoUrl) {
			youtubeReadyLabel->setText(
				canKeepCurrentLoadedVideo() ?
					"Video: Keep loaded video" :
					"Video: Reload link");
			youtubeReadyLabel->setStyleSheet(STYLE_SUBTLE);
		} else {
			youtubeReadyLabel->setText(videoLoaded ?
							  "Video: Loaded" :
							  "Video: Load YouTube");
			youtubeReadyLabel->setStyleSheet(videoLoaded ? STYLE_SUCCESS
								     : STYLE_SUBTLE);
		}
	}

	if (controlsReadyLabel) {
		controlsReadyLabel->setText(playerControlsStatusText(controlsState));
		controlsReadyLabel->setStyleSheet(
			controlsReady ? STYLE_SUCCESS :
					(sceneSourceReady ? STYLE_ERROR :
								STYLE_SUBTLE));
		QString controlsTooltip = sceneSourceReady ?
						  playerControlsStateDetailText(
							  controlsState) :
						  QStringLiteral(
							  "Choose an OBS scene first.");
		if (!controlsReady) {
			const QString repairHint =
				playerControlsRepairHintText(controlsState);
			if (!repairHint.isEmpty()) {
				controlsTooltip += QStringLiteral("\n");
				controlsTooltip += repairHint;
			}
		}
		controlsReadyLabel->setToolTip(controlsTooltip);
	}

	auto canvas = virtuedit_get_canvas_size();
	auto output = virtuedit_get_output_size();
	bool canvas16x9 =
		virtuedit_is_near_16x9(canvas.width, canvas.height);
	bool output16x9 =
		virtuedit_is_near_16x9(output.width, output.height);
	bool videoSettings16x9 = canvas16x9 && output16x9;
	if (canvasReadyLabel) {
		QString text = videoSizeText("Canvas", canvas) +
			       QStringLiteral(" / ") +
			       videoSizeText("Output", output);
		if (videoSettings16x9)
			text += QStringLiteral(" (16:9)");
		else
			text += QStringLiteral(" - 16:9 works best");
		text = QStringLiteral("Format: ") + text;
		canvasReadyLabel->setText(text);
		canvasReadyLabel->setStyleSheet(videoSettings16x9 ? STYLE_SUCCESS
								  : STYLE_ERROR);
		canvasReadyLabel->setToolTip(
			videoSettings16x9 ?
				QStringLiteral("OBS video settings are 16:9.") :
				QStringLiteral(
					"Recording follows OBS video settings. Use a 16:9 "
					"canvas/output for standard YouTube reactions."));
	}

	bool diskOk = true;
	double freeGB = freeDiskGB(getRecordingOutputDir());
	if (freeGB >= 0 && freeGB < 1.0)
		diskOk = false;

	if (diskReadyLabel) {
		QString text = QStringLiteral("Storage: ");
		if (freeGB >= 0)
			text += QString::number(freeGB, 'f', 1) +
				QStringLiteral(" GB free");
		else
			text += QStringLiteral("Available");
		diskReadyLabel->setText(text);
		diskReadyLabel->setStyleSheet(diskOk ? STYLE_SUCCESS
						      : STYLE_ERROR);
	}

	if (obsReadyLabel) {
		obsReadyLabel->setText(obsAlreadyRecording ?
					       "OBS: Stop manual recording" :
					       "OBS: Ready");
		obsReadyLabel->setStyleSheet(obsAlreadyRecording ? STYLE_ERROR
								 : STYLE_SUCCESS);
	}

	bool readyToRecordNow = devicesReady && videoLoaded && diskOk &&
				!videoLoading && !editingVideoUrl &&
				controlsReady && playerControlsOpened &&
				!obsAlreadyRecording &&
				!recordingStarting && !recordingActive &&
				!stoppingRecording &&
				!webcamRecorder.isFinalizing() &&
				!waitingForComposite;

	if (readyTitleLabel) {
		if (recordingBusy)
			readyTitleLabel->setText("Finalizing");
		else if (needsSources || controlsNeedRepair)
			readyTitleLabel->setText("Step 1 - OBS Scene & Audio");
		else if (showVideoStep)
			readyTitleLabel->setText("Step 2 - YouTube Video");
		else if (showLayoutStep)
			readyTitleLabel->setText("Step 3 - Position & Size");
		else if (showReactionStep)
			readyTitleLabel->setText("Step 4 - Reaction Video");
		else
			readyTitleLabel->setText("Step 5 - Record");
	}
	if (readyCaptionLabel) {
		readyCaptionLabel->clear();
		readyCaptionLabel->setVisible(false);
	}
	bool showBackStep = false;
	bool showNextStep = false;
	bool showSkipToRecord = false;
	const bool canKeepCurrentVideo = canKeepCurrentLoadedVideo();
	const bool canContinueWithLoadedVideo =
		videoLoaded && (!editingVideoUrl || canKeepCurrentVideo);
	if (backStepBtn) {
		const bool canBack = devicesReady && !controlsNeedRepair &&
				     !videoLoading &&
				     !recordingBusy && !waitingForComposite;
		showBackStep = canBack;
		backStepBtn->setVisible(showBackStep);
		backStepBtn->setEnabled(canBack);
		if (readyWorkflowStep == READY_STEP_RECORD)
			backStepBtn->setToolTip("Go back to Reaction Video.");
		else if (readyWorkflowStep == READY_STEP_REACTION)
			backStepBtn->setToolTip("Go back to Position & Size.");
		else if (readyWorkflowStep == READY_STEP_LAYOUT)
			backStepBtn->setToolTip("Go back to YouTube Video.");
		else
			backStepBtn->setToolTip("Go back to OBS Scene Setup.");
	}
	if (nextStepBtn) {
		showNextStep = devicesReady && !controlsNeedRepair &&
			       !recordingBusy &&
			       !waitingForComposite && !videoLoading &&
			       readyWorkflowStep != READY_STEP_RECORD;
		bool enabled = false;
		QString nextTip =
			QStringLiteral("Available after the current step is complete.");
		if (readyWorkflowStep == READY_STEP_VIDEO) {
			enabled = canContinueWithLoadedVideo;
			if (canContinueWithLoadedVideo) {
				nextTip = QStringLiteral(
					"Continue to Position & Size.");
			} else if (!enabled) {
				nextTip = QStringLiteral(
					"Load the YouTube link before continuing.");
			}
		} else if (readyWorkflowStep == READY_STEP_LAYOUT) {
			enabled = readyStep;
			nextTip = QStringLiteral("Continue to the next step.");
		} else if (readyWorkflowStep == READY_STEP_REACTION) {
			enabled = readyStep;
			nextTip = QStringLiteral("Continue to the next step.");
		}
		QString nextText = QStringLiteral("Next");
		if (readyWorkflowStep == READY_STEP_VIDEO &&
		    canContinueWithLoadedVideo) {
			nextText = QStringLiteral("Next");
		} else if (readyWorkflowStep == READY_STEP_LAYOUT) {
			nextText = QStringLiteral("Next");
		} else if (readyWorkflowStep == READY_STEP_REACTION) {
			nextText = QStringLiteral("Next");
		}
		nextStepBtn->setText(nextText);
		nextStepBtn->setMinimumWidth(86);
		nextStepBtn->setVisible(showNextStep);
		nextStepBtn->setEnabled(enabled);
		nextStepBtn->setStyleSheet(enabled ? STYLE_SMALL_BTN_ACTIVE :
						    STYLE_SMALL_BTN);
		nextStepBtn->setToolTip(nextTip);
	}
	if (skipToRecordBtn) {
		const bool canSkipToRecord =
			readyWorkflowStep == READY_STEP_VIDEO && readyStep &&
			controlsReady && !controlsNeedRepair && !recordingBusy &&
			!waitingForComposite && !videoLoading && videoLoaded &&
			!editingVideoUrl;
		showSkipToRecord = canSkipToRecord;
		skipToRecordBtn->setEnabled(canSkipToRecord);
		skipToRecordBtn->setToolTip(
			"Jump to Step 5 - Record. Skips Position & Size and Reaction Video settings.");
	}
	if (skipToRecordWidget)
		skipToRecordWidget->setVisible(showSkipToRecord);
	if (readyNavWidget)
		readyNavWidget->setVisible(showBackStep || showNextStep ||
					   showSkipToRecord);
	if (devicePromptLabel) {
		if (showLayoutStep) {
			devicePromptLabel->setText(
				"Check the YouTube Controls position in OBS Scene Preview.");
			devicePromptLabel->setStyleSheet(STYLE_SUBTLE);
		} else if (controlsNeedRepair) {
			devicePromptLabel->setText(controlsBlockMessage);
			devicePromptLabel->setStyleSheet(STYLE_ERROR);
		} else {
			devicePromptLabel->setText(
				(devicesReady && controlsReady) ?
					QStringLiteral("OBS scene setup ready") :
					QStringLiteral("Set up: ") + missingDevices);
			devicePromptLabel->setStyleSheet(
				(devicesReady && controlsReady) ? STYLE_SUCCESS :
								  STYLE_ERROR);
		}
	}
	if (devicePromptBtn) {
		devicePromptBtn->setVisible(needsSources || controlsNeedRepair);
		devicePromptBtn->setText(
			controlsNeedRepair ? playerControlsFixButtonText(controlsState) :
						 (devicesReady ? "Edit" :
								 "Set Up"));
		devicePromptBtn->setEnabled(!recordingBusy);
		devicePromptBtn->setStyleSheet(controlsNeedRepair ?
						       STYLE_SMALL_BTN_ACTIVE :
						       STYLE_SMALL_BTN);
		QString promptTip = QStringLiteral("Choose the OBS scene and audio to save.");
		if (controlsNeedRepair) {
			promptTip = controlsBlockMessage;
			const QString repairHint =
				playerControlsRepairHintText(controlsState);
			if (!repairHint.isEmpty()) {
				if (!promptTip.isEmpty())
					promptTip += QStringLiteral("\n");
				promptTip += repairHint;
			}
		}
		devicePromptBtn->setToolTip(promptTip);
	}
	if (cameraDeviceLabel) {
		std::string sceneName =
			sceneMgr ? sceneMgr->getCurrentSceneName() : std::string();
		QString scene = sceneName.empty() ?
					QStringLiteral("Not set") :
					QString::fromUtf8(sceneName.c_str());
		cameraDeviceLabel->setText(QStringLiteral("Current OBS scene: ") +
					   scene);
		cameraDeviceLabel->setStyleSheet(sceneReady ? STYLE_SUCCESS
							     : STYLE_ERROR);
	}
	if (micDeviceLabel) {
		QString audio = selectedAudioSourceNames(availableAudioSources,
						 currentAudioIds);
		QString text = QStringLiteral("Saved in Reaction Video: ") + audio;
		if (audioRisk)
			text += QStringLiteral("\n") + reactionAudioWarning();
		micDeviceLabel->setText(text);
		micDeviceLabel->setStyleSheet(
			audioReady ? (audioRisk ? STYLE_WARNING : STYLE_SUCCESS)
				   : STYLE_ERROR);
	}
	if (loadedVideoLinkLabel) {
		const QString loadedLink = currentLoadedVideoLink();
		QString text = QStringLiteral("Loaded YouTube video: ") +
			       currentVideoSummary();
		if (!loadedLink.isEmpty())
			text += QStringLiteral("\nLink: ") + loadedLink;
		loadedVideoLinkLabel->setText(text);
		loadedVideoLinkLabel->setVisible(
			videoLoaded && !controlsNeedRepair &&
			readyDetailsExpanded);
	}
	if (videoStepWidget)
		videoStepWidget->setVisible((showVideoStep || videoLoading) &&
					    !controlsNeedRepair);
	if (urlInput && videoLoaded && !editingVideoUrl && !videoLoading) {
		const QString loadedLink = currentLoadedVideoLink();
		if (!loadedLink.isEmpty() &&
		    urlInput->text().trimmed() != loadedLink) {
			QSignalBlocker block(urlInput);
			urlInput->setText(loadedLink);
		}
	}
	if (savedDeviceNoteLabel)
		savedDeviceNoteLabel->setVisible(showVideoStep && devicesReady);
	if (devicePromptWidget)
		devicePromptWidget->setVisible((needsSources || controlsNeedRepair) &&
					       !recordingBusy);
	const bool showLoadedVideoChoice = false;
	if (loadedVideoChoiceWidget)
		loadedVideoChoiceWidget->setVisible(showLoadedVideoChoice);
	if (loadedVideoTitleLabel)
		loadedVideoTitleLabel->setText(currentVideoSummary());
	if (loadedVideoMetaLabel) {
		QString loadedLink = currentLoadedVideoLink();
		QString metaText;
		if (!loadedLink.isEmpty())
			metaText = QStringLiteral("Link: ") + loadedLink +
				   QStringLiteral("\n");
		metaText +=
			editingVideoUrl ?
				QStringLiteral("Paste a different link below, or keep the loaded video.") :
				QStringLiteral("Use this video for the next recording, or change the link.");
		loadedVideoMetaLabel->setText(
			metaText);
	}
	if (changeVideoBtn) {
		changeVideoBtn->setVisible(showLoadedVideoChoice);
		changeVideoBtn->setEnabled(!recordingBusy && !videoLoading);
		changeVideoBtn->setText(editingVideoUrl ? "Cancel Change" :
						       "Change Video");
		changeVideoBtn->setToolTip(
			editingVideoUrl ?
				QStringLiteral("Close the link field and keep the loaded YouTube video.") :
				QStringLiteral("Open the link field to load a different YouTube video."));
		changeVideoBtn->setStyleSheet(STYLE_SMALL_BTN);
	}
	if (urlEditorWidget)
		urlEditorWidget->setVisible((showVideoStep || videoLoading) &&
					    !controlsNeedRepair);
	if (videoSummaryWidget) {
		bool hasStatusMessage =
			videoInfoLabel &&
			videoInfoLabel->text() != QStringLiteral("No YouTube video loaded.");
		videoSummaryWidget->setVisible(
			videoLoading || (!showLoadedVideoChoice &&
					 (videoLoaded || hasStatusMessage)));
	}
	if (cameraDeviceLabel)
		cameraDeviceLabel->setVisible(needsSources || controlsNeedRepair ||
					       readyDetailsExpanded);
	if (micDeviceLabel)
		micDeviceLabel->setVisible(needsSources || controlsNeedRepair ||
					    readyDetailsExpanded);
	if (captureSettingsWidget)
		captureSettingsWidget->setVisible(
			(needsSources || controlsNeedRepair || showLayoutStep ||
			 readyDetailsExpanded) &&
			!videoLoading);
	if (postLoadControlsWidget)
		postLoadControlsWidget->setVisible(showRecordingPanel);
	if (fixPlayerControlsBtn) {
		fixPlayerControlsBtn->setVisible(controlsNeedRepair);
		fixPlayerControlsBtn->setEnabled(controlsNeedRepair);
		fixPlayerControlsBtn->setText(
			playerControlsFixButtonText(controlsState));
		fixPlayerControlsBtn->setStyleSheet(
			controlsNeedRepair ? STYLE_SMALL_BTN_ACTIVE :
					     STYLE_SMALL_BTN);
		QString repairTip = controlsBlockMessage;
		const QString repairHint =
			playerControlsRepairHintText(controlsState);
		if (!repairHint.isEmpty()) {
			if (!repairTip.isEmpty())
				repairTip += QStringLiteral("\n");
			repairTip += repairHint;
		}
		fixPlayerControlsBtn->setToolTip(repairTip);
	}
	if (playerControlsRepairHintLabel) {
		playerControlsRepairHintLabel->setVisible(controlsNeedRepair);
		playerControlsRepairHintLabel->setText(
			playerControlsRepairHintText(controlsState));
	}
	if (openPlayerReadyBtn)
		openPlayerReadyBtn->setVisible(showRecordStep);
	if (readyVolumeWidget)
		readyVolumeWidget->setVisible(showRecordStep);
	if (audioBleedWarningLabel)
		audioBleedWarningLabel->setVisible(showRecordStep);
	if (audioBleedNoteLabel)
		audioBleedNoteLabel->setVisible(showRecordStep);
	if (sceneLayoutWidget)
		sceneLayoutWidget->setVisible(showLayoutStep);
	if (reactionSettingsWidget)
		reactionSettingsWidget->setVisible(showReactionStep);
	if (sceneLayoutLabel) {
		sceneLayoutLabel->setText(
			QStringLiteral("Place the YouTube Controls box in OBS Scene Preview."));
		sceneLayoutLabel->setStyleSheet(STYLE_STEP_ACTION_ACCENT);
	}
	if (sceneLayoutNoteLabel) {
		sceneLayoutNoteLabel->setText(
			QStringLiteral("Drag or resize it there if needed. This position is saved with the Sync File."));
		sceneLayoutNoteLabel->setStyleSheet(STYLE_STEP_NOTE);
	}
	if (readyStatusWidget)
		readyStatusWidget->setVisible(showRecordingPanel);

	setSectionPanelState(captureSettingsWidget,
			     (needsSources || controlsNeedRepair) &&
				     !recordingBusy,
			     devicesReady && controlsReady);
	setSectionPanelState(videoStepWidget, false, videoReady);
	setSectionPanelState(sceneLayoutWidget,
			     false, false);
	setSectionPanelState(reactionSettingsWidget, false, false);
	setSectionPanelState(readyStatusWidget, false, false);
	updateReactionSettingsUI();

	if (readySummaryLabel) {
		QString summary;
		const char *style = STYLE_SUBTLE;
		if (videoLoading) {
			summary = QStringLiteral(
				"Loading the video in YouTube Controls...");
		} else if (!devicesReady) {
			summary = QStringLiteral("Set up ") + missingDevices +
				  QStringLiteral(".");
			style = STYLE_ERROR;
		} else if (controlsNeedRepair) {
			summary = controlsBlockMessage;
			style = STYLE_ERROR;
		} else if (!videoLoaded) {
			summary = QStringLiteral("Paste and load a YouTube link.");
		} else if (obsAlreadyRecording) {
			summary = QStringLiteral(
				"Stop OBS recording, then start from Virtual Recorder.");
			style = STYLE_ERROR;
		} else if (!diskOk) {
			summary = QStringLiteral("Free up disk space.");
			style = STYLE_ERROR;
		} else if (showRecordStep && !controlsReady) {
			summary = controlsBlockMessage;
			style = STYLE_ERROR;
		} else if (showRecordStep && !playerControlsOpened) {
			summary = openControlsForSyncText();
		} else if (audioRisk) {
			summary = QStringLiteral(
				"Ready to record. Check audio routing if needed.");
			style = STYLE_WARNING;
		} else if (!videoSettings16x9) {
			summary = QStringLiteral("Ready to record. 16:9 works best.");
			style = STYLE_WARNING;
		} else {
			summary = QStringLiteral("Ready to record. Press Start Recording.");
			style = STYLE_SUCCESS;
		}
		readySummaryLabel->setText(summary);
		readySummaryLabel->setStyleSheet(style);
		readySummaryLabel->setVisible(readyStep || controlsNeedRepair);
	}
	if (readyDetailsBtn)
		readyDetailsBtn->setVisible(readyStep && !controlsNeedRepair);
	if (readyDetailsWidget)
		readyDetailsWidget->setVisible(readyStep && !controlsNeedRepair &&
					       readyDetailsExpanded);
	if (recordFlowLabel)
		recordFlowLabel->setVisible(readyStep && !controlsNeedRepair &&
					    readyDetailsExpanded);

	if (startRecBtn) {
		QString text = QStringLiteral("Start Recording");
		QString tip = QStringLiteral("Start the synced reaction recording.");
		bool buttonEnabled = readyToRecordNow;
		if (readyToRecordNow && !videoSettings16x9)
			tip = QStringLiteral(
				"Start recording. The result will follow the current "
				"non-16:9 OBS video settings.");
		if (recordingStarting) {
			text = QStringLiteral("Starting...");
			tip = QStringLiteral(
				"Preparing combined and reaction recording.");
		} else if (recordingActive) {
			text = QStringLiteral("Recording...");
			tip = QStringLiteral("Recording is already in progress.");
		} else if (stoppingRecording || webcamRecorder.isFinalizing()) {
			text = QStringLiteral("Finalizing...");
			tip = QStringLiteral("Waiting for files to finish processing.");
		} else if (!devicesReady) {
			tip = QStringLiteral(
				"Set up an OBS scene and audio to save.");
		} else if (obsAlreadyRecording) {
			tip = QStringLiteral(
				"Stop the current OBS recording, then start from "
				"Virtual Recorder.");
		} else if (videoLoading) {
			tip = QStringLiteral(
				"Wait for the YouTube video to finish loading.");
		} else if (!videoLoaded) {
			tip = QStringLiteral("Load a YouTube video before recording.");
		} else if (editingVideoUrl) {
			tip = QStringLiteral(
				"Reload the edited link or keep the current video "
				"before recording.");
		} else if (!diskOk) {
			tip = QStringLiteral("Free up disk space before recording.");
		} else if (!controlsReady) {
			tip = controlsBlockMessage;
		} else if (!playerControlsOpened) {
			tip = openControlsForSyncText();
		}
		startRecBtn->setText(text);
		startRecBtn->setToolTip(tip);
		startRecBtn->setStyleSheet(readyToRecordNow ? STYLE_PRIMARY_BTN_ACTIVE :
					    STYLE_PRIMARY_BTN);
		startRecBtn->setEnabled(buttonEnabled);
		startRecBtn->setVisible(readyStep || recordingBusy);
	}
	updateVideoControls();
	updateCompletionActions();
}

void VirtuEditDock::updateVideoControls()
{
	bool recordingBusy = isRecordingFlowBusy();
	bool editable = editingVideoUrl && !videoLoading && !recordingBusy;

	if (urlInput) {
		urlInput->setReadOnly(!editable);
		urlInput->setToolTip(editable ?
					      "Paste or edit the YouTube link." :
					      "Locked after loading so the recorded video "
				      "matches the loaded YouTube video.");
	}

	if (loadVideoBtn) {
		const QString currentUrl =
			urlInput ? urlInput->text().trimmed() : QString();
		const bool sameLoadedVideo = canKeepCurrentLoadedVideo();
		const bool changingLoadedVideo =
			videoLoaded && !editingVideoUrl && !videoLoading;
		const bool canLoad =
			editable && !currentUrl.isEmpty() && !sameLoadedVideo;
		const bool enabled = changingLoadedVideo || canLoad;

		if (videoLoading)
			loadVideoBtn->setText("Loading...");
		else if (changingLoadedVideo)
			loadVideoBtn->setText("Change");
		else
			loadVideoBtn->setText("Load");
		loadVideoBtn->setEnabled(enabled);
		loadVideoBtn->setStyleSheet(changingLoadedVideo ? STYLE_SMALL_BTN :
					    (canLoad ? STYLE_PRIMARY_BTN_ACTIVE :
						       STYLE_PRIMARY_BTN));
		QString loadTip = QStringLiteral("Paste a YouTube link first.");
		if (changingLoadedVideo)
			loadTip = QStringLiteral(
				"Edit the loaded YouTube link.");
		else if (sameLoadedVideo)
			loadTip = QStringLiteral(
				"The current video is already loaded. Press Next to continue.");
		else if (canLoad)
			loadTip = QStringLiteral(
				"Load this link in Virtual Recorder.");
		loadVideoBtn->setToolTip(loadTip);
	}

	if (openPlayerReadyBtn) {
		SceneManager::PlayerControlsState controlsState;
		bool controlsReady = false;
		if (sceneMgr && sceneMgr->getScene()) {
			controlsState = sceneMgr->getPlayerControlsState();
			controlsReady = controlsState.exactlyOneVisible();
		}
		openPlayerReadyBtn->setEnabled(videoLoaded && !videoLoading &&
					       controlsReady);
		openPlayerReadyBtn->setText(
			playerControlsButtonText(playerControlsOpened));
		openPlayerReadyBtn->setStyleSheet(
			playerControlsOpened || !controlsReady ?
				STYLE_SMALL_BTN :
				STYLE_SMALL_BTN_ACTIVE);
		QString tip = QStringLiteral(
			"Available after a YouTube video is loaded.");
		if (!controlsReady)
			tip = playerControlsBlockingMessage(controlsState);
		else if (videoLoaded)
			tip = playerControlsButtonTooltip(playerControlsOpened);
		openPlayerReadyBtn->setToolTip(tip);
	}
}

void VirtuEditDock::updateReactionSettingsUI()
{
	if (!reactionSettingsWidget)
		return;

	const ReactionEncoderCustomSettings custom =
		sceneMgr ? sceneMgr->getReactionEncoderCustomSettings() :
			   ReactionEncoderCustomSettings();
	const ReactionEncoderSettings imported =
		WebcamRecorder::getEncoderSettings(-1, nullptr);
	const ReactionEncoderSettings result =
		WebcamRecorder::getEncoderSettings(
			-1, custom.enabled ? &custom : nullptr);
	const std::string reactionFormat = selectedReactionOutputFormat();
	const std::string obsFormat = obsRecordingOutputFormat();
	const bool formatCustomized =
		sceneMgr && !normalizeReactionOutputFormat(
				     sceneMgr->getReactionOutputFormat())
				     .empty();

	if (reactionSettingsImportedLabel) {
		reactionSettingsImportedLabel->setText(
			QStringLiteral("OBS setting: ") +
			reactionFormatDisplayName(obsFormat) +
			QStringLiteral(", ") +
			reactionEncoderSummaryText(imported));
	}
	if (reactionSettingsResultLabel) {
		reactionSettingsResultLabel->setText(
			QStringLiteral("Reaction output: ") +
			reactionFormatDisplayName(reactionFormat) +
			QStringLiteral(", ") +
			reactionEncoderSummaryText(result));
	}

	if (reactionFormatCombo) {
		QSignalBlocker block(reactionFormatCombo);
		setComboCurrentData(
			reactionFormatCombo,
			QString::fromUtf8(normalizeReactionOutputFormat(
						  reactionFormat)
						  .c_str()));
		reactionFormatCombo->setToolTip(
			"File format for the separate Reaction Video.");
	}

	if (reactionVideoSettingLabel && reactionVideoSettingCombo) {
		QSignalBlocker block(reactionVideoSettingCombo);
		if (imported.rateControl == "CBR") {
			reactionVideoSettingLabel->setText("Video bitrate");
			populateIntegerCombo(
				reactionVideoSettingCombo,
				{2500, 4000, 6000, 8000, 12000, 16000, 20000},
				result.videoBitrate, QString(), QStringLiteral(" kbps"));
			reactionVideoSettingCombo->setToolTip(
				"Higher bitrate can improve quality and create a larger Reaction Video file.");
		} else {
			reactionVideoSettingLabel->setText("Video quality (CRF)");
			populateIntegerCombo(reactionVideoSettingCombo,
					     {18, 20, 21, 23, 26, 28},
					     result.crf, QStringLiteral("CRF "),
					     QString());
			reactionVideoSettingCombo->setToolTip(
				"Lower CRF means higher quality and a larger Reaction Video file.");
		}
	}
	if (reactionAudioBitrateCombo) {
		QSignalBlocker block(reactionAudioBitrateCombo);
		populateIntegerCombo(reactionAudioBitrateCombo,
				     {128, 160, 192, 256, 320},
				     result.audioBitrate, QString(),
				     QStringLiteral(" kbps"));
		reactionAudioBitrateCombo->setToolTip(
			"Audio bitrate for the separate Reaction Video file.");
	}
	if (reactionResetObsBtn) {
		reactionResetObsBtn->setEnabled(custom.enabled ||
						formatCustomized);
		reactionResetObsBtn->setStyleSheet(STYLE_SMALL_BTN);
		reactionResetObsBtn->setToolTip(
			(custom.enabled || formatCustomized) ?
				QStringLiteral("Use your current OBS recording values again.") :
				QStringLiteral("Reaction Video is already using your current OBS recording values."));
	}
}

void VirtuEditDock::onReactionSettingsEdited()
{
	if (!sceneMgr || !reactionVideoSettingCombo || !reactionAudioBitrateCombo)
		return;
	const ReactionEncoderSettings imported =
		WebcamRecorder::getEncoderSettings(-1, nullptr);
	ReactionEncoderCustomSettings custom;
	custom.enabled = true;
	custom.crf = imported.crf;
	custom.videoBitrate = imported.videoBitrate;
	custom.audioBitrate = currentIntegerComboValue(reactionAudioBitrateCombo);
	if (imported.rateControl == "CBR")
		custom.videoBitrate =
			currentIntegerComboValue(reactionVideoSettingCombo);
	else
		custom.crf = currentIntegerComboValue(reactionVideoSettingCombo);

	sceneMgr->saveReactionEncoderCustomSettings(custom);
	updateReactionSettingsUI();
}

void VirtuEditDock::onReactionFormatChanged()
{
	if (!sceneMgr || !reactionFormatCombo)
		return;

	const std::string format =
		qPathToStd(reactionFormatCombo->currentData().toString());
	sceneMgr->saveReactionOutputFormat(format);
	updateReactionSettingsUI();
}

void VirtuEditDock::onReactionSettingsReset()
{
	if (!sceneMgr)
		return;
	sceneMgr->clearReactionEncoderCustomSettings();
	sceneMgr->saveReactionOutputFormat("");
	updateReactionSettingsUI();
}

void VirtuEditDock::updateResponsiveLayout()
{
	if (nextTakeLayout) {
		nextTakeLayout->setDirection(QBoxLayout::TopToBottom);
		nextTakeLayout->setSpacing(6);
	}

	if (recordAnotherBtn)
		recordAnotherBtn->setSizePolicy(QSizePolicy::Expanding,
						QSizePolicy::Fixed);
	if (newVideoBtn)
		newVideoBtn->setSizePolicy(QSizePolicy::Expanding,
					   QSizePolicy::Fixed);
}

static int recordingStateCount(bool starting, bool active, bool stopping)
{
	return (starting ? 1 : 0) + (active ? 1 : 0) + (stopping ? 1 : 0);
}

static const char *recordingStateName(bool starting, bool active,
				      bool stopping)
{
	if (recordingStateCount(starting, active, stopping) > 1)
		return "invalid";
	if (starting)
		return "starting";
	if (active)
		return "active";
	if (stopping)
		return "stopping";
	return "idle";
}

void VirtuEditDock::setRecordingState(bool starting, bool active,
				      bool stopping, const char *context)
{
	const char *oldState = recordingStateName(
		recordingStarting, recordingActive, stoppingRecording);
	const bool oldStarting = recordingStarting;
	const bool oldActive = recordingActive;
	const bool oldStopping = stoppingRecording;
	const int requestedCount =
		recordingStateCount(starting, active, stopping);
	if (requestedCount > 1) {
		blog(LOG_WARNING,
		     "[VirtuEdit] Invalid recording state requested%s%s: starting=%d active=%d stopping=%d",
		     context ? " from " : "", context ? context : "",
		     starting ? 1 : 0, active ? 1 : 0, stopping ? 1 : 0);
		if (stopping) {
			starting = false;
			active = false;
		} else if (active) {
			starting = false;
		}
	}

	recordingStarting = starting;
	recordingActive = active;
	stoppingRecording = stopping;
	updateReleaseNoticeVisibility();
	const char *newState = recordingStateName(
		recordingStarting, recordingActive, stoppingRecording);
	if (oldStarting != starting || oldActive != active ||
	    oldStopping != stopping) {
		if (context) {
			blog(LOG_INFO,
			     "[VirtuEdit] Recording state: %s -> %s (%s)",
			     oldState, newState, context);
		} else {
			blog(LOG_INFO, "[VirtuEdit] Recording state: %s -> %s",
			     oldState, newState);
		}
	}
	if ((starting || active || stopping) && recordingSceneUuid.empty()) {
		if (context) {
			blog(LOG_WARNING,
			     "[VirtuEdit] Recording state %s has no locked OBS scene (%s)",
			     newState, context);
		} else {
			blog(LOG_WARNING,
			     "[VirtuEdit] Recording state %s has no locked OBS scene",
			     newState);
		}
	}
	if (!starting && !active && !stopping) {
		recordingSceneUuid.clear();
		recordingSceneName.clear();
		restoringRecordingScene = false;
	}
	updateCompletionActions();
}

void VirtuEditDock::restoreRecordingSceneState()
{
	if (!sceneMgr)
		return;
	sceneMgr->lockSceneItems(false);
	sceneMgr->restoreAudioTracks();
}

void VirtuEditDock::clearRecordingOutputState()
{
	lastRecordingPath.clear();
	lastReactBase.clear();
	lastReactExpectedPath.clear();
	lastReactionOutputFormat.clear();
	lastReactPath.clear();
	lastJsonPath.clear();
	hasLastOverlay = false;
	waitingForComposite = false;
	compositePathReported = false;
	reactFinalizeFailed = false;
	reactOutputNeedsAttention = false;
	jsonOutputNeedsAttention = false;
	compositeOutputNeedsAttention = false;
	if (syncCountLabel)
		syncCountLabel->setText(syncTimingCountText(0));
	updateCompletionActions();
}

bool VirtuEditDock::isRecordingFlowBusy() const
{
	return recordingStarting || recordingActive || stoppingRecording ||
	       webcamRecorder.isFinalizing();
}

bool VirtuEditDock::isCompletionProcessing() const
{
	return isRecordingFlowBusy() || waitingForComposite;
}

void VirtuEditDock::updateCompletionActions()
{
	bool locked = isCompletionProcessing();
	bool needsAttention = reactOutputNeedsAttention ||
			      jsonOutputNeedsAttention ||
			      compositeOutputNeedsAttention;
	QString lockedTip = QStringLiteral(
		"Available after all recording files finish processing.");
	if (completeTitleLabel) {
		if (locked) {
			completeTitleLabel->setText("Finalizing Files");
			completeTitleLabel->setStyleSheet(STYLE_BRAND_TITLE);
		} else if (needsAttention) {
			completeTitleLabel->setText(
				"Recording Complete - Check Files");
			completeTitleLabel->setStyleSheet(
				STYLE_COMPLETE_WARNING_TITLE);
		} else {
			completeTitleLabel->setText("Recording Complete");
			completeTitleLabel->setStyleSheet(STYLE_COMPLETE_TITLE);
		}
	}
	if (recordAnotherBtn) {
			const bool sameVideoAvailable = !locked && videoLoaded;
			recordAnotherBtn->setEnabled(sameVideoAvailable);
			recordAnotherBtn->setStyleSheet(sameVideoAvailable ?
								STYLE_PRIMARY_BTN_ACTIVE :
								STYLE_FLAT_BTN);
		if (locked) {
			recordAnotherBtn->setToolTip(lockedTip);
		} else if (!videoLoaded) {
			recordAnotherBtn->setToolTip(
				"Available when the same YouTube video is still loaded.");
		} else {
			recordAnotherBtn->setToolTip(
				"Use the same loaded YouTube link. You will return to Position & Size before recording again.");
		}
	}
		if (newVideoBtn) {
			newVideoBtn->setEnabled(!locked);
			newVideoBtn->setStyleSheet(STYLE_FLAT_BTN);
			newVideoBtn->setToolTip(
				locked ? lockedTip
				       : QStringLiteral(
						 "Clear the current YouTube link and prepare a different video."));
		}
	}

void VirtuEditDock::clearCompletionOutputState()
{
	clearRecordingOutputState();
	setReactOutputStatus("", STYLE_COMPLETE_STATUS_SUBTLE,
			     CompletionAttention::Normal);
	setJsonOutputStatus("", STYLE_COMPLETE_STATUS_SUBTLE,
			    CompletionAttention::Normal);
	setCompositeOutputStatus("", STYLE_COMPLETE_STATUS_SUBTLE,
				 CompletionAttention::Normal);
	updateOpenFolderButton();
}

void VirtuEditDock::stopActiveOutputsForCleanup()
{
	if (webcamRecorder.isActive())
		webcamRecorder.stop("mkv", nullptr);
	if (obs_frontend_recording_active())
		obs_frontend_recording_stop();
}

int VirtuEditDock::beginRecordingSession()
{
	if (isCompletionProcessing()) {
		blog(LOG_WARNING,
		     "[VirtuEdit] Starting a new recording session while previous processing is still active");
	}
	lastRecordingPath.clear();
	return ++recordingSessionId;
}

bool VirtuEditDock::isCurrentRecordingSession(int session) const
{
	return session == recordingSessionId;
}

void VirtuEditDock::scheduleCompositeFinalizeWatchdogs(int session)
{
	QTimer::singleShot(15000, this, [this, session]() {
		if (!isCurrentRecordingSession(session) ||
		    !waitingForComposite || !compositeStatusLabel ||
		    pages->currentWidget() != completePage)
			return;
		setCompositeOutputStatus(completeStatusFinalizingText(),
					 STYLE_COMPLETE_STATUS_SUBTLE,
					 CompletionAttention::Normal,
					 compositeFinalizingTooltipText());
	});

	QTimer::singleShot(60000, this, [this, session]() {
		if (!isCurrentRecordingSession(session) ||
		    !waitingForComposite || compositePathReported ||
		    pages->currentWidget() != completePage)
			return;
		blog(LOG_WARNING,
		     "[VirtuEdit] Composite recording path was not reported before timeout");
		onCompositeRecordingReady("");
	});
}

void VirtuEditDock::setReactOutputStatus(const QString &text,
					 const char *style,
					 CompletionAttention attention,
					 const QString &detailTooltip)
{
	reactOutputNeedsAttention =
		attention == CompletionAttention::NeedsAttention;
	if (reactStatusLabel) {
		reactStatusLabel->setText(text);
		reactStatusLabel->setStyleSheet(style);
		QString tooltip = detailTooltip;
		if (fileLooksSaved(lastReactPath)) {
			if (!tooltip.isEmpty())
				tooltip += QStringLiteral("\n");
			tooltip += stdPathToQString(lastReactPath);
		}
		reactStatusLabel->setToolTip(tooltip);
	}
	updateCompletionActions();
}

void VirtuEditDock::setJsonOutputStatus(const QString &text, const char *style,
					CompletionAttention attention,
					const QString &detailTooltip)
{
	jsonOutputNeedsAttention =
		attention == CompletionAttention::NeedsAttention;
	if (jsonStatusLabel) {
		jsonStatusLabel->setText(text);
		jsonStatusLabel->setStyleSheet(style);
		QString tooltip = detailTooltip;
		if (fileLooksSaved(lastJsonPath)) {
			if (!tooltip.isEmpty())
				tooltip += QStringLiteral("\n");
			tooltip += stdPathToQString(lastJsonPath);
		}
		jsonStatusLabel->setToolTip(tooltip);
	}
	updateCompletionActions();
}

void VirtuEditDock::setCompositeOutputStatus(const QString &text,
					     const char *style,
					     CompletionAttention attention,
					     const QString &detailTooltip)
{
	compositeOutputNeedsAttention =
		attention == CompletionAttention::NeedsAttention;
	if (compositeStatusLabel) {
		compositeStatusLabel->setText(text);
		compositeStatusLabel->setStyleSheet(style);
		QString tooltip = detailTooltip;
		if (fileLooksSaved(lastRecordingPath)) {
			if (!tooltip.isEmpty())
				tooltip += QStringLiteral("\n");
			tooltip += stdPathToQString(lastRecordingPath);
		}
		compositeStatusLabel->setToolTip(tooltip);
	}
	updateCompletionActions();
}

void VirtuEditDock::updateOpenFolderButton()
{
	if (!openFolderBtn)
		return;
	const std::string folder = currentOutputFolder();
	const bool enabled = !folder.empty();
	openFolderBtn->setEnabled(enabled);
	openFolderBtn->setStyleSheet(enabled ? STYLE_PRIMARY_BTN_ACTIVE :
						   STYLE_PRIMARY_BTN);
	openFolderBtn->setToolTip(
		enabled ? QStringLiteral("Open recording folder:\n") +
				  stdPathToQString(folder)
			: QStringLiteral(
				  "Available after a recording folder is created."));
}

void VirtuEditDock::onUrlSubmit()
{
	if (isRecordingFlowBusy() || waitingForComposite) {
		updateReadiness();
		return;
	}

	if (videoLoading)
		return;

	if (videoLoaded && !editingVideoUrl) {
		editingVideoUrl = true;
		fallbackVideoUrl.clear();
		fallbackVideoId.clear();
		videoInfoLabel->setText(
			"Paste a new YouTube link, or keep the current video.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		updateReadiness();
		urlInput->setFocus();
		urlInput->selectAll();
		return;
	}

	bool hadLoadedVideo = videoLoaded;
	QString url = urlInput->text().trimmed();
	if (url.isEmpty()) {
		videoInfoLabel->setText(
			"Paste a YouTube link here, then press Load.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		editingVideoUrl = true;
		updateReadiness();
		return;
	}

	std::string videoId = extractVideoId(url);
	if (videoId.empty()) {
		videoInfoLabel->setText(
			"Paste a YouTube watch, Shorts, Live, embed, or youtu.be link.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		videoLoading = false;
		videoLoaded = hadLoadedVideo;
		editingVideoUrl = true;
		updateReadiness();
		return;
	}

	if (!ensureYouTubePlayerReady(videoId)) {
		videoInfoLabel->setText(
			"Couldn't prepare YouTube Controls. Restart OBS and try again.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		videoLoading = false;
		videoLoaded = hadLoadedVideo;
		editingVideoUrl = true;
		updateReadiness();
		return;
	}

	videoInfoLabel->setText("Loading in YouTube Controls...");
	videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	if (hadLoadedVideo && !loadedVideoId.empty() &&
	    videoId != loadedVideoId) {
		fallbackVideoUrl = loadedVideoUrl;
		fallbackVideoId = loadedVideoId;
	} else {
		fallbackVideoUrl.clear();
		fallbackVideoId.clear();
	}
	pendingVideoUrl = url;
	pendingVideoId = videoId;
	videoLoading = true;
	videoLoaded = false;
	editingVideoUrl = false;
	playerControlsOpened = false;
	updateReadiness();

	yt->loadVideo(videoId);
	openPlayerControlsWindow(false);
	loadTimer->start(10000); // 10s watchdog

	blog(LOG_INFO, "[VirtuEdit] Loading video: %s", videoId.c_str());
}

void VirtuEditDock::onCancelVideoChange()
{
	if (isRecordingFlowBusy() || waitingForComposite) {
		updateReadiness();
		return;
	}

	if (!videoLoaded)
		return;

	editingVideoUrl = false;
	pendingVideoUrl.clear();
	pendingVideoId.clear();
	fallbackVideoUrl.clear();
	fallbackVideoId.clear();
	if (!loadedVideoUrl.isEmpty())
		urlInput->setText(loadedVideoUrl);
	videoInfoLabel->setText(currentVideoSummary());
	videoInfoLabel->setStyleSheet(STYLE_SUCCESS_STATUS);
	updateReadiness();
}

void VirtuEditDock::onVideoError(int code)
{
	if (videoLoaded)
		return; // already playing; ignore late/benign errors
	loadTimer->stop();

	QString msg;
	switch (code) {
	case 101:
	case 150:
		msg = QStringLiteral(
			"This video blocks embedded playback. Use another "
			"YouTube video so Virtual Recorder can save sync timing.");
		break;
	case 100:
		msg = QStringLiteral(
			"This video is private, removed, or unavailable. Try a "
			"public YouTube video.");
		break;
	case 2:
		msg = QStringLiteral(
			"Invalid video link. Paste a YouTube watch, Shorts, Live, embed, or youtu.be URL.");
		break;
	default:
		msg = QStringLiteral(
			"Couldn't load this video. Check the link or network, "
			"then try again.");
		break;
	}
	if (restorePreviousVideoAfterLoadFailure(msg))
		return;
	videoInfoLabel->setText(msg);
	videoInfoLabel->setStyleSheet(STYLE_ERROR);
	pendingVideoUrl.clear();
	pendingVideoId.clear();
	fallbackVideoUrl.clear();
	fallbackVideoId.clear();
	videoLoading = false;
	videoLoaded = false;
	editingVideoUrl = true;
	playerControlsOpened = false;
	updateReadiness();
}

void VirtuEditDock::onLoadTimeout()
{
	if (videoLoaded)
		return;
	videoInfoLabel->setText(
		"Video did not report ready. Try loading again, or use a "
		"YouTube video that allows embedded playback.");
	if (restorePreviousVideoAfterLoadFailure(
		    "Video did not report ready."))
		return;
	videoInfoLabel->setStyleSheet(STYLE_ERROR);
	pendingVideoUrl.clear();
	pendingVideoId.clear();
	fallbackVideoUrl.clear();
	fallbackVideoId.clear();
	videoLoading = false;
	videoLoaded = false;
	editingVideoUrl = true;
	playerControlsOpened = false;
	updateReadiness();
}

void VirtuEditDock::onOpenProjector()
{
	openPlayerControlsWindow(true);
}

bool VirtuEditDock::openPlayerControlsWindow(bool requireVideoLoaded)
{
	if (requireVideoLoaded && !videoLoaded) {
		videoInfoLabel->setText(
			"Load a YouTube video first. The YouTube Controls window opens automatically while loading.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		updateReadiness();
		return false;
	}

	if (!sceneMgr || !sceneMgr->getScene()) {
		videoInfoLabel->setText(
			"Set up an OBS scene before opening YouTube Controls.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return false;
	}

	SceneManager::PlayerControlsState controlsState =
		sceneMgr->getPlayerControlsState();
	if (!controlsState.exactlyOneVisible()) {
		videoInfoLabel->setText(
			playerControlsBlockingMessage(controlsState));
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return false;
	}

	obs_source_t *browserSrc = yt->getBrowserSource();
	if (!browserSrc) {
		videoInfoLabel->setText(
			"Complete setup before opening YouTube Controls.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return false;
	}
	if (videoLoaded)
		yt->repairBrowserSourceSettings(loadedVideoId);

	if (!sceneMgr->isPlayerControlsSizeInitialized() &&
	    initializePlayerControlsWindowSize(this)) {
		sceneMgr->setPlayerControlsSizeInitialized(true);
	}

#ifdef _WIN32
	const char *browserSourceName = obs_source_get_name(browserSrc);
	const QString interactionSourceName =
		QString::fromUtf8(browserSourceName ? browserSourceName : "");
#endif

	obs_frontend_open_source_interaction(browserSrc);

#ifdef _WIN32
	QTimer::singleShot(100, this, [interactionSourceName]() {
		enableInteractionWindowMaximizeButton(interactionSourceName);
	});
	QTimer::singleShot(500, this, [interactionSourceName]() {
		enableInteractionWindowMaximizeButton(interactionSourceName);
	});
#endif

	playerControlsOpened = true;
	updateReadiness();
	return true;
}

void VirtuEditDock::onFixPlayerControls()
{
	if (isRecordingFlowBusy() || waitingForComposite) {
		updateReadiness();
		return;
	}

	if (!sceneMgr->getScene() && !sceneMgr->selectReactionScene("")) {
		videoInfoLabel->setText(
			"Couldn't set up this OBS scene. Check Step 1 and try again.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}

	if (!yt->getBrowserSource())
		yt->createBrowserSource(loadedVideoId);
	obs_source_t *browserSrc = yt->getBrowserSource();
	if (!browserSrc || !sceneMgr->resetPlayerControls(browserSrc)) {
		videoInfoLabel->setText(
			"Couldn't fix YouTube Controls. Restart OBS, then try again.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}

	sceneMgr->activateScene();
	playerControlsOpened = false;
	if (videoLoaded) {
		readyWorkflowStep = READY_STEP_VIDEO;
		videoInfoLabel->setText(
			currentVideoSummary());
		videoInfoLabel->setStyleSheet(STYLE_SUCCESS_STATUS);
	} else {
		readyWorkflowStep = READY_STEP_VIDEO;
		videoInfoLabel->setText(
			"YouTube Controls added to this OBS scene. Load a YouTube video next.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	}
	updateReadiness();
}

void VirtuEditDock::onStartRecording()
{
	if (isCompletionProcessing()) {
		updateReadiness();
		return;
	}

	std::vector<std::string> currentAudioIds =
		sceneMgr ? sceneMgr->getCurrentReactionAudioSourceIds()
			 : std::vector<std::string>();
	bool devicesReady =
		sceneMgr && sceneMgr->getScene() &&
		audioSourceIdsAvailable(currentAudioIds,
					SceneManager::listCurrentSceneAudioSources());
	if (!devicesReady) {
		onOpenSettings();
		return;
	}

	if (!videoLoaded || videoLoading || editingVideoUrl ||
	    recordingStarting || recordingActive || stoppingRecording ||
	    webcamRecorder.isFinalizing()) {
		updateReadiness();
		return;
	}
	SceneManager::PlayerControlsState controlsState =
		sceneMgr->getPlayerControlsState();
	if (!controlsState.exactlyOneVisible()) {
		videoInfoLabel->setText(
			playerControlsBlockingMessage(controlsState));
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}
	if (yt->repairBrowserSourceSettings(loadedVideoId)) {
		playerControlsOpened = false;
		videoInfoLabel->setText(
			"YouTube Controls were repaired. Open the window again before recording.");
		videoInfoLabel->setStyleSheet(STYLE_WARNING);
		updateReadiness();
		return;
	}
	if (!playerControlsOpened) {
		videoInfoLabel->setText(openControlsForSyncText());
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
		updateReadiness();
		return;
	}
	if (startRecBtn)
		startRecBtn->setEnabled(false);

	// Guard: OBS already recording (manually). Our flow owns the composite
	// recording, so refuse rather than fight an in-progress capture.
	if (obs_frontend_recording_active()) {
		videoInfoLabel->setText(
			"OBS is already recording. Stop OBS recording, then start "
			"from Virtual Recorder so Reaction Video and sync timing start together.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}

	std::string dir = getRecordingOutputDir();
	if (dir.empty()) {
		videoInfoLabel->setText(
			"Virtual Recorder needs OBS Advanced Output > Custom Output "
			"(FFmpeg) to write to a file. URL-only output cannot create a Combined Video.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}

	// Guard: critically low disk space — recording would fail mid-capture.
	double freeGB = freeDiskGB(dir);
	if (freeGB >= 0 && freeGB < 1.0) {
		char buf[160];
		snprintf(buf, sizeof(buf),
			 "Low disk space (%.1f GB free). Free up space "
			 "before recording.",
			 freeGB);
		videoInfoLabel->setText(buf);
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}
	if (freeGB >= 0 && freeGB < 5.0)
		blog(LOG_WARNING,
		     "[VirtuEdit] Low disk space: %.1f GB free", freeGB);

	DeviceInfo currentScene = SceneManager::currentFrontendScene();
	const std::string configuredSceneUuid = sceneMgr ?
							sourceUuidFromStoredId(
								sceneMgr->getCurrentSceneId()) :
							std::string();
	if (currentScene.id.empty()) {
		videoInfoLabel->setText(
			"No OBS scene selected. Select an OBS scene before recording.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}
	if (!configuredSceneUuid.empty() &&
	    currentScene.id != configuredSceneUuid) {
		if (sceneMgr->selectReactionScene(currentScene.id)) {
			playerControlsOpened = false;
			readyWorkflowStep = READY_STEP_VIDEO;
			SceneManager::PlayerControlsState controlsState =
				sceneMgr->getPlayerControlsState();
			videoInfoLabel->setText(
				sceneChangedGuidanceText(controlsState));
			videoInfoLabel->setStyleSheet(STYLE_ERROR);
			updateReadiness();
			blog(LOG_WARNING,
			     "[VirtuEdit] Recording start blocked after OBS scene changed: %s",
			     currentScene.name.c_str());
			return;
		}

		videoInfoLabel->setText(
			"Couldn't use the current OBS scene. Check OBS scenes, then start again.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		return;
	}

	recordingSceneUuid =
		configuredSceneUuid.empty() ? currentScene.id : configuredSceneUuid;
	std::string configuredSceneName = sceneMgr->getCurrentSceneName();
	recordingSceneName = currentScene.name.empty() ?
				     QString::fromUtf8(configuredSceneName.c_str()) :
				     QString::fromUtf8(currentScene.name.c_str());

	sceneMgr->activateScene();
	sceneMgr->lockSceneItems(true);
	if (!sceneMgr->setupAudioTracks()) {
		restoreRecordingSceneState();
		const std::string setupError = sceneMgr->getLastAudioSetupError();
		videoInfoLabel->setText(
			setupError.empty()
				? QStringLiteral(
					  "Couldn't prepare the selected audio. Check the selected OBS audio sources, then start again.")
				: QString::fromUtf8(setupError.c_str(),
						    static_cast<int>(setupError.size())));
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		blog(LOG_ERROR,
		     "[VirtuEdit] Recording aborted: audio track setup failed");
		return;
	}

	int reactionAudioTrack = sceneMgr->getSeparatedTrackIndex();
	if (!sceneMgr->getScene() || reactionAudioTrack < 0) {
		restoreRecordingSceneState();
		videoInfoLabel->setText(
			"Couldn't prepare reaction-only recording. Check the "
			"OBS scene and selected audio sources, then start again.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		blog(LOG_ERROR,
		     "[VirtuEdit] Recording aborted: recorder inputs missing");
		return;
	}

	QDateTime now = QDateTime::currentDateTime();
	QString dateCode = now.toString("yyMMdd");
	const auto &info = yt->getInfo();
	const QString slug =
		recordingTitleSlug(info.title, yt->getCurrentVideoId());
	QString recordingBase =
		createRecordingSessionBase(dir, dateCode, slug);
	if (recordingBase.isEmpty()) {
		restoreRecordingSceneState();
		videoInfoLabel->setText(
			"Couldn't create the recording folder. Check the OBS "
			"recording path, then start again.");
		videoInfoLabel->setStyleSheet(STYLE_ERROR);
		updateReadiness();
		blog(LOG_ERROR,
		     "[VirtuEdit] Recording aborted: failed to create session folder");
		return;
	}
	lastReactBase = qPathToStd(recordingBase);
	lastReactionOutputFormat = selectedReactionOutputFormat();
	lastReactExpectedPath = reactionOutputPathForBase(
		lastReactBase, lastReactionOutputFormat);

	int session = beginRecordingSession();
	setRecordingState(true, false, false, "onStartRecording");
	videoInfoLabel->setText("Starting recording...");
	videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	obs_frontend_recording_start();
	QTimer::singleShot(5000, this, [this, session]() {
		if (!isCurrentRecordingSession(session) || !recordingStarting)
			return;
		if (obs_frontend_recording_active()) {
			onCompositeRecordingStarted();
			return;
		}
		abortRecordingStart(
			"Couldn't start OBS combined recording. Check OBS output "
			"settings and recording path, then start again.");
	});
}

void VirtuEditDock::onCompositeRecordingStarted()
{
	QMetaObject::invokeMethod(
		this,
		[this]() {
			updateReleaseNoticeVisibility();
			if (!recordingStarting) {
				blog(LOG_INFO,
				     "[VirtuEdit] Ignored OBS recording started event outside Virtual Recorder start state");
				return;
			}
			setRecordingState(false, true, false,
					  "onCompositeRecordingStarted");

			int reactionAudioTrack =
				sceneMgr->getSeparatedTrackIndex();
			ReactionEncoderCustomSettings reactionCustom =
				sceneMgr->getReactionEncoderCustomSettings();
			const ReactionEncoderCustomSettings *reactionCustomPtr =
				reactionCustom.enabled ? &reactionCustom : nullptr;
			std::string reactPath = reactionMkvPathForBase(lastReactBase);
			if (!sceneMgr->getScene() || reactionAudioTrack < 0 ||
			    !webcamRecorder.start(sceneMgr->getScene(),
						  yt->getBrowserSource(),
						  reactPath,
						  reactionAudioTrack,
						  reactionCustomPtr)) {
				abortRecordingStart(
					"Couldn't start reaction-only recording. Check "
					"the OBS scene, encoder, and disk settings, then "
					"start again.");
				blog(LOG_ERROR,
				     "[VirtuEdit] Recording aborted: reaction recorder failed to start");
				return;
			}

			yt->startRecording();

			recStartMs = QDateTime::currentMSecsSinceEpoch();
			recTimer->start(200);
			if (playbackLabel) {
				playbackLabel->setText("-- 00:00 / 00:00");
				playbackLabel->setStyleSheet(STYLE_BODY_LABEL);
			}
			syncCountLabel->setText(syncTimingCountText(0));
			if (stopRecBtn) {
				stopRecBtn->setText("Stop Recording");
				stopRecBtn->setEnabled(true);
			}

			pages->setCurrentWidget(recordingPage);

			blog(LOG_INFO, "[VirtuEdit] Recording started");
		},
		Qt::QueuedConnection);
}

void VirtuEditDock::abortRecordingStart(const QString &message)
{
	setRecordingState(false, false, false, "abortRecordingStart");
	stopActiveOutputsForCleanup();
	restoreRecordingSceneState();
	videoInfoLabel->setText(message);
	videoInfoLabel->setStyleSheet(STYLE_ERROR);
	clearRecordingOutputState();
	updateReadiness();

	blog(LOG_ERROR, "[VirtuEdit] Recording aborted: composite start failed");
}

void VirtuEditDock::setVolumeUI(int value)
{
	QString txt = QString::number(value) + "%";
	if (volSliderReady && volSliderReady->value() != value) {
		volSliderReady->blockSignals(true);
		volSliderReady->setValue(value);
		volSliderReady->blockSignals(false);
	}
	if (volSliderRec && volSliderRec->value() != value) {
		volSliderRec->blockSignals(true);
		volSliderRec->setValue(value);
		volSliderRec->blockSignals(false);
	}
	if (volValReady)
		volValReady->setText(txt);
	if (volValRec)
		volValRec->setText(txt);
}

// User moved a dock slider -> push to the synced player.
void VirtuEditDock::onVolumeChanged(int value)
{
	relativeVolume = value;
	yt->setVolume(value);
	setVolumeUI(value);
}

// Volume changed inside the synced player -> reflect it back in the dock + JSON,
// unless the user is mid-drag.
void VirtuEditDock::onPlayerVolumeChanged(int value)
{
	if (volumeDragging)
		return;
	if (value == relativeVolume)
		return;
	relativeVolume = value;
	setVolumeUI(value);
	sceneMgr->saveVolume(value);
}

void VirtuEditDock::onStopRecording()
{
	if (!recordingActive || stoppingRecording) {
		blog(LOG_INFO,
		     "[VirtuEdit] Ignored stop request: active=%d stopping=%d",
		     recordingActive ? 1 : 0, stoppingRecording ? 1 : 0);
		return;
	}
	setRecordingState(false, false, true, "onStopRecording");
	if (stopRecBtn) {
		stopRecBtn->setText("Finalizing...");
		stopRecBtn->setEnabled(false);
	}
	if (playbackLabel) {
		playbackLabel->setText("Finalizing files...");
		playbackLabel->setStyleSheet(STYLE_SUBTLE);
	}

	int session = recordingSessionId;
	yt->stopRecording();
	QTimer::singleShot(350, this, [this, session]() {
		if (!isCurrentRecordingSession(session) ||
		    pages->currentWidget() != completePage)
			return;
		saveSyncJson();
	});
	recTimer->stop();

	lastOverlay = sceneMgr->readOverlayPosition();
	hasLastOverlay = true;
	lastRecordingPath.clear();
	waitingForComposite = true;
	compositePathReported = false;

	// Stop reaction-only recording (async: waits for finalize, then
	// remuxes to the selected format). UI updates via the callback below.
	lastReactPath = "";
	reactFinalizeFailed = false;
	const std::string reactionFormat = lastReactionOutputFormat.empty()
						  ? std::string("mp4")
						  : lastReactionOutputFormat;
	webcamRecorder.stop(reactionFormat,
			    [this, session](bool ok, const std::string &finalPath) {
		QMetaObject::invokeMethod(
			this,
			[this, session, ok, finalPath]() {
				if (!isCurrentRecordingSession(session)) {
					blog(LOG_INFO,
					     "[VirtuEdit] Ignored stale reaction finalize callback for session %d (current %d)",
					     session, recordingSessionId);
					updateReadiness();
					return;
				}
				const std::string savedReactPath = existingPath(
					existingPath(finalPath, lastReactExpectedPath),
					reactionMkvPathForBase(lastReactBase));
				if ((ok && fileLooksSaved(finalPath)) ||
				    !savedReactPath.empty()) {
					reactFinalizeFailed = false;
					lastReactPath = !savedReactPath.empty()
								? savedReactPath
								: finalPath;
					if (!ok) {
						blog(LOG_WARNING,
						     "[VirtuEdit] React finalize reported failure, but output file exists: %s",
						     lastReactPath.c_str());
					}
					saveSyncJson();
					const std::string savedFormat =
						fileContainerFormat(lastReactPath);
					const bool savedRequestedFormat =
						normalizeReactionOutputFormat(savedFormat) ==
						normalizeReactionOutputFormat(
							lastReactionOutputFormat);
					if (savedRequestedFormat) {
						setReactOutputStatus(
							completeStatusSavedForFormatText(
								savedFormat),
							STYLE_COMPLETE_STATUS_SUCCESS,
							CompletionAttention::Normal);
					} else {
						setReactOutputStatus(
							completeStatusSavedAsMkvText(),
							STYLE_COMPLETE_STATUS_WARNING,
							CompletionAttention::Normal,
							reactionSavedAsMkvTooltipText());
					}
				} else {
					reactFinalizeFailed = true;
					setReactOutputStatus(
						completeStatusFailedText(),
						STYLE_COMPLETE_STATUS_ERROR,
						CompletionAttention::NeedsAttention,
						reactionFailedTooltipText());
					const std::string expectedJson =
						lastReactBase.empty()
							? std::string()
							: lastReactBase + "_sync.json";
					const std::string savedJsonPath =
						existingPath(lastJsonPath, expectedJson);
					if (!savedJsonPath.empty()) {
						lastJsonPath = savedJsonPath;
						saveSyncJson();
					} else {
						setJsonOutputStatus(
							completeStatusNotSavedText(),
							STYLE_COMPLETE_STATUS_ERROR,
							CompletionAttention::NeedsAttention,
							syncBlockedByReactionFailedTooltipText());
					}
				}
				updateOpenFolderButton();
				updateReadiness();
			},
			Qt::QueuedConnection);
	});

	if (obs_frontend_recording_active())
		obs_frontend_recording_stop();
	else
		onCompositeRecordingReady("");

	restoreRecordingSceneState();

	const auto &info = yt->getInfo();
	saveSyncJson();

	QString folderSrc = !lastReactBase.empty()
				    ? stdPathToQString(lastReactBase)
				    : stdPathToQString(lastRecordingPath);
	QString folderText = QStringLiteral("Saving files...");
	if (!folderSrc.isEmpty()) {
		QString folder = QFileInfo(folderSrc).absolutePath();
		folderText = QStringLiteral("Saved to:\n") + folder;
	}
	completeInfoLabel->setText(folderText);
	updateOpenFolderButton();
	if (completeSyncLabel) {
		QString title = QString::fromUtf8(info.title.c_str());
		if (title.isEmpty())
			title = QStringLiteral("Loaded YouTube video");
		completeSyncLabel->setText(QStringLiteral("YouTube: ") + title +
					   QStringLiteral(" - ") +
					   formatTime(info.duration));
	}
	// React video is being finalized/remuxed in the background.
	// The reaction recorder callback updates this label when done.
	setReactOutputStatus(completeStatusFinalizingText(),
			     STYLE_COMPLETE_STATUS_SUBTLE,
			     CompletionAttention::Normal,
			     reactFinalizingTooltipText());
	setCompositeOutputStatus(completeStatusFinalizingText(),
				 STYLE_COMPLETE_STATUS_SUBTLE,
				 CompletionAttention::Normal,
				 compositeFinalizingTooltipText());
	scheduleCompositeFinalizeWatchdogs(recordingSessionId);

	pages->setCurrentWidget(completePage);

	blog(LOG_INFO, "[VirtuEdit] Recording stopped");
}

void VirtuEditDock::saveSyncJson()
{
	if (yt && yt->isRecording()) {
		setJsonOutputStatus(completeStatusFinalizingText(),
				    STYLE_COMPLETE_STATUS_SUBTLE,
				    CompletionAttention::Normal,
				    syncFinalizingTooltipText());
		return;
	}

	if (lastReactPath.empty() && fileLooksSaved(lastReactExpectedPath)) {
		lastReactPath = lastReactExpectedPath;
		reactFinalizeFailed = false;
		setReactOutputStatus(completeStatusSavedForFormatText(
					     fileContainerFormat(lastReactPath)),
				     STYLE_COMPLETE_STATUS_SUCCESS,
				     CompletionAttention::Normal);
	}

	if (!hasLastOverlay)
		lastOverlay = sceneMgr->readOverlayPosition();

	const auto &info = yt->getInfo();
	const std::vector<SyncPoint> syncPoints = yt->getSyncPoints();
	const std::string reactionPathForJson =
		lastReactPath.empty() ? lastReactExpectedPath : lastReactPath;
	std::string json = JsonExport::buildJson(
		yt->getCurrentVideoId(), info.title, info.duration,
		basename(reactionPathForJson), basename(lastRecordingPath),
		syncPoints, lastOverlay, relativeVolume);

	lastJsonPath = "";
	std::string targetJsonPath;
	if (!lastReactBase.empty()) {
		targetJsonPath = lastReactBase + "_sync.json";
		lastJsonPath = JsonExport::saveToPath(targetJsonPath, json);
	} else if (!lastRecordingPath.empty()) {
		lastJsonPath = JsonExport::saveNextToRecording(
			lastRecordingPath, json);
	}
	if (!fileLooksSaved(lastJsonPath))
		lastJsonPath.clear();
	if (lastJsonPath.empty() && fileLooksSaved(targetJsonPath))
		lastJsonPath = targetJsonPath;

	const int count = static_cast<int>(syncPoints.size());
	const bool jsonReady = !lastJsonPath.empty();
	const bool syncTimingReady = hasMeaningfulSyncTiming(syncPoints);
	QString statusText = completeStatusFailedText();
	const char *statusStyle = STYLE_COMPLETE_STATUS_ERROR;
	CompletionAttention attention = CompletionAttention::NeedsAttention;
	if (jsonReady && syncTimingReady) {
		statusText = syncFileSavedStatusText(count);
		statusStyle = STYLE_COMPLETE_STATUS_SUCCESS;
		attention = CompletionAttention::Normal;
	} else if (jsonReady) {
		statusText = completeStatusNoSyncTimingText();
		statusStyle = STYLE_COMPLETE_STATUS_WARNING;
	}
	QString detailTooltip;
	if (jsonReady && syncTimingReady) {
		detailTooltip = QString::number(count) +
				QStringLiteral(" sync timing events saved.");
	} else if (jsonReady) {
		detailTooltip = syncNoTimingTooltipText();
	} else if (!jsonReady) {
		detailTooltip = syncWriteFailedTooltipText();
	}
	setJsonOutputStatus(statusText, statusStyle, attention, detailTooltip);
	updateOpenFolderButton();
}

bool VirtuEditDock::canSaveSyncJsonNow() const
{
	return hasLastOverlay &&
	       (!lastReactBase.empty() || !lastRecordingPath.empty());
}

void VirtuEditDock::saveSyncJsonIfOutputReady()
{
	if (canSaveSyncJsonNow())
		saveSyncJson();
}

void VirtuEditDock::onCompositeRecordingReady(const std::string &path)
{
	QMetaObject::invokeMethod(
		this,
		[this, path]() {
			updateReleaseNoticeVisibility();
			if (!waitingForComposite) {
				if (!path.empty()) {
					blog(LOG_INFO,
					     "[VirtuEdit] Ignored recording path outside active Virtual Recorder stop");
				}
				return;
			}

			std::string effectivePath = path;
			effectivePath = renameCompositeToRecordingSet(
				effectivePath, lastReactBase);
			compositePathReported = !effectivePath.empty();

			const int session = recordingSessionId;
			if (!fileLooksSaved(effectivePath)) {
				finishCompositeRecording(session, "");
				return;
			}

			finishCompositeRecording(session, effectivePath);
		},
		Qt::QueuedConnection);
}

void VirtuEditDock::finishCompositeRecording(int session,
					     const std::string &path)
{
	if (!isCurrentRecordingSession(session)) {
		blog(LOG_INFO,
		     "[VirtuEdit] Ignored stale composite finalize for session %d (current %d)",
		     session, recordingSessionId);
		return;
	}

	waitingForComposite = false;
	setRecordingState(false, false, false, "finishCompositeRecording");
	lastRecordingPath = path;

	QString text = completeStatusNotSavedText();
	const char *style = STYLE_COMPLETE_STATUS_ERROR;
	QString detailTooltip;
	const bool compositeSaved = fileLooksSaved(path);
	if (compositeSaved) {
		text = completeStatusSavedText();
		style = STYLE_COMPLETE_STATUS_SUCCESS;
	} else {
		detailTooltip = combinedFailedTooltipText();
	}
	setCompositeOutputStatus(
		text, style,
		compositeSaved ? CompletionAttention::Normal :
				 CompletionAttention::NeedsAttention,
		detailTooltip);

	if (!lastJsonPath.empty() || !lastReactBase.empty())
		saveSyncJson();
	updateOpenFolderButton();
	updateCompletionActions();
	updateReadiness();
}

void VirtuEditDock::onOpenOutputFolder()
{
	std::string folder = currentOutputFolder();
	if (!folder.empty())
		FfmpegExtract::openFolder(folder);
}

void VirtuEditDock::onRecordAnotherTake()
{
	if (isCompletionProcessing()) {
		updateCompletionActions();
		return;
	}

	beginRecordingSession();
	setRecordingState(false, false, false, "onRecordAnotherTake");
	yt->clearSyncPoints();
	videoLoading = false;
	playerControlsOpened = false;
	readyWorkflowStep = READY_STEP_VIDEO;
	setReadyDetailsExpanded(false);
	if (videoLoaded) {
		editingVideoUrl = false;
		if (!loadedVideoUrl.isEmpty())
			urlInput->setText(loadedVideoUrl);
		videoInfoLabel->setText(currentVideoSummary());
		videoInfoLabel->setStyleSheet(STYLE_SUCCESS_STATUS);
	} else {
		editingVideoUrl = true;
		videoInfoLabel->setText("Load a YouTube video before recording.");
		videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	}

	clearCompletionOutputState();
	updateReadiness();
	pages->setCurrentWidget(readyPage);
}

void VirtuEditDock::onNewVideo()
{
	if (isCompletionProcessing()) {
		updateCompletionActions();
		return;
	}

	beginRecordingSession();
	setRecordingState(false, false, false, "onNewVideo");
	yt->clearSyncPoints();
	playerControlsOpened = false;
	readyWorkflowStep = READY_STEP_VIDEO;
	setReadyDetailsExpanded(false);
	urlInput->clear();
	videoInfoLabel->setText("No YouTube video loaded.");
	videoInfoLabel->setStyleSheet(STYLE_SUBTLE);
	loadedVideoUrl.clear();
	pendingVideoUrl.clear();
	videoLoading = false;
	videoLoaded = false;
	editingVideoUrl = true;
	loadedVideoId.clear();
	pendingVideoId.clear();
	fallbackVideoUrl.clear();
	fallbackVideoId.clear();

	// Clear the previous video so it doesn't linger in the scene
	yt->clearVideo();
	clearCompletionOutputState();
	updateReadiness();

	pages->setCurrentWidget(readyPage);
}

void VirtuEditDock::updateRecordingTimer()
{
	qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - recStartMs;
	recTimeLabel->setText("REC " + formatRecTime(elapsed));
}

void VirtuEditDock::onYouTubeStateChanged()
{
	const auto &info = yt->getInfo();

	bool playerReady = !info.title.empty() || info.duration > 0;
	if (playerReady && !videoLoaded) {
		videoLoading = false;
		videoLoaded = true;
		editingVideoUrl = false;
		if (!pendingVideoUrl.isEmpty()) {
			loadedVideoUrl = pendingVideoUrl;
			pendingVideoUrl.clear();
		}
		if (!pendingVideoId.empty()) {
			loadedVideoId = pendingVideoId;
			pendingVideoId.clear();
		} else if (!yt->getCurrentVideoId().empty()) {
			loadedVideoId = yt->getCurrentVideoId();
		}
		fallbackVideoUrl.clear();
		fallbackVideoId.clear();
		loadTimer->stop();
		videoInfoLabel->setText(currentVideoSummary());
		videoInfoLabel->setStyleSheet(STYLE_SUCCESS_STATUS);
		readyWorkflowStep = READY_STEP_VIDEO;
		updateReadiness();

		// Apply the chosen relative volume now that the synced player is ready.
		yt->setVolume(relativeVolume);
	}

	if (pages->currentWidget() == recordingPage) {
		if (stoppingRecording || webcamRecorder.isFinalizing()) {
			playbackLabel->setText("Finalizing files...");
			playbackLabel->setStyleSheet(STYLE_SUBTLE);
			return;
		}
		QString stateStr;
		switch (info.state) {
		case 1:
			stateStr = QStringLiteral("Playing");
			break;
		case 2:
			stateStr = QStringLiteral("Paused");
			break;
		case 0:
			stateStr = QStringLiteral("Ended");
			break;
		default:
			stateStr = QStringLiteral("--");
			break;
		}
		playbackLabel->setText(stateStr + QStringLiteral(" ") +
				      formatTime(info.currentTime) +
				      QStringLiteral(" / ") +
				      formatTime(info.duration));
	}
}

void VirtuEditDock::onTimestampReceived()
{
	int count = static_cast<int>(yt->getSyncPoints().size());
	syncCountLabel->setText(syncTimingCountText(count));
	if (pages->currentWidget() == completePage)
		saveSyncJsonIfOutputReady();
}
