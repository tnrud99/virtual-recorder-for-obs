#pragma once

#include <string>

struct ReactionEncoderSettings {
	std::string outputMode = "Simple";
	std::string sourceDescription = "OBS recording settings";
	std::string rateControl = "CRF";
	std::string preset = "veryfast";
	std::string profile = "high";
	int crf = 18;
	int videoBitrate = 6000;
	int audioBitrate = 192;
};

struct ReactionEncoderCustomSettings {
	bool enabled = false;
	int crf = 18;
	int videoBitrate = 6000;
	int audioBitrate = 192;
};
