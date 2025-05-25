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
#endif

class WASAPIStream final : public SoundStream
{
#ifdef _WIN32
public:
	WASAPIStream(bool exclusive_mode, std::string device = "Default");

	~WASAPIStream()
	{
		if(m_need_data_event)
			CloseHandle(m_need_data_event);
		if(m_renderer)
			m_renderer->Release();
		if(m_audio_client)
			m_audio_client->Release();

		CoUninitialize();
	}

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

	std::wstring m_default_audio_device_id_prior_to_switch;
	void SwitchDefaultAudioOutputDeviceByDeviceNameAndStorePriorDeviceId(const std::string &device_name);
	void RestoreDefaultAudioOutputDevice();
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
