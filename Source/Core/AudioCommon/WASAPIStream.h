// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "AudioCommon/SoundStream.h"

#include <string>
#include <vector>
#include <deque>

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
	void AlterVolumeOfAudioDevice(float db_change, bool store_original_and_set_flag);
	void RestoreVolumeIfNeeded();

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
