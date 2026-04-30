// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "AudioCommon/SoundStream.h"

#include <string>
#include <vector>
#include <deque>
#include <mutex>

#ifdef _WIN32
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

class WASAPIStream final : public SoundStream
{
#ifdef _WIN32
public:
	WASAPIStream(bool exclusive_mode, std::string device = "Default");

	~WASAPIStream();

	bool Start() override;
	void SoundLoop() override;
	void Stop() override;

	static bool isValid()
	{
		return true;
	}

	static std::vector<std::string> GetRenderDeviceNames();
	static std::vector<std::string> GetCaptureDeviceNames();

  private:
	IAudioClient* m_audio_client = nullptr;
	IAudioRenderClient * m_renderer = nullptr;
	IAudioClient* m_capture_audio_client = nullptr;
	IAudioCaptureClient* m_capture_client = nullptr;
	std::string m_selected_device;

	// Long lived device COM object for output device used, used to switch volume back on Stop
	IMMDevice *m_mm_device = nullptr;

	HANDLE m_need_data_event = nullptr;

	u32 frames_in_buffer = 0;

	WAVEFORMATEXTENSIBLE fmt;

	bool m_exclusive_mode;

	enum class AudioCaptureType
	{
		None,
		Recording,
		Loopback,
	};
	AudioCaptureType m_audioCaptureType;
	std::deque<s16> m_internal_capture_buffer;
	bool InitializeCaptureClient();
	WAVEFORMATEXTENSIBLE captureFormat;
	void CaptureAudioAndMix(s16 *mix_buffer, u32 num_samples);

	bool m_pending_audio_device_switch_back = false; // Should switch to optional upon migration to C++17
	std::wstring m_default_audio_device_id_prior_to_switch;

	void SwitchDefaultAudioOutputDeviceByDeviceNameAndStorePriorDeviceId(const std::string &device_name, bool doubleVolume);
	void RestoreDefaultAudioOutputDevice();

	bool m_should_switch_volume_back = false;
	float m_original_volume_db = 0.0f;
	// True if AlterVolumeOfAudioDevice successfully applied the full +6 dB boost.
	// Controls whether CaptureAudioAndMix halves both signals before summing.
	bool m_volume_boost_applied = false;
	// Original volume of the sacrificial endpoint, restored on Stop().
	float m_sacrificial_device_original_volume_db = 0.0f;
	bool m_should_restore_sacrificial_volume = false;
	bool AlterVolumeOfAudioDevice(float db_change, bool store_original_and_set_flag);
	void RestoreVolumeIfNeeded();

	// --- Timer-driven exclusive direct-push path ---

	// Called from the DMA thread via CMixer::PushSamples. Resamples the incoming game audio
	// from input_sample_rate to 48 kHz, applies margin-based drop/duplicate correction, mixes
	// in any captured audio, applies volume, and writes directly to the WASAPI render buffer.
	void FeedSamplesDirect(const s16 *samples, u32 num_samples, u32 input_sample_rate);

	// Margin (target padding) in render frames. Recomputed at Start() from SConfig::iMargin.
	u32 m_margin_frames = 0;

	// Linear resampler state for the direct path. Persisted across PushSamples calls so the
	// resampler picks up where the previous block left off without discontinuities.
	float m_resample_phase = 0.0f;     // current fractional position into the input stream
	s16 m_resample_last_l = 0;         // last input-domain sample for interpolation
	s16 m_resample_last_r = 0;
	u32 m_resample_last_input_rate = 0;

	// Rolling window of the smallest pre-write padding (in frames) seen across the last N
	// pushes. Used to detect persistent over/under-fill of the WASAPI buffer relative to the
	// margin and trigger a single drop/duplicate correction per push.
	static constexpr u32 PADDING_HISTORY_SIZE = 20;
	std::deque<u32> m_padding_history;

	// Set to false at Start(); the first FeedSamplesDirect call primes the buffer with 10 ms
	// of silence before writing real audio, then flips this to true.
	bool m_direct_path_primed = false;

	std::mutex m_direct_path_mutex; // guards FeedSamplesDirect against Stop()

#else
public:
	WASAPIStream(bool exclusive_mode, std::string device = "Default") { }

	inline static std::vector<std::string> GetRenderDevices()
	{
		return {};
	}
	inline static std::vector<std::string> GetCaptureDevices()
	{
		return {};
	}
#endif
};
