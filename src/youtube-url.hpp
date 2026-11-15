#pragma once

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

inline std::string virtuedit_trim_youtube_input(std::string value)
{
	auto isSpace = [](unsigned char c) { return std::isspace(c); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(),
						[&](unsigned char c) {
							return !isSpace(c);
						}));
	value.erase(std::find_if(value.rbegin(), value.rend(),
				 [&](unsigned char c) { return !isSpace(c); })
			    .base(),
		    value.end());
	if (value.size() >= 2 && value.front() == '<' && value.back() == '>')
		value = value.substr(1, value.size() - 2);
	return value;
}

inline bool virtuedit_is_youtube_video_id(const std::string &value)
{
	static const std::regex re("^[A-Za-z0-9_-]{11}$");
	return std::regex_match(value, re);
}

inline std::string
virtuedit_extract_youtube_video_id(const std::string &input)
{
	const std::string value = virtuedit_trim_youtube_input(input);
	if (virtuedit_is_youtube_video_id(value))
		return value;

	static const auto flags = std::regex_constants::icase |
				  std::regex_constants::ECMAScript;
	static const std::regex watchRe(
		R"((?:^|://|\.)(?:youtube\.com|youtube-nocookie\.com)/[^\s#]*[?&]v=([A-Za-z0-9_-]{11})(?=$|[&#\s]))",
		flags);
	static const std::regex pathRe(
		R"((?:^|://|\.)(?:youtube\.com|youtube-nocookie\.com)/(?:shorts|embed|live|v)/([A-Za-z0-9_-]{11})(?=$|[/?#&\s]))",
		flags);
	static const std::regex shortRe(
		R"((?:^|://|\.)youtu\.be/([A-Za-z0-9_-]{11})(?=$|[/?#&\s]))",
		flags);

	std::smatch match;
	if (std::regex_search(value, match, watchRe) && match.size() > 1)
		return match[1].str();
	if (std::regex_search(value, match, pathRe) && match.size() > 1)
		return match[1].str();
	if (std::regex_search(value, match, shortRe) && match.size() > 1)
		return match[1].str();

	return "";
}
