#include "audio-track-selection.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main()
{
	assert(virtuedit_audio_track_bit(0) == 0u);
	assert(virtuedit_audio_track_bit(1) == 0x01u);
	assert(virtuedit_audio_track_bit(6) == 0x20u);
	assert(virtuedit_audio_track_bit(7) == 0u);

	assert(virtuedit_select_reaction_audio_track(0x00u) == 5);
	assert(virtuedit_select_reaction_audio_track(0x03u) == 5);
	assert(virtuedit_select_reaction_audio_track(0x1Fu) == 5);
	assert(virtuedit_select_reaction_audio_track(0x3Eu) == 0);
	assert(virtuedit_select_reaction_audio_track(0x3Fu) == -1);

	std::cout << "audio track selection tests passed\n";
	return 0;
}
