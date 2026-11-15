#pragma once

#include <QString>
#include <QUrl>
#include <QVersionNumber>
#include <functional>
#include <optional>

struct RecorderUpdate {
	QString version;
	QUrl downloadUrl;
};

std::optional<RecorderUpdate> findRecorderUpdate(
	const QByteArray &manifest, const QString &channel,
	const QString &installedVersion, const QString &obsVersion,
	const QVersionNumber &osVersion);

void checkRecorderUpdate(QObject *context, const QString &obsVersion,
			 std::function<void(const RecorderUpdate &)> onUpdate);
