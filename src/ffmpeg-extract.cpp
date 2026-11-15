#include "ffmpeg-extract.hpp"
#include <obs.h>
#include <obs-module.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

#ifdef _WIN32
static std::wstring utf8ToWide(const std::string &value)
{
	if (value.empty())
		return std::wstring();

	int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
				       static_cast<int>(value.size()),
				       nullptr, 0);
	if (size <= 0)
		return std::wstring();

	std::wstring wide(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
			    static_cast<int>(value.size()), wide.data(), size);
	return wide;
}

static std::wstring quoteWindowsArg(const std::string &arg)
{
	const std::wstring value = utf8ToWide(arg);
	std::wstring quoted = L"\"";
	int backslashes = 0;

	for (wchar_t ch : value) {
		if (ch == L'\\') {
			backslashes++;
			continue;
		}
		if (ch == L'"') {
			quoted.append(backslashes * 2 + 1, L'\\');
			quoted += L'"';
			backslashes = 0;
			continue;
		}
		quoted.append(backslashes, L'\\');
		backslashes = 0;
		quoted += ch;
	}

	quoted.append(backslashes * 2, L'\\');
	quoted += L"\"";
	return quoted;
}

static void deleteFileUtf8(const std::string &path)
{
	const std::wstring widePath = utf8ToWide(path);
	if (!widePath.empty())
		DeleteFileW(widePath.c_str());
}
#endif

static bool fileExists(const std::string &path)
{
#ifdef _WIN32
	const std::wstring widePath = utf8ToWide(path);
	DWORD attr = widePath.empty() ? INVALID_FILE_ATTRIBUTES
				      : GetFileAttributesW(widePath.c_str());
	return attr != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return stat(path.c_str(), &st) == 0;
#endif
}

// Run a process safely without shell interpretation (no command injection).
// Returns exit code, or -1 on spawn failure.
static int runProcess(const std::vector<std::string> &args)
{
	if (args.empty())
		return -1;

#ifdef _WIN32
	// Build a quoted command line for CreateProcessW.
	std::wstring cmdLine;
	for (size_t i = 0; i < args.size(); i++) {
		if (i > 0)
			cmdLine += L' ';
		cmdLine += quoteWindowsArg(args[i]);
	}

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};

	if (!CreateProcessW(nullptr, cmdLine.data(),
			    nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			    nullptr, nullptr, &si, &pi)) {
		blog(LOG_ERROR, "[VirtuEdit] CreateProcess failed for: %s",
		     args[0].c_str());
		return -1;
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return static_cast<int>(exitCode);
#else
	std::vector<char *> argv;
	for (const auto &a : args)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);

	pid_t pid;
	int err = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(),
			       environ);
	if (err != 0) {
		blog(LOG_ERROR, "[VirtuEdit] posix_spawnp failed for %s: %s",
		     argv[0], strerror(err));
		return -1;
	}

	int status;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Search PATH environment variable for a named binary.
static std::string findInPath(const char *name)
{
	const char *pathEnv = getenv("PATH");
	if (!pathEnv)
		return "";

	std::istringstream iss(pathEnv);
	std::string dir;
#ifdef _WIN32
	const char delim = ';';
#else
	const char delim = ':';
#endif
	while (std::getline(iss, dir, delim)) {
		if (dir.empty())
			continue;
#ifdef _WIN32
		std::string candidate = dir + "\\" + name;
#else
		std::string candidate = dir + "/" + name;
#endif
		if (fileExists(candidate))
			return candidate;
	}
	return "";
}

static bool allowSystemFfmpegFallback()
{
	const char *allow = getenv("VIRTUEDIT_ALLOW_SYSTEM_FFMPEG");
	return allow && strcmp(allow, "1") == 0;
}

std::string FfmpegExtract::findFfmpeg()
{
	// 1. Check plugin data directory (bundled ffmpeg)
	char *dataPath = obs_module_file("ffmpeg");
	if (dataPath) {
		std::string bundled = dataPath;
#ifdef _WIN32
		bundled += ".exe";
#endif
		bfree(dataPath);
		if (fileExists(bundled)) {
			blog(LOG_INFO, "[VirtuEdit] Using bundled ffmpeg: %s",
			     bundled.c_str());
			return bundled;
		}
	}

	if (!allowSystemFfmpegFallback()) {
		blog(LOG_ERROR,
		     "[VirtuEdit] Bundled LGPL ffmpeg was not found. Reinstall Virtual Recorder.");
		return "";
	}

#ifndef _WIN32
	// 2. Check common absolute locations first.
	// GUI apps launched from Finder have a minimal PATH that usually
	// excludes Homebrew dirs, so PATH search would fail there.
	const char *candidates[] = {
		"/opt/homebrew/bin/ffmpeg", // Apple Silicon Homebrew
		"/usr/local/bin/ffmpeg",    // Intel Homebrew
		"/usr/bin/ffmpeg",          // system
	};
	for (const char *c : candidates) {
		if (fileExists(c)) {
			blog(LOG_INFO, "[VirtuEdit] Using ffmpeg: %s", c);
			return c;
		}
	}
#endif

	// 3. Check system PATH (without invoking a shell)
#ifdef _WIN32
	std::string found = findInPath("ffmpeg.exe");
#else
	std::string found = findInPath("ffmpeg");
#endif
	if (!found.empty()) {
		blog(LOG_INFO, "[VirtuEdit] Using system ffmpeg: %s",
		     found.c_str());
		return found;
	}

	blog(LOG_WARNING, "[VirtuEdit] ffmpeg not found");
	return "";
}

bool FfmpegExtract::remuxBlocking(const std::string &inputPath,
				  const std::string &outputPath,
				  bool deleteSource)
{
	std::string ffmpeg = findFfmpeg();
	if (ffmpeg.empty())
		return false;

	std::vector<std::string> args = {ffmpeg,       "-i",          inputPath,
					 "-c",         "copy",        "-movflags",
					 "+faststart", "-y",          outputPath};

	blog(LOG_INFO, "[VirtuEdit] remux: %s -> %s", inputPath.c_str(),
	     outputPath.c_str());

	int ret = runProcess(args);
	bool success = (ret == 0) && fileExists(outputPath);

	if (success) {
		blog(LOG_INFO, "[VirtuEdit] Remuxed: %s", outputPath.c_str());
		if (deleteSource) {
#ifdef _WIN32
			deleteFileUtf8(inputPath);
#else
			remove(inputPath.c_str());
#endif
		}
	} else {
		blog(LOG_ERROR, "[VirtuEdit] remux failed (exit code %d)", ret);
	}

	return success;
}

void FfmpegExtract::revealInExplorer(const std::string &path)
{
#ifdef _WIN32
	const std::wstring widePath = utf8ToWide(path);
	const std::wstring params = L"/select,\"" + widePath + L"\"";
	ShellExecuteW(NULL, L"open", L"explorer.exe", params.c_str(), NULL,
		      SW_SHOWNORMAL);
#elif defined(__APPLE__)
	runProcess({"open", "-R", path});
#else
	// Open the containing directory
	std::string dir = path;
	auto lastSlash = dir.find_last_of('/');
	if (lastSlash != std::string::npos)
		dir = dir.substr(0, lastSlash);
	runProcess({"xdg-open", dir});
#endif
}

void FfmpegExtract::openFolder(const std::string &path)
{
#ifdef _WIN32
	const std::wstring widePath = utf8ToWide(path);
	ShellExecuteW(NULL, L"open", widePath.c_str(), NULL, NULL,
		      SW_SHOWNORMAL);
#elif defined(__APPLE__)
	runProcess({"open", path});
#else
	runProcess({"xdg-open", path});
#endif
}

void FfmpegExtract::openInBrowser(const std::string &url)
{
#ifdef _WIN32
	const std::wstring wideUrl = utf8ToWide(url);
	ShellExecuteW(NULL, L"open", wideUrl.c_str(), NULL, NULL,
		      SW_SHOWNORMAL);
#elif defined(__APPLE__)
	runProcess({"open", url});
#else
	runProcess({"xdg-open", url});
#endif
}
