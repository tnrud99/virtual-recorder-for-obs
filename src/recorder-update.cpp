#include "recorder-update.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QOperatingSystemVersion>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <utility>

namespace {
constexpr qsizetype ManifestLimit = 16 * 1024;
constexpr int RequestTimeoutMs = 8000;

QVersionNumber parseVersion(const QString &text)
{
	static const QRegularExpression numericVersion(
		QStringLiteral("\\A[0-9]+(?:\\.[0-9]+){0,2}\\z"));
	if (!numericVersion.match(text).hasMatch())
		return {};

	QList<int> segments;
	for (const QString &part : text.split(QLatin1Char('.'))) {
		bool ok = false;
		const int number = part.toInt(&ok);
		if (!ok)
			return {};
		segments.append(number);
	}
	return QVersionNumber(segments);
}

QString platformChannel()
{
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
	return QStringLiteral("windows-x64");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
	return QStringLiteral("macos-arm64");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_X86_64)
	return QStringLiteral("macos-x64");
#else
	return {};
#endif
}

bool isRecorderDownloadUrl(const QUrl &url)
{
	return url.isValid() && url.scheme() == QStringLiteral("https") &&
	       url.userInfo().isEmpty() && url.port(443) == 443 &&
	       ((url.host() == QStringLiteral("virtuedit.com") &&
		 url.path() == QStringLiteral("/download")) ||
		(url.host() == QStringLiteral("github.com") &&
		 url.path().startsWith(QStringLiteral(
			 "/tnrud99/virtuedit-downloads/releases/"))));
}
}

std::optional<RecorderUpdate> findRecorderUpdate(
	const QByteArray &manifest, const QString &channel,
	const QString &installedVersion, const QString &obsVersion,
	const QVersionNumber &osVersion)
{
	if (manifest.size() > ManifestLimit || channel.isEmpty())
		return std::nullopt;

	const QJsonObject root = QJsonDocument::fromJson(manifest).object();
	if (root.value(QStringLiteral("schema")).toInt() != 1)
		return std::nullopt;

	const QJsonObject release = root.value(QStringLiteral("releases"))
					   .toObject()
					   .value(channel)
					   .toObject();
	const QString version = release.value(QStringLiteral("version")).toString();
	const auto available = parseVersion(version);
	const auto installed = parseVersion(installedVersion);
	const auto obs = parseVersion(obsVersion);
	const auto minObs = parseVersion(
		release.value(QStringLiteral("minObsVersion")).toString());
	const auto maxObs = parseVersion(
		release.value(QStringLiteral("maxObsVersion")).toString());
	const auto minOs = parseVersion(
		release.value(QStringLiteral("minOsVersion")).toString());
	const QUrl url(release.value(QStringLiteral("downloadUrl")).toString(),
		       QUrl::StrictMode);

	if (available.segmentCount() != 3 || installed.segmentCount() != 3 ||
	    obs.isNull() || minObs.isNull() || maxObs.isNull() ||
	    minOs.isNull() || osVersion.isNull() || !isRecorderDownloadUrl(url))
		return std::nullopt;
	if (available <= installed || obs.normalized() < minObs.normalized() ||
	    obs.normalized() > maxObs.normalized() ||
	    osVersion.normalized() < minOs.normalized())
		return std::nullopt;

	return RecorderUpdate{version, url};
}

void checkRecorderUpdate(QObject *context, const QString &obsVersion,
			 std::function<void(const RecorderUpdate &)> onUpdate)
{
	const QString channel = platformChannel();
	if (channel.isEmpty())
		return;

	auto *network = new QNetworkAccessManager(context);
	QNetworkRequest request(
		QUrl(QStringLiteral("https://virtuedit.com/recorder-update.json")));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
			     QNetworkRequest::SameOriginRedirectPolicy);
	auto *reply = network->get(request);
	reply->setReadBufferSize(ManifestLimit + 1);

	QTimer::singleShot(RequestTimeoutMs, reply, &QNetworkReply::abort);
	QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply]() {
		if (reply->bytesAvailable() > ManifestLimit)
			reply->abort();
	});
	QObject::connect(reply, &QNetworkReply::finished, context,
		[network, reply, channel, obsVersion, onUpdate = std::move(onUpdate)]() {
			network->deleteLater();
			if (reply->error() != QNetworkReply::NoError ||
			    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
				    .toInt() != 200)
				return;

			const auto update = findRecorderUpdate(
				reply->readAll(), channel,
				QStringLiteral(VIRTUEDIT_VERSION), obsVersion,
				QOperatingSystemVersion::current().version());
			if (update)
				onUpdate(*update);
		});
}
