#include "sound_player.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_VORBIS
#define MINIAUDIO_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "miniaudio.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>

namespace lootfilter::sound {
namespace {

struct Request {
    std::wstring path;
    int          volume{};
};

std::mutex              QueueLock;
std::condition_variable QueueSignal;
std::deque<Request>     Queue;
std::thread             Worker;
bool                    Stopping{};
bool                    Started{};

void Run() noexcept {
    ma_engine engine{};
    bool engineReady = ma_engine_init(nullptr, &engine) == MA_SUCCESS;
    std::list<ma_sound> playing;
    while (true) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(QueueLock);
            QueueSignal.wait_for(lock, std::chrono::milliseconds(250), [] { return Stopping || !Queue.empty(); });
            if (Stopping) break;
            if (!Queue.empty()) {
                request = std::move(Queue.front());
                Queue.pop_front();
            }
        }
        for (auto it = playing.begin(); it != playing.end();) {
            if (ma_sound_at_end(&*it)) {
                ma_sound_uninit(&*it);
                it = playing.erase(it);
            } else {
                ++it;
            }
        }
        if (request.path.empty() || !engineReady) continue;
        if (playing.size() >= 16) continue;  // a pile of drops must not stack dozens of sounds
        playing.emplace_back();
        ma_sound& sound = playing.back();
        if (ma_sound_init_from_file_w(&engine, request.path.c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                      nullptr, nullptr, &sound) != MA_SUCCESS) {
            playing.pop_back();
            continue;
        }
        ma_sound_set_volume(&sound, static_cast<float>(request.volume) / 100.0f);
        if (ma_sound_start(&sound) != MA_SUCCESS) {
            ma_sound_uninit(&sound);
            playing.pop_back();
        }
    }
    for (ma_sound& sound : playing) ma_sound_uninit(&sound);
    playing.clear();
    if (engineReady) ma_engine_uninit(&engine);
}

}  // namespace

void PlayFile(const std::wstring& path, int volume) noexcept {
    try {
        std::lock_guard<std::mutex> lock(QueueLock);
        if (Stopping) return;
        if (!Started) {
            Worker  = std::thread(Run);
            Started = true;
        }
        if (Queue.size() < 32) Queue.push_back(Request{ path, volume < 0 ? 0 : (volume > 100 ? 100 : volume) });
    } catch (...) {
        return;
    }
    QueueSignal.notify_one();
}

void Shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(QueueLock);
        if (!Started || Stopping) return;
        Stopping = true;
        Queue.clear();
    }
    QueueSignal.notify_one();
    try {
        if (Worker.joinable()) Worker.join();
    } catch (...) {
    }
}

}  // namespace lootfilter::sound
