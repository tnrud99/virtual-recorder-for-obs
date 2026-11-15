#pragma once

#include <string>

class FfmpegExtract {
public:
	// Find ffmpeg binary: plugin data dir first, then system PATH
	static std::string findFfmpeg();

	// Remux a file to a new container without re-encoding.
	static bool remuxBlocking(const std::string &inputPath,
				  const std::string &outputPath,
				  bool deleteSource);

	// Open file in system file explorer (reveal in Finder/Explorer)
	static void revealInExplorer(const std::string &path);

	// Open a folder in the system file explorer
	static void openFolder(const std::string &path);

	// Open URL in system default browser
	static void openInBrowser(const std::string &url);
};
