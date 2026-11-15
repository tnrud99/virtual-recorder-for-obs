#include "youtube-url.hpp"

#include <iostream>
#include <string>
#include <vector>

struct Case {
	std::string input;
	std::string expected;
};

int main()
{
	const std::vector<Case> cases = {
		{"https://www.youtube.com/watch?v=dQw4w9WgXcQ",
		 "dQw4w9WgXcQ"},
		{"https://m.youtube.com/watch?v=dQw4w9WgXcQ&feature=youtu.be",
		 "dQw4w9WgXcQ"},
		{"https://music.youtube.com/watch?feature=share&v=dQw4w9WgXcQ",
		 "dQw4w9WgXcQ"},
		{"https://youtu.be/dQw4w9WgXcQ?si=abc", "dQw4w9WgXcQ"},
		{"youtu.be/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
		{"https://www.youtube.com/shorts/dQw4w9WgXcQ",
		 "dQw4w9WgXcQ"},
		{"https://www.youtube.com/embed/dQw4w9WgXcQ?start=10",
		 "dQw4w9WgXcQ"},
		{"https://www.youtube.com/live/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
		{"https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ",
		 "dQw4w9WgXcQ"},
		{"dQw4w9WgXcQ", "dQw4w9WgXcQ"},
		{" https://www.youtube.com/watch?foo=1&v=dQw4w9WgXcQ ",
		 "dQw4w9WgXcQ"},
		{"https://example.com/watch?v=dQw4w9WgXcQ", ""},
		{"https://www.youtube.com/playlist?list=PL123", ""},
		{"not a youtube link", ""},
	};

	int failures = 0;
	for (const auto &test : cases) {
		const std::string actual =
			virtuedit_extract_youtube_video_id(test.input);
		if (actual != test.expected) {
			std::cerr << "FAIL: " << test.input << "\n  expected: "
				  << test.expected << "\n  actual: " << actual
				  << "\n";
			failures++;
		}
	}

	if (failures > 0)
		return 1;

	std::cout << "[OK] youtube-url-parser (" << cases.size()
		  << " cases)\n";
	return 0;
}
