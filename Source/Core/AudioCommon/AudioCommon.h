// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <memory>

#include "AudioCommon/SoundStream.h"
#include "Common/CommonTypes.h"

class CMixer;

extern std::unique_ptr<SoundStream> g_sound_stream;

namespace AudioCommon
{
void InitSoundStream(void *hwnd);
void ShutdownSoundStream();
std::vector<std::string> GetSoundBackends();
std::vector<std::string> GetAudioInputDeviceNames();
std::vector<std::string> GetAudioOutputDeviceNames();
bool SupportsDPL2Decoder(const std::string &backend);
bool SupportsLatencyControl(const std::string &backend);
bool SupportsVolumeChanges(const std::string &backend);
void UpdateSoundStream();
void ClearAudioBuffer(bool mute);
void SendAIBuffer(const short *samples, unsigned int num_samples);
void StartAudioDump();
void StopAudioDump();
void IncreaseVolume(unsigned short offset);
void DecreaseVolume(unsigned short offset);
void ToggleMuteVolume();

// Audio margin display (timer-driven exclusive WASAPI only).
// Updated from FeedSamplesDirect on every push with the rolling 20-block minimum padding
// in milliseconds. -1 when the direct path is not active.
extern std::atomic<float> g_audio_min_margin_ms;

} // namespace AudioCommon
