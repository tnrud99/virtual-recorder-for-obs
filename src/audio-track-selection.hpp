#pragma once

#include <cstdint>

inline uint32_t virtuedit_audio_track_bit(int oneBasedTrack)
{
	return oneBasedTrack >= 1 && oneBasedTrack <= 6
		       ? (1u << static_cast<unsigned>(oneBasedTrack - 1))
		       : 0u;
}

inline int virtuedit_select_reaction_audio_track(uint32_t protectedMask)
{
	for (int track = 5; track >= 0; --track) {
		if (!(protectedMask & (1u << track)))
			return track;
	}
	return -1;
}
