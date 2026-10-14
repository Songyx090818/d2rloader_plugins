#pragma once
//
// Plays sound files from a filter's sounds folder (.wav, .mp3 and .flac) on its
// own thread, so the game never waits on a file. Built on miniaudio (public
// domain / MIT-0, miniaudio.h next to this file).
//
#include <string>

namespace lootfilter::sound {

// Starts the worker on first use. volume is 0-100.
void PlayFile(const std::wstring& path, int volume) noexcept;

// Stops the worker and releases the audio device. Safe to call more than once.
void Shutdown() noexcept;

}  // namespace lootfilter::sound
