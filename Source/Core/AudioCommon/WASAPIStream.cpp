// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "AudioCommon/WASAPIStream.h"
#include "AudioCommon/AudioCommon.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Common/CommonFuncs.h"
#include "VideoCommon/OnScreenDisplay.h"

#include <avrt.h>
#include <mmdeviceapi.h>

#include <locale>
#include <sstream>

#include "IPolicyConfig.h"
#include <Endpointvolume.h>
#include <cmath>
#include <algorithm>

// TODO
// Ensure volume control works in both modes
// Ensure change of output device works in shared mode (see Windows SDK samples linked in
// https://learn.microsoft.com/en-us/windows/win32/coreaudio/stream-routing)


#define SAFE_RELEASE(p)                                                                                                \
	{                                                                                                                  \
		if ((p))                                                                                                       \
		{                                                                                                              \
			(p)->Release();                                                                                            \
			(p) = nullptr;                                                                                             \
		}                                                                                                              \
	}

WASAPIStream::WASAPIStream(bool exclusive_mode, std::string device)
    : m_exclusive_mode(exclusive_mode)
    , m_selected_device(device)
    , m_audioCaptureType(SConfig::GetInstance().m_mixLoopedBackAudioIn ? AudioCaptureType::Loopback :
                         SConfig::GetInstance().m_mixRecordedAudioIn ? AudioCaptureType::Recording : AudioCaptureType::None)
{
	CoInitialize(nullptr);
}
WASAPIStream::~WASAPIStream() {
	if (m_need_data_event)
		CloseHandle(m_need_data_event);

	SAFE_RELEASE(m_renderer);
	SAFE_RELEASE(m_capture_client);
	SAFE_RELEASE(m_audio_client);
	SAFE_RELEASE(m_capture_audio_client);
	SAFE_RELEASE(m_mm_device);

	CoUninitialize();
}

static std::string wasapi_hresult_to_string(HRESULT res)
{
	switch (res)
	{
#define DEFINE_FOR(hres) case hres: return #hres;
		DEFINE_FOR(AUDCLNT_E_NOT_INITIALIZED)
		DEFINE_FOR(AUDCLNT_E_ALREADY_INITIALIZED)
		DEFINE_FOR(AUDCLNT_E_WRONG_ENDPOINT_TYPE)
		DEFINE_FOR(AUDCLNT_E_DEVICE_INVALIDATED)
		DEFINE_FOR(AUDCLNT_E_NOT_STOPPED)
		DEFINE_FOR(AUDCLNT_E_BUFFER_TOO_LARGE)
		DEFINE_FOR(AUDCLNT_E_OUT_OF_ORDER)
		DEFINE_FOR(AUDCLNT_E_UNSUPPORTED_FORMAT)
		DEFINE_FOR(AUDCLNT_E_INVALID_SIZE)
		DEFINE_FOR(AUDCLNT_E_DEVICE_IN_USE)
		DEFINE_FOR(AUDCLNT_E_BUFFER_OPERATION_PENDING)
		DEFINE_FOR(AUDCLNT_E_THREAD_NOT_REGISTERED)
		DEFINE_FOR(AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
		DEFINE_FOR(AUDCLNT_E_ENDPOINT_CREATE_FAILED)
		DEFINE_FOR(AUDCLNT_E_SERVICE_NOT_RUNNING)
		DEFINE_FOR(AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED)
		DEFINE_FOR(AUDCLNT_E_EXCLUSIVE_MODE_ONLY)
		DEFINE_FOR(AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL)
		DEFINE_FOR(AUDCLNT_E_EVENTHANDLE_NOT_SET)
		DEFINE_FOR(AUDCLNT_E_INCORRECT_BUFFER_SIZE)
		DEFINE_FOR(AUDCLNT_E_BUFFER_SIZE_ERROR)
		DEFINE_FOR(AUDCLNT_E_CPUUSAGE_EXCEEDED)
		DEFINE_FOR(AUDCLNT_E_RESOURCES_INVALIDATED)
		DEFINE_FOR(AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		DEFINE_FOR(AUDCLNT_E_INVALID_DEVICE_PERIOD)
		DEFINE_FOR(E_POINTER)
		DEFINE_FOR(E_INVALIDARG)
		DEFINE_FOR(E_OUTOFMEMORY)
	}

	return "UNKNOWN, " + std::to_string(res);
}

#ifndef PKEY_Device_FriendlyName
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);
#endif

// Historical link https://github.com/mvaneerde/blog/blob/master/play-exclusive/play-exclusive/play.cpp

/* Everything used in this file is explained here https://learn.microsoft.com/en-us/windows/win32/CoreAudio/about-the-windows-core-audio-apis
   We used event driven audio processing, in both shared and exclusive modes.

   Event driven modes let us ask for buffer sizes much smaller. Not necessarily because we couldn't keep up otherwise,
   but because WASAPI doesn't let us get small sizes outside of event driven modes.*/

/* >Windows Vista has several features to support applications that require low-latency audio streams.
   >As discussed in User-Mode Audio Components, applications that perform time-critical operations can call the Multimedia Class
   >Scheduler Service (MMCSS) functions to increase thread priority without denying  CPU resources to lower-priority applications.
   >In addition, the IAudioClient::Initialize method supports an AUDCLNT_STREAMFLAGS_EVENTCALLBACK flag that enables an application's
   >buffer-servicing thread to schedule its execution to occur when a new buffer becomes available from the audio device.
   https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams */

bool WASAPIStream::Start()
{
	HRESULT hr = S_OK;
	IMMDeviceEnumerator *mm_device_enumerator = nullptr;

	hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator),
		(void**)&mm_device_enumerator
	);

	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Error @ CoCreateInstance of MMDeviceEnumerator");
		// mm_device_enumerator is already null or CoCreateInstance failed
		return false;
	}

	m_mm_device = nullptr;

	std::string lower_device = "";
	for (char c : m_selected_device)
		lower_device += std::tolower(c, std::locale::classic());

	if (lower_device.find("default") != std::string::npos)
	{
		hr = mm_device_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_mm_device);
		if (FAILED(hr))
		{
			ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			ERROR_LOG(AUDIO, "WASAPIStream: Error @ MMDeviceEnumerator::GetDefaultAudioEndpoint");
			SAFE_RELEASE(mm_device_enumerator);
			return false;
		}
	}
	else
	{
		IMMDeviceCollection *devices = nullptr;
		hr = mm_device_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
		if (FAILED(hr))
		{
			ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			ERROR_LOG(AUDIO, "WASAPIStream: Error @ MMDeviceEnumerator::EnumAudioEndpoints");
			SAFE_RELEASE(mm_device_enumerator);
			return false;
		}

		UINT device_count = 0;
		devices->GetCount(&device_count);

		for (UINT i = 0; i < device_count; i++)
		{
			IMMDevice *device_item = nullptr; // Use a temporary variable to avoid releasing mm_device prematurely
			devices->Item(i, &device_item);

			IPropertyStore *pstore = nullptr;
			device_item->OpenPropertyStore(STGM_READ, &pstore);

			PROPVARIANT name_prop;
			PropVariantInit(&name_prop);

			pstore->GetValue(PKEY_Device_FriendlyName, &name_prop);

			char name_cstr[2048];
			size_t ret;

			ret = wcstombs(name_cstr, name_prop.pwszVal, sizeof(name_cstr));
			if(ret == 2048) name_cstr[2047] = '\0';

			std::string name_stdstr = name_cstr;

			for (int j = 0; j <= 9; j++)
			{
				if (name_stdstr.substr(0, std::string("0 - ").size()) == std::to_string(j) + " - ")
					name_stdstr = name_stdstr.substr(std::string("0 - ").size()) + " [" + std::to_string(j) + "]";
			}

			if (name_stdstr == m_selected_device)
			{
				m_mm_device =
				    device_item; // Assign and it will be released later if not used, or by SAFE_RELEASE(mm_device)
			}

			PropVariantClear(&name_prop);
			SAFE_RELEASE(pstore);

			if (m_mm_device != device_item) // If this item is not the selected one, release it now
				SAFE_RELEASE(device_item);
		}
		SAFE_RELEASE(devices);
	}

	if (m_mm_device == nullptr)
	{
		OSD::AddMessage("Invalid audio device \"" + m_selected_device +
		                    "\" selected for WASAPI. Check your backend settings.",
		                6000U);
		SAFE_RELEASE(mm_device_enumerator);
		return false;
	}


	hr = m_mm_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&m_audio_client);

	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Error @ MMDevice::Activate -> IAudioClient");
		SAFE_RELEASE(mm_device_enumerator);
		// m_audio_client is not valid or null
		return false;
	}

	/* LPWSTR id;
	mm_device->GetId(&id); // HRESULT ignored here, but GetId is unlikely to fail if Activate succeeded

	char buffer[2048];
	size_t ret_id;

	ret_id = wcstombs(buffer, id, sizeof(buffer));
	if (ret_id == 2048)
		buffer[2047] = '\0';
	CoTaskMemFree(id); // Free the string allocated by GetId

	INFO_LOG(AUDIO, "WASAPIStream: Using device %s", buffer);*/

	fmt = {0};
	fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	fmt.Format.nChannels = 2;
	fmt.Format.nSamplesPerSec = 48000;
	fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * 4;
	fmt.Format.nBlockAlign = 4;
	fmt.Format.wBitsPerSample = 16;

	fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

	fmt.Samples.wValidBitsPerSample = fmt.Format.wBitsPerSample;
	fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

	REFERENCE_TIME default_legacy_device_period;
	REFERENCE_TIME minimum_legacy_device_period;
	hr = m_audio_client->GetDevicePeriod(&default_legacy_device_period, &minimum_legacy_device_period);

	REFERENCE_TIME exclusive_device_period = minimum_legacy_device_period;

	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get minimum device period.");

		SAFE_RELEASE(m_audio_client);
		SAFE_RELEASE(mm_device_enumerator);
		return false;
	}

	// Timer-driven exclusive: no event callback, the DMA push thread fills the WASAPI buffer
	// directly via FeedSamplesDirect. Shared mode keeps the legacy event-driven flow.
	DWORD exclusiveStreamFlags = AUDCLNT_STREAMFLAGS_NOPERSIST;
	DWORD sharedStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	auto format_ptr = reinterpret_cast<WAVEFORMATEX *>(&fmt);

	if (m_exclusive_mode)
	{
		// Important: must switch other streams to the new output device before taking exclusive control of the one they were using
		// or we'll run into a myriad of issues with the other apps' streams crashing
		// Similarly, must switch back only after the exclusive stream is closed
		if (SConfig::GetInstance().m_SwitchDefaultAudioOutputDeviceDuringGameplay)
		{
			m_volume_boost_applied = AlterVolumeOfAudioDevice(6.0f, true); // +6dB i.e *2, to compensate for system signal halved
			SwitchDefaultAudioOutputDeviceByDeviceNameAndStorePriorDeviceId(SConfig::GetInstance().sAudioOutputDeviceToSwitchTo, true);
		}
		else
		{
			m_volume_boost_applied = false;
		}

		// Fixed 2048-frame buffer at 48 kHz (~42.67 ms). The actual buffered audio level is
		// regulated by the margin (drop/duplicate logic in FeedSamplesDirect), not by this size.
		constexpr u32 TARGET_FRAMES = 2048;
		exclusive_device_period =
			static_cast<REFERENCE_TIME>(10000.0 * 1000 * TARGET_FRAMES / fmt.Format.nSamplesPerSec + 0.5);

		hr = m_audio_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveStreamFlags, exclusive_device_period,
										exclusive_device_period, format_ptr, nullptr);

		if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
			OSD::AddMessage("Your current audio device doesn't support 16-bit 48000 hz PCM audio. WASAPI exclusive "
							"mode won't work.",
							6000U);

		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			INFO_LOG(AUDIO, "WASAPIStream: Device period not aligned, attempting to fix...");

			UINT32 temp_frames_in_buffer;
			HRESULT hr_align_bufsize = m_audio_client->GetBufferSize(&temp_frames_in_buffer);
			SAFE_RELEASE(m_audio_client); // Release the current client instance before re-activating

			if (FAILED(hr_align_bufsize))
			{
				ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr_align_bufsize).c_str());
				ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get buffer size for alignment.");
				SAFE_RELEASE(mm_device_enumerator);
				return false;
			}

			exclusive_device_period =
				static_cast<REFERENCE_TIME>(10000.0 * 1000 * temp_frames_in_buffer / fmt.Format.nSamplesPerSec + 0.5);

			// Need to re-activate to get a new IAudioClient instance
			hr = m_mm_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&m_audio_client);
			if (FAILED(hr))
			{
				ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
				ERROR_LOG(AUDIO, "WASAPIStream: Error @ MMDevice::Activate (alignment fix) -> IAudioClient");
				SAFE_RELEASE(mm_device_enumerator);
				// m_audio_client is already null or invalid
				return false;
			}

			hr = m_audio_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveStreamFlags, exclusive_device_period,
											exclusive_device_period, format_ptr, nullptr);
		}

		if (SUCCEEDED(hr) && m_audioCaptureType != AudioCaptureType::None)
		{
			if (!InitializeCaptureClient())
			{
				ERROR_LOG(AUDIO, "WASAPIStream: Failed to initialize capture client. External audio will not be mixed in.");
				// hr = E_FAIL; // Uncommenting this causes capture client init failing to cause overall audio init to fail
				// Currently we let audio work even if audio mix-in was asked for but doesn't work
			}
		}
	}
	else
	{
		// Shared mode => Use AudioClient3 => new device periods (Windows 10+ exclusive, enables short buffers)
		
		IAudioClient3 *audio_client_3 = nullptr;
		hr = m_audio_client->QueryInterface(__uuidof(IAudioClient3), (void **)&audio_client_3);

		if (FAILED(hr))
		{
			std::string message = "QueryInterface for IAudioClient3 failed: " + wasapi_hresult_to_string(hr) +
			                      ". Falling back to legacy Initialize.\n";
			ERROR_LOG(AUDIO, message.c_str());

			// If no IAudioClient3 interface (e.g. W7), fall back to legacy Initialize
			hr = m_audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, sharedStreamFlags, default_legacy_device_period,
			                                0, format_ptr, nullptr);
		}
		else
		{
			UINT32 defaultPeriod, fundamentalPeriod, minPeriod, maxPeriod{};
			HRESULT hr_period = audio_client_3->GetSharedModeEnginePeriod(format_ptr, &defaultPeriod,
			                                                              &fundamentalPeriod, &minPeriod, &maxPeriod);
			if (SUCCEEDED(hr_period))
			{
				std::ostringstream oss{};
				oss << "Shared Mode Engine Periods (frames @ " << format_ptr->nSamplesPerSec << " Hz):\n";
				oss << "  Default:     " << defaultPeriod << " (" << (float)defaultPeriod * 1000.0 / format_ptr->nSamplesPerSec << " ms)\n";
				oss << "  Fundamental: " << fundamentalPeriod << " (" << (float)fundamentalPeriod * 1000.0 / format_ptr->nSamplesPerSec << " ms)\n";
				oss << "  Min:         " << minPeriod << " (" << (float)minPeriod * 1000.0 / format_ptr->nSamplesPerSec << " ms)\n";
				oss << "  Max:         " << maxPeriod << " (" << (float)maxPeriod * 1000.0 / format_ptr->nSamplesPerSec << " ms)\n";
				INFO_LOG(AUDIO, oss.str().c_str());

				OSD::AddMessage(std::string{"Using audio engine period: " +
				                            std::to_string((float)minPeriod * 1000.0 / format_ptr->nSamplesPerSec) +
				                            " ms (Shared WASAPI)"},
				                10000U);
			}
			else
			{
				minPeriod = 0; // 0 means use default period
				ERROR_LOG( AUDIO,
					std::string{"GetSharedModeEnginePeriod failed: " + wasapi_hresult_to_string(hr_period)}.c_str());
			}

			hr = audio_client_3->InitializeSharedAudioStream(sharedStreamFlags, minPeriod, format_ptr, nullptr);
			if (FAILED(hr))
			{
				ERROR_LOG(AUDIO,
				          std::string{"InitializeSharedAudioStream failed: " + wasapi_hresult_to_string(hr)}.c_str());
			}
			SAFE_RELEASE(audio_client_3);
		}
	}

	SAFE_RELEASE(mm_device_enumerator);

	auto cleanUpAudioClients = [&](bool stopCaptureClient = true)
	{
		SAFE_RELEASE(m_audio_client);
		if (m_exclusive_mode && m_audioCaptureType != AudioCaptureType::None && m_capture_audio_client)
		{
			m_capture_audio_client->Stop();
			SAFE_RELEASE(m_capture_client);
			SAFE_RELEASE(m_capture_audio_client);
			if (SConfig::GetInstance().m_SwitchDefaultAudioOutputDeviceDuringGameplay)
			{
				RestoreVolumeIfNeeded();
				RestoreDefaultAudioOutputDevice();
			}
		}
	};

	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s during Initialize", wasapi_hresult_to_string(hr).c_str());
		cleanUpAudioClients(false);
		return false;
	}

	hr = m_audio_client->GetBufferSize(&frames_in_buffer);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get buffer size.");
		cleanUpAudioClients();
		return false;
	}
	else
	{
		double bufferInMs = (float)frames_in_buffer * 1000.0 / fmt.Format.nSamplesPerSec;
		INFO_LOG(AUDIO, "WASAPIStream: Buffer size: %u frames; %f ms", frames_in_buffer, bufferInMs);
		OSD::AddMessage("Effective audio buffer size: " + std::to_string(bufferInMs) + " ms", 10000U);
	}

	// In exclusive timer-driven mode there is no event handle: the DMA push thread feeds the
	// WASAPI buffer directly via FeedSamplesDirect. Only shared mode still uses the event.
	if (!m_exclusive_mode)
	{
		m_need_data_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (m_need_data_event == NULL)
		{
			ERROR_LOG(AUDIO, "WASAPIStream: Failed to create event handle.");
			cleanUpAudioClients();
			return false;
		}

		hr = m_audio_client->SetEventHandle(m_need_data_event);
		if (FAILED(hr)) // Should not fail if handle is valid
		{
			ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			ERROR_LOG(AUDIO, "WASAPIStream: Failed to set event handle.");
			CloseHandle(m_need_data_event);
			m_need_data_event = nullptr;
			cleanUpAudioClients();
			return false;
		}
	}

	hr = m_audio_client->GetService(__uuidof(IAudioRenderClient), (void **)&m_renderer);

	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get IAudioClient renderer.");
		if (m_need_data_event)
		{
			CloseHandle(m_need_data_event);
			m_need_data_event = nullptr;
		}
		cleanUpAudioClients();
		return false;
	}

	// Prime the buffer with one full period of silence so playback can start cleanly. In
	// timer-driven exclusive mode this also seeds the buffered audio level near full, from
	// which the margin regulation will quickly drift it back toward m_margin_frames.
	if (m_exclusive_mode)
	{
		BYTE *prime_data = nullptr;
		HRESULT prime_hr = m_renderer->GetBuffer(frames_in_buffer, &prime_data);
		if (SUCCEEDED(prime_hr) && prime_data != nullptr)
		{
			m_renderer->ReleaseBuffer(frames_in_buffer, AUDCLNT_BUFFERFLAGS_SILENT);
		}
	}

	hr = m_audio_client->Start();
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't start audio client.");
		if (m_need_data_event)
		{
			CloseHandle(m_need_data_event);
			m_need_data_event = nullptr;
		}
		SAFE_RELEASE(m_renderer);
		cleanUpAudioClients();
		return false;
	}

	if (m_exclusive_mode)
	{
		// Compute margin in 48 kHz frames (clamped to [1ms, half the buffer]).
		int margin_ms = std::max(1, SConfig::GetInstance().iMargin);
		m_margin_frames = static_cast<u32>(margin_ms) * (fmt.Format.nSamplesPerSec / 1000);
		if (m_margin_frames > frames_in_buffer / 2)
			m_margin_frames = frames_in_buffer / 2;

		// Reset resampler / padding-history state for a fresh stream.
		m_resample_phase = 0.0f;
		m_resample_last_l = 0;
		m_resample_last_r = 0;
		m_resample_last_input_rate = 0;
		m_padding_history.clear();
		m_direct_path_primed = false;

		// Disable the base SoundLoop thread; the DMA push path drives playback directly.
		m_enablesoundloop = false;

		// Register the direct sink on the mixer so PushSamples lands in FeedSamplesDirect.
		m_mixer->SetDirectSink([this](const s16 *samples, u32 num_samples, u32 input_sample_rate)
							   { FeedSamplesDirect(samples, num_samples, input_sample_rate); });
	}

	SoundStream::Start();
	return true;
}

void WASAPIStream::SoundLoop()
{
	if (m_audio_client && m_renderer && m_need_data_event)
	{
		Common::SetCurrentThreadName("WASAPI Event Thread");

		// Tag thread as pro audio for scheduler. Done automatically for exclusive mode by WASAPI,
		// but we do it for low latency shared mode too

		DWORD taskIndex = 0;
		HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
		if (hTask == nullptr)
		{
			DWORD error = GetLastError();
			WARN_LOG(AUDIO, "Failed to set MM thread characteristics (Error: %lu).", error);
		}
		else
		{
			INFO_LOG(AUDIO, "Set MM thread characteristics to Pro Audio.");
		}

		if (m_exclusive_mode)
		{
			// Unreachable: in timer-driven exclusive mode m_enablesoundloop is set to false in
			// Start(), so the base SoundStream never spawns this thread. The DMA push path
			// (FeedSamplesDirect) is what feeds the WASAPI buffer instead.
		}
		else // Shared mode
		{
			HRESULT hr;
			while (threadData.load())
			{
				// Wait for event
				DWORD waitResult = WaitForSingleObject(m_need_data_event, 200);
				if (!threadData.load())
					break;

				if (waitResult == WAIT_TIMEOUT)
				{
					WARN_LOG(AUDIO, "WASAPI event timeout - potential stall?");
					// Should check if client is still valid or if core state changed drastically
					continue;
				}
				if (waitResult != WAIT_OBJECT_0)
				{
					// Some other error occurred
					DWORD error = GetLastError();
					ERROR_LOG(AUDIO, "WaitForSingleObject failed with result %lu (Error: %lu)", waitResult, error);
					// Should decide whether to stop the stream here
					break;
				}

				// 'Padding' is the number of frames currently queued up in the buffer
				UINT32 padding = 0;
				hr = m_audio_client->GetCurrentPadding(&padding);
				if (FAILED(hr))
				{
					ERROR_LOG(AUDIO, "GetCurrentPadding failed: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
					if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
					{
						ERROR_LOG(AUDIO, "Audio device invalidated. Stopping stream.");
						break;
					}
					continue; // No need to sleep since we wait for the event anyway
				}

				UINT32 frames_available = frames_in_buffer - padding;
				if (frames_available == 0)
				{
					continue;
				}

				BYTE *pData = nullptr;
				hr = m_renderer->GetBuffer(frames_available, &pData);
				if (FAILED(hr))
				{
					ERROR_LOG(AUDIO, "GetBuffer failed for %u frames: HRESULT %s", frames_available,
					          wasapi_hresult_to_string(hr).c_str());
					if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
					{
						ERROR_LOG(AUDIO, "Audio device invalidated during GetBuffer. Stopping stream.");
						break;
					}
					continue;
				}
				if (pData == nullptr)
				{
					ERROR_LOG(AUDIO, "GetBuffer succeeded but returned nullptr data pointer, aborting.");
					break;
				}

				m_mixer->Mix(reinterpret_cast<s16 *>(pData), frames_available);

				float volume = SConfig::GetInstance().m_IsMuted ? 0 : SConfig::GetInstance().m_Volume / 100.0f;
				s16 *samples = reinterpret_cast<s16 *>(pData);
				UINT32 num_shorts = frames_available * 2; // Stereo
				for (UINT32 i = 0; i < num_shorts; ++i)
				{
					samples[i] = static_cast<s16>(samples[i] * volume);
				}

				// Use AUDCLNT_BUFFERFLAGS_SILENT if the core is paused/stopped to prevent leftover noise.
				DWORD flags = (Core::GetState() != Core::CORE_RUN) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
				hr = m_renderer->ReleaseBuffer(frames_available, flags);
				if (FAILED(hr))
				{
					ERROR_LOG(AUDIO, "ReleaseBuffer failed: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
					if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
					{
						ERROR_LOG(AUDIO, "Audio device invalidated during ReleaseBuffer. Stopping stream.");
					}
					break; // Exit loop on any ReleaseBuffer failure
				}
			}
		}

		if (hTask != nullptr)
		{
			AvRevertMmThreadCharacteristics(hTask);
			INFO_LOG(AUDIO, "Reverted MM thread characteristics.");
		}
	}
}

// Direct push from the DMA thread (timer-driven exclusive WASAPI). Samples arrive in big-endian
// 16-bit stereo at input_sample_rate; we resample to 48 kHz, regulate the buffered audio level
// against m_margin_frames using a rolling 20-block minimum of pre-write padding, optionally
// mix in captured audio, apply volume and write to the WASAPI render buffer.
void WASAPIStream::FeedSamplesDirect(const s16 *samples, u32 num_samples, u32 input_sample_rate)
{
	std::lock_guard<std::mutex> lk(m_direct_path_mutex);

	if (!m_audio_client || !m_renderer || num_samples == 0 || input_sample_rate == 0)
		return;

	// On the very first push after stream start, write 10 ms of silence so the device has a
	// comfortable cushion before real audio arrives and avoids an underrun-induced stutter.
	if (!m_direct_path_primed)
	{
		m_direct_path_primed = true;
		constexpr u32 PRIME_FRAMES = 48000 / 100; // 10 ms @ 48 kHz = 480 frames
		u32 frames_to_prime = std::min(PRIME_FRAMES, frames_in_buffer);
		BYTE *prime_data = nullptr;
		if (SUCCEEDED(m_renderer->GetBuffer(frames_to_prime, &prime_data)) && prime_data != nullptr)
			m_renderer->ReleaseBuffer(frames_to_prime, AUDCLNT_BUFFERFLAGS_SILENT);
	}

	// Reset interpolation state on input-rate changes (e.g. AIDFR toggle).
	if (input_sample_rate != m_resample_last_input_rate)
	{
		m_resample_last_input_rate = input_sample_rate;
		m_resample_phase = 0.0f;
		m_resample_last_l = 0;
		m_resample_last_r = 0;
	}

	const u32 output_rate = fmt.Format.nSamplesPerSec; // always 48000
	const float ratio = static_cast<float>(input_sample_rate) / static_cast<float>(output_rate);

	// Generate output frames from input frames via linear interpolation. m_resample_last_*
	// holds input frame "n-1" carried over from the previous call; samples[] holds frames
	// starting at "n". m_resample_phase is the current fractional position within frame n.
	// For each output we read frame floor(idx) and frame floor(idx)+1, lerp by frac(idx).
	// We advance idx by ratio per output frame; when floor(idx) reaches num_samples we stop.

	// Pre-size scratch generously; we never produce more than ceil(num_samples/ratio)+2 frames,
	// plus one extra slot in case the margin logic duplicates the trailing frame.
	const u32 max_out_frames = static_cast<u32>(num_samples / ratio) + 4;
	std::vector<s16> out_buf(max_out_frames * 2);

	auto sample_at = [&](s32 input_idx, s16 *l, s16 *r)
	{
		if (input_idx < 0)
		{
			*l = m_resample_last_l;
			*r = m_resample_last_r;
		}
		else
		{
			// Input is big-endian stereo with right at even index, left at odd (matches AX
			// output convention used elsewhere in this codebase).
			s16 right = static_cast<s16>(Common::swap16(samples[input_idx * 2]));
			s16 left = static_cast<s16>(Common::swap16(samples[input_idx * 2 + 1]));
			*l = left;
			*r = right;
		}
	};

	u32 out_count = 0;
	float pos = m_resample_phase;
	while (true)
	{
		s32 base = static_cast<s32>(std::floor(pos));
		if (base >= static_cast<s32>(num_samples))
			break;
		float frac = pos - static_cast<float>(base);

		// Interpolate between input frame (base-1) and input frame (base). When base==0 the
		// "previous" frame is the carry-over from the previous push (m_resample_last_*).
		s16 l0, r0, l1, r1;
		sample_at(base - 1, &l0, &r0);
		sample_at(base, &l1, &r1);

		s16 left = static_cast<s16>((1.0f - frac) * l0 + frac * l1);
		s16 right = static_cast<s16>((1.0f - frac) * r0 + frac * r1);

		// Right at even index, left at odd — matches the downstream / capture-mix convention.
		out_buf[out_count * 2 + 0] = right;
		out_buf[out_count * 2 + 1] = left;
		++out_count;
		pos += ratio;
	}

	// Update carry-over state: m_resample_last_* becomes the last consumed input frame; the
	// new phase is pos mapped into the next call's coordinate frame (subtract num_samples).
	if (num_samples > 0)
	{
		s16 last_l, last_r;
		sample_at(static_cast<s32>(num_samples) - 1, &last_l, &last_r);
		m_resample_last_l = last_l;
		m_resample_last_r = last_r;
	}
	m_resample_phase = pos - static_cast<float>(num_samples);
	if (m_resample_phase < 0.0f)
		m_resample_phase = 0.0f; // safety

	if (out_count == 0)
		return;

	// --- Margin regulation: drop/duplicate one output frame per push ---
	// Read pre-write padding and update rolling minimum window.
	UINT32 padding = 0;
	if (FAILED(m_audio_client->GetCurrentPadding(&padding)))
		padding = 0;

	m_padding_history.push_back(padding);
	if (m_padding_history.size() > PADDING_HISTORY_SIZE)
		m_padding_history.pop_front();

	u32 min_padding = m_padding_history.front();
	for (u32 p : m_padding_history)
		if (p < min_padding)
			min_padding = p;

	// Publish the 20-block minimum for the on-screen audio margin display.
	AudioCommon::g_audio_min_margin_ms.store(
		static_cast<float>(min_padding) * 1000.0f / static_cast<float>(output_rate));

	const u32 one_ms_frames = output_rate / 1000; // 48 frames

	// Only correct once the history is full, otherwise we react to startup transients.
	if (m_padding_history.size() == PADDING_HISTORY_SIZE && out_count >= 2)
	{
		if (min_padding > m_margin_frames + one_ms_frames)
		{
			// Persistently too full — drop one output frame from this block.
			--out_count;
			// Bias the history so we don't repeatedly correct on the same evidence.
			for (u32 &p : m_padding_history)
				p = (p > 0) ? p - 1 : 0;
		}
		else if (min_padding + one_ms_frames < m_margin_frames)
		{
			// Persistently too empty — duplicate one output frame at the end of this block.
			out_buf[out_count * 2 + 0] = out_buf[(out_count - 1) * 2 + 0];
			out_buf[out_count * 2 + 1] = out_buf[(out_count - 1) * 2 + 1];
			++out_count;
			for (u32 &p : m_padding_history)
				p += 1;
		}
	}

	// --- Write to WASAPI buffer in chunks bounded by available space ---
	u32 written = 0;
	while (written < out_count)
	{
		UINT32 cur_padding = 0;
		if (FAILED(m_audio_client->GetCurrentPadding(&cur_padding)))
			break;
		u32 free_frames = (frames_in_buffer > cur_padding) ? (frames_in_buffer - cur_padding) : 0;
		if (free_frames == 0)
		{
			// Buffer is full — drop the rest of this block to avoid blocking the DMA thread.
			break;
		}
		u32 to_write = std::min(out_count - written, free_frames);

		BYTE *data = nullptr;
		HRESULT hr = m_renderer->GetBuffer(to_write, &data);
		if (FAILED(hr) || data == nullptr)
		{
			if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
				ERROR_LOG(AUDIO, "FeedSamplesDirect: device invalidated");
			break;
		}

		std::memcpy(data, &out_buf[written * 2], to_write * 2 * sizeof(s16));

		s16 *s16_data = reinterpret_cast<s16 *>(data);

		// Apply Dolphin volume to game samples first, before mixing in capture.
		// Pass-through audio is intentionally not subject to the Dolphin volume slider.
		float volume = SConfig::GetInstance().m_IsMuted ? 0.0f : SConfig::GetInstance().m_Volume / 100.0f;
		for (u32 i = 0; i < to_write * 2; ++i)
			s16_data[i] = static_cast<s16>(s16_data[i] * volume);

		if (m_audioCaptureType != AudioCaptureType::None)
			CaptureAudioAndMix(s16_data, to_write);

		DWORD flags = (Core::GetState() != Core::CORE_RUN) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
		m_renderer->ReleaseBuffer(to_write, flags);
		written += to_write;
	}
}


// Poll until the given endpoint can be opened in shared mode, or timeout.
// Probes by activating an IAudioClient and calling GetMixFormat: this exercises
// the same kernel path that fails with AUDCLNT_E_DEVICE_IN_USE while an exclusive
// stream still holds the endpoint. We never call Initialize(), so we never claim it.
static bool WaitUntilEndpointReady(LPCWSTR endpointId, DWORD timeoutMs = 2000, DWORD pollIntervalMs = 50)
{
	IMMDeviceEnumerator *enumerator = nullptr;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
	                              reinterpret_cast<void **>(&enumerator));
	if (FAILED(hr) || !enumerator)
	{
		ERROR_LOG(AUDIO, "WaitUntilEndpointReady: CoCreateInstance(MMDeviceEnumerator) failed");
		return false;
	}

	IMMDevice *device = nullptr;
	hr = enumerator->GetDevice(endpointId, &device);
	enumerator->Release();
	if (FAILED(hr) || !device)
	{
		ERROR_LOG(AUDIO, "WaitUntilEndpointReady: GetDevice failed");
		return false;
	}

	const DWORD deadline = GetTickCount() + timeoutMs;
	bool ready = false;
	DWORD attempts = 0;

	while (true)
	{
		IAudioClient *client = nullptr;
		hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
		if (SUCCEEDED(hr) && client)
		{
			WAVEFORMATEX *mixFmt = nullptr;
			HRESULT mixHr = client->GetMixFormat(&mixFmt);
			client->Release();
			if (mixFmt)
				CoTaskMemFree(mixFmt);

			if (SUCCEEDED(mixHr))
			{
				ready = true;
				break;
			}
		}

		++attempts;
		if (GetTickCount() >= deadline)
			break;

		Sleep(pollIntervalMs);
	}

	device->Release();

	if (ready)
		INFO_LOG(AUDIO, "WaitUntilEndpointReady: Endpoint ready after %lu poll(s).", attempts);
	else
		WARN_LOG(AUDIO, "WaitUntilEndpointReady: Endpoint did not become ready within %lu ms timeout.", timeoutMs);

	return ready;
}

void WASAPIStream::Stop()
{
	// Unregister the direct sink first so no new PushSamples calls land in FeedSamplesDirect.
	// The mixer's own mutex pairs with this so a push already in flight either fully completes
	// before ClearDirectSink returns, or won't observe the sink at all.
	if (m_mixer)
		m_mixer->ClearDirectSink();

	// Clear the OSD audio margin display.
	AudioCommon::g_audio_min_margin_ms.store(-1.0f);

	// Then take the direct-path mutex to ensure any still-in-flight FeedSamplesDirect call has
	// finished before we tear down the audio client and renderer it accesses.
	{
		std::lock_guard<std::mutex> lk(m_direct_path_mutex);
	}

	SoundStream::Stop(); // This should signal threadData to false and join the SoundLoop thread

	if (m_need_data_event)
	{
		CloseHandle(m_need_data_event);
		m_need_data_event = nullptr;
	}

	if (m_audio_client)
		m_audio_client->Stop();

	if (m_capture_audio_client)
	{
		m_capture_audio_client->Stop();
	}

	// Restore the exclusive endpoint's volume boost now, while m_mm_device is still valid.
	// This must happen before SAFE_RELEASE(m_mm_device) below.
	if (m_exclusive_mode && SConfig::GetInstance().m_SwitchDefaultAudioOutputDeviceDuringGameplay)
		RestoreVolumeIfNeeded();

	// Release the exclusive stream and all audio resources BEFORE restoring the default
	// endpoint, so that the endpoint is no longer held when other applications try to
	// re-open their streams on it. Hold the direct-path mutex while releasing m_audio_client
	// and m_renderer so that even if a push slipped through, it observes a coherent null state.
	{
		std::lock_guard<std::mutex> lk(m_direct_path_mutex);
		SAFE_RELEASE(m_renderer);
		SAFE_RELEASE(m_capture_client);
		SAFE_RELEASE(m_audio_client);
		SAFE_RELEASE(m_capture_audio_client);
		SAFE_RELEASE(m_mm_device);
	}

	if (m_exclusive_mode && SConfig::GetInstance().m_SwitchDefaultAudioOutputDeviceDuringGameplay)
	{
		// Wait until the original endpoint is no longer held exclusively before
		// switching the system default back to it. Without this wait, applications
		// that resume on the restored endpoint immediately after the switch may fail
		// to open their streams because the kernel hasn't yet recorded the exclusive
		// stream's release.
		if (!m_default_audio_device_id_prior_to_switch.empty())
			WaitUntilEndpointReady(m_default_audio_device_id_prior_to_switch.c_str());

		RestoreDefaultAudioOutputDevice();
	}
}

std::string lwpstrToString(LPCWSTR wstr)
{
	if (wstr == nullptr)
		return "";
	char buffer[2048];
	size_t ret = wcstombs(buffer, wstr, sizeof(buffer));
	if (ret == 2048)
		buffer[2047] = '\0';
	return std::string(buffer);
}

struct AudioDevice
{
	std::wstring id;
	std::string friendlyName;
};
std::vector<AudioDevice> GetAudioDevices(__MIDL___MIDL_itf_mmdeviceapi_0000_0000_0001 audioDeviceMode)
{
	HRESULT hr = S_OK;
	IMMDeviceEnumerator *mm_device_enumerator = nullptr;

	hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator),
		(void**)&mm_device_enumerator
	);

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Error in GetAudioDevices @ CoCreateInstance of MMDeviceEnumerator");
		return {};
	}

	IMMDeviceCollection* devices = nullptr;
	hr = mm_device_enumerator->EnumAudioEndpoints(audioDeviceMode, DEVICE_STATE_ACTIVE, &devices);

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Error in GetAudioDevices @ EnumAudioEndpoints");
		SAFE_RELEASE(mm_device_enumerator);
		return {};
	}

	UINT device_count = 0;
	devices->GetCount(&device_count);

	std::vector<AudioDevice> results;
	for (UINT i = 0; i < device_count; i++)
	{
		IMMDevice *device = nullptr;
		devices->Item(i, &device);

		IPropertyStore *pstore = nullptr;
		device->OpenPropertyStore(STGM_READ, &pstore);

		LPWSTR pwszID = NULL;
		device->GetId(&pwszID);
		std::wstring id{};
		if (pwszID)
		{
			id = pwszID;
			CoTaskMemFree(pwszID);
			pwszID = nullptr;
		}

		PROPVARIANT name_prop;
		PropVariantInit(&name_prop);
		pstore->GetValue(PKEY_Device_FriendlyName, &name_prop);
		std::string name_stdstr = lwpstrToString(name_prop.pwszVal);

		for (int j = 0; j <= 9; j++)
		{
			if (name_stdstr.substr(0, std::string("0 - ").size()) == std::to_string(j) + " - ")
				name_stdstr = name_stdstr.substr(std::string("0 - ").size()) + " [" + std::to_string(j) + "]";
		}
		results.push_back({id, name_stdstr});

		PropVariantClear(&name_prop);
		SAFE_RELEASE(pstore);
		SAFE_RELEASE(device);
	}

	SAFE_RELEASE(devices);
	SAFE_RELEASE(mm_device_enumerator);

	return results;
}

std::vector<std::string> WASAPIStream::GetRenderDeviceNames()
{
	std::vector<std::string> v{};
	for (const auto &device : GetAudioDevices(eRender))
	{
		v.push_back(device.friendlyName);
	}
	return v;
}

std::vector<std::string> WASAPIStream::GetCaptureDeviceNames()
{
	std::vector<std::string> v{};
	for (const auto &device : GetAudioDevices(eCapture))
	{
		v.push_back(device.friendlyName);
	}
	return v;
}

bool WASAPIStream::InitializeCaptureClient()
{
	if (!m_exclusive_mode || m_audioCaptureType == AudioCaptureType::None)
		return true;

	HRESULT hr;
	IMMDeviceEnumerator* enumerator = nullptr;
	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
	                      (void **)&enumerator);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "Failed to create IMMDeviceEnumerator for capture: HRESULT %s",
		          wasapi_hresult_to_string(hr).c_str());
		return false;
	}

	IMMDevice *capture_device_ptr = nullptr; // Renamed to avoid confusion with class member
	std::string selected_device_name = m_audioCaptureType == AudioCaptureType::Recording
	                                               ? SConfig::GetInstance().sAudioInputDevice
	                                               : SConfig::GetInstance().sAudioLoopedBackOutputDevice;

	IMMDeviceCollection *devices = nullptr;
	hr = enumerator->EnumAudioEndpoints(
		m_audioCaptureType == AudioCaptureType::Recording ? eCapture : eRender, DEVICE_STATE_ACTIVE, &devices);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "Failed to enumerate audio capture endpoints: HRESULT %s",
		          wasapi_hresult_to_string(hr).c_str());
		SAFE_RELEASE(enumerator);
		return false;
	}

	DEBUG_LOG(AUDIO, (m_audioCaptureType == AudioCaptureType::Recording ? "Selected capture device: %s" : "Selected loopback device: %s"),
	          selected_device_name.c_str());
	UINT count;
	devices->GetCount(&count);
	for (UINT i = 0; i < count; ++i)
	{
		IMMDevice *device_item = nullptr;
		devices->Item(i, &device_item);

		IPropertyStore *store = nullptr;
		device_item->OpenPropertyStore(STGM_READ, &store);

		PROPVARIANT name_prop; // Renamed to avoid conflict
		PropVariantInit(&name_prop);
		store->GetValue(PKEY_Device_FriendlyName, &name_prop);

		char name_cstr[2048];
		size_t ret_capture_name; // Renamed

		ret_capture_name = wcstombs(name_cstr, name_prop.pwszVal, sizeof(name_cstr));
		if (ret_capture_name == 2048)
			name_cstr[2047] = '\0';

		std::string name_stdstr = name_cstr;

		for (int j = 0; j <= 9; j++)
		{
			if (name_stdstr.substr(0, std::string("0 - ").size()) == std::to_string(j) + " - ")
				name_stdstr = name_stdstr.substr(std::string("0 - ").size()) + " [" + std::to_string(j) + "]";
		}

		DEBUG_LOG(AUDIO, "Checking device: %s", name_stdstr.c_str());
		if (name_stdstr == selected_device_name)
		{
			capture_device_ptr = device_item; // Assign, will be released later
			INFO_LOG(AUDIO, "Found matching device: %s", name_stdstr.c_str());
		}

		PropVariantClear(&name_prop);
		SAFE_RELEASE(store);
		if (capture_device_ptr != device_item)
			SAFE_RELEASE(device_item);
	}
	SAFE_RELEASE(devices);

	if (!capture_device_ptr)
	{
		ERROR_LOG(AUDIO, "Selected device not found: %s", selected_device_name.c_str());
		SAFE_RELEASE(enumerator);
		return false;
	}

	hr = capture_device_ptr->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&m_capture_audio_client);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "Failed to activate device: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		SAFE_RELEASE(capture_device_ptr);
		SAFE_RELEASE(enumerator);
		// m_capture_audio_client is not valid
		return false;
	}

	captureFormat = {0};
	captureFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	captureFormat.Format.nChannels = 2;
	captureFormat.Format.nSamplesPerSec = 48000;
	captureFormat.Format.nAvgBytesPerSec = captureFormat.Format.nSamplesPerSec * 4;
	captureFormat.Format.nBlockAlign = 4;
	captureFormat.Format.wBitsPerSample = 16;
	captureFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	captureFormat.Samples.wValidBitsPerSample = captureFormat.Format.wBitsPerSample;
	captureFormat.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	captureFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

	auto capture_format_ptr = reinterpret_cast<WAVEFORMATEX *>(&captureFormat);

	// Require 16bits 48000hz PCM from the capture device (the capture device may not support it,
	// but because we use shared mode, the mixer translation layer will handle that)
	DWORD dwStreamFlags = m_audioCaptureType == AudioCaptureType::Recording ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
	hr = m_capture_audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, dwStreamFlags, 0, 0, capture_format_ptr, nullptr);
	
	if (FAILED(hr))
	{
		if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
		{
			ERROR_LOG(AUDIO, "Error - capture/loopback device doesn't support the required stereo 16-bit 48000 Hz PCM format.");
		}
		else
		{
			ERROR_LOG(AUDIO, "Failed to initialize capture client: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		}
		SAFE_RELEASE(m_capture_audio_client);
		SAFE_RELEASE(capture_device_ptr);
		SAFE_RELEASE(enumerator);
		return false;
	}

	hr = m_capture_audio_client->GetService(__uuidof(IAudioCaptureClient), (void **)&m_capture_client);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "Failed to get IAudioCaptureClient service: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		SAFE_RELEASE(m_capture_audio_client);
		SAFE_RELEASE(capture_device_ptr);
		SAFE_RELEASE(enumerator);
		return false;
	}

	hr = m_capture_audio_client->Start();
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "Failed to start capture client: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		SAFE_RELEASE(m_capture_client);
		SAFE_RELEASE(m_capture_audio_client);
		SAFE_RELEASE(capture_device_ptr);
		SAFE_RELEASE(enumerator);
		return false;
	}

	SAFE_RELEASE(capture_device_ptr);
	SAFE_RELEASE(enumerator);
	return true;
}

void WASAPIStream::CaptureAudioAndMix(s16 *mix_buffer, u32 num_frames_to_render_target)
{
	if (!m_capture_client || m_audioCaptureType == AudioCaptureType::None
		|| !m_capture_audio_client)
	{
		return;
	}

	/* The capture API is such that we need to always retrieve the full buffer, or not at all.
	Because the full buffer for capture is not guaranteed to be the same size as the render buffer,
	that means we can't just ask the capture stream to give us the number of samples we need: we 
	have to handle buffering ourselves, this is what the deque is about.*/

	// If the hardware +6 dB boost was successfully applied to the exclusive endpoint, we halve
	// both signals before summing so the sum fits in s16 with no risk of overflow (the boost
	// compensates perceptually). If the boost was not achievable we instead do a straight
	// saturating add, accepting the rare risk of clipping over a guaranteed permanent volume drop.
	u32 num_shorts_to_render_target = num_frames_to_render_target * 2; // Stereo samples

	u32 shorts_mixed_this_cycle = 0;

	auto mix_one_sample = [&](s16 &dest, s16 src)
	{
		if (m_volume_boost_applied)
		{
			// (A + B) / 2 in 32-bit: one divide on the sum preserves the bit that
			// (A/2) + (B/2) would discard when both LSBs are 1.
			int32_t sum = static_cast<int32_t>(dest) + static_cast<int32_t>(src);
			dest = static_cast<s16>(sum / 2);
		}
		else
		{
			// Saturating add: clamp the result to the s16 range.
			int32_t sum = static_cast<int32_t>(dest) + static_cast<int32_t>(src);
			dest = static_cast<s16>(std::max(-32768, std::min(32767, sum)));
		}
	};

	// Phase 1: Consume from our internal buffer first
	while (shorts_mixed_this_cycle < num_shorts_to_render_target && !m_internal_capture_buffer.empty())
	{
		mix_one_sample(mix_buffer[shorts_mixed_this_cycle], m_internal_capture_buffer.front());
		m_internal_capture_buffer.pop_front();
		shorts_mixed_this_cycle++;
	}

	// Phase 2: If still need more data for the current render target, fetch new packets from the device
	while (shorts_mixed_this_cycle < num_shorts_to_render_target)
	{
		UINT32 packet_size_in_frames = 0;
		HRESULT hr = m_capture_client->GetNextPacketSize(&packet_size_in_frames);

		if (FAILED(hr))
		{
			ERROR_LOG(AUDIO, "Capture: Failed to get next packet size: HRESULT %s",
			          wasapi_hresult_to_string(hr).c_str());
			if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
			{
				WARN_LOG(AUDIO, "Capture device invalidated. Disabling further capture for this session.");
				SAFE_RELEASE(m_capture_client); // Prevent further calls
			}
			return;
		}

		if (packet_size_in_frames == 0)
		{
			break; // No more data currently available from device
		}

		BYTE *p_capture_data_packet = nullptr;
		UINT32 frames_in_packet = 0; // This will be set by GetBuffer to the actual packet size
		DWORD packet_flags = 0;

		hr = m_capture_client->GetBuffer(&p_capture_data_packet, &frames_in_packet, &packet_flags, nullptr, nullptr);

		if (hr == AUDCLNT_S_BUFFER_EMPTY)
		{
			// This should ideally be caught by packet_size_in_frames == 0 from GetNextPacketSize.
			// If it happens, just try to get the next packet size again in the next loop iteration (if any).
			continue;
		}
		if (FAILED(hr))
		{
			ERROR_LOG(AUDIO, "Capture: Failed to get buffer: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
			{
				WARN_LOG(AUDIO, "Capture device invalidated during GetBuffer. Disabling further capture.");
				SAFE_RELEASE(m_capture_client);
			}
			return;
		}

		if (!(packet_flags & AUDCLNT_BUFFERFLAGS_SILENT) && p_capture_data_packet != nullptr)
		{
			s16 *s16_capture_data = reinterpret_cast<s16 *>(p_capture_data_packet);
			u32 shorts_in_packet = frames_in_packet * 2; // Stereo samples in the captured packet

			for (u32 i = 0; i < shorts_in_packet; ++i)
			{
				if (shorts_mixed_this_cycle < num_shorts_to_render_target)
				{
					mix_one_sample(mix_buffer[shorts_mixed_this_cycle], s16_capture_data[i]);
					shorts_mixed_this_cycle++;
				}
				else
				{
					// We have filled the render buffer for this cycle, buffer the rest internally
					m_internal_capture_buffer.push_back(s16_capture_data[i]);
				}
			}
		}
		// If AUDCLNT_BUFFERFLAGS_SILENT is set, or p_capture_data_packet is null (though GetBuffer S_OK should ensure
		// it's not), we effectively mix silence (or rather, don't add anything to the already halved mix_buffer).
		hr = m_capture_client->ReleaseBuffer(frames_in_packet);
		if (FAILED(hr))
		{
			ERROR_LOG(AUDIO, "Capture: Failed to release buffer: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
			{
				WARN_LOG(AUDIO, "Capture device invalidated during ReleaseBuffer. Disabling further capture.");
				SAFE_RELEASE(m_capture_client);
			}
			return;
		}
	}
	// At this point, mix_buffer has been processed up to shorts_mixed_this_cycle.
	// Any remaining part of mix_buffer (if shorts_mixed_this_cycle < num_shorts_to_render_target)
	// will not have captured audio mixed in for this call if no more packets were available.
}

void WASAPIStream::SwitchDefaultAudioOutputDeviceByDeviceNameAndStorePriorDeviceId(const std::string& device_name, bool doubleVolume)
{
	HRESULT hr = S_OK;
	IMMDeviceEnumerator *pEnumerator = NULL;

	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
						  (void **)&pEnumerator);

	if (FAILED(hr))
		return;

	// Store the current default console endpoint ID so we can switch back on Stop().
	IMMDevice *pCurrentDefaultConsoleDevice = NULL;
	LPWSTR pwszDefaultConsoleId = NULL;
	if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pCurrentDefaultConsoleDevice)))
	{
		pCurrentDefaultConsoleDevice->GetId(&pwszDefaultConsoleId);
		if (pwszDefaultConsoleId)
		{
			m_default_audio_device_id_prior_to_switch = std::wstring(pwszDefaultConsoleId);
			m_pending_audio_device_switch_back = true;
			CoTaskMemFree(pwszDefaultConsoleId);
			pwszDefaultConsoleId = nullptr;
		}
		SAFE_RELEASE(pCurrentDefaultConsoleDevice);
	}
	else
	{
		SAFE_RELEASE(pEnumerator);
		return;
	}

	// Determine if the communications endpoint currently equals the console default.
	// If not, we leave it alone so unrelated communication apps are not disrupted.
	bool switch_comms = false;
	IMMDevice *pCurrentDefaultCommsDevice = NULL;
	LPWSTR pwszDefaultCommsId = NULL;
	if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eCommunications, &pCurrentDefaultCommsDevice)))
	{
		pCurrentDefaultCommsDevice->GetId(&pwszDefaultCommsId);
		if (pwszDefaultCommsId)
		{
			switch_comms = (std::wstring(pwszDefaultCommsId) == m_default_audio_device_id_prior_to_switch);
			CoTaskMemFree(pwszDefaultCommsId);
		}
		SAFE_RELEASE(pCurrentDefaultCommsDevice);
	}

	// Find the sacrificial endpoint and set its volume to 100% so the loopback capture gets
	// a full-scale signal. Store the original volume for restore on Stop().
	m_should_restore_sacrificial_volume = false;
	std::vector<AudioDevice> audioDevices = GetAudioDevices(eRender);
	auto findResult = std::find_if(audioDevices.begin(), audioDevices.end(), [&device_name](const AudioDevice &device)
								   { return device.friendlyName == device_name; });
	if (findResult != audioDevices.end())
	{
		// Open the sacrificial device and set its endpoint volume to max (0 dB / 100%).
		IMMDeviceEnumerator *pEnum2 = NULL;
		IMMDevice *pSacrificialDevice = NULL;
		CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&pEnum2);
		if (pEnum2)
		{
			IMMDeviceCollection *pAll = NULL;
			pEnum2->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pAll);
			if (pAll)
			{
				UINT count = 0;
				pAll->GetCount(&count);
				for (UINT i = 0; i < count; ++i)
				{
					IMMDevice *pDev = NULL;
					pAll->Item(i, &pDev);
					LPWSTR pId = NULL;
					pDev->GetId(&pId);
					if (pId && std::wstring(pId) == findResult->id)
					{
						pSacrificialDevice = pDev;
						CoTaskMemFree(pId);
						break;
					}
					CoTaskMemFree(pId);
					SAFE_RELEASE(pDev);
				}
				SAFE_RELEASE(pAll);
			}
			SAFE_RELEASE(pEnum2);
		}

		if (pSacrificialDevice)
		{
			IAudioEndpointVolume *pSacrifVol = nullptr;
			if (SUCCEEDED(pSacrificialDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL,
													   (void **)&pSacrifVol)) && pSacrifVol)
			{
				float minDB, maxDB, stepDB;
				float currentDB;
				if (SUCCEEDED(pSacrifVol->GetMasterVolumeLevel(&currentDB)) &&
					SUCCEEDED(pSacrifVol->GetVolumeRange(&minDB, &maxDB, &stepDB)))
				{
					m_sacrificial_device_original_volume_db = currentDB;
					m_should_restore_sacrificial_volume = true;
					if (SUCCEEDED(pSacrifVol->SetMasterVolumeLevel(maxDB, nullptr)))
						INFO_LOG(AUDIO, "Sacrificial endpoint volume set to %.2f dB (was %.2f dB)", maxDB, currentDB);
					else
						WARN_LOG(AUDIO, "Failed to set sacrificial endpoint volume to max.");
				}
				SAFE_RELEASE(pSacrifVol);
			}
			SAFE_RELEASE(pSacrificialDevice);
		}

		// Switch the default endpoints to the sacrificial device.
		IPolicyConfig *pPolicyConfig = NULL;
		hr = CoCreateInstance(_uuidof(CPolicyConfigClient), NULL, CLSCTX_INPROC_SERVER, _uuidof(IPolicyConfig),
							  (LPVOID *)&pPolicyConfig);
		if (SUCCEEDED(hr) && pPolicyConfig)
		{
			pPolicyConfig->SetDefaultEndpoint(findResult->id.c_str(), eConsole);
			pPolicyConfig->SetDefaultEndpoint(findResult->id.c_str(), eMultimedia);
			if (switch_comms)
				pPolicyConfig->SetDefaultEndpoint(findResult->id.c_str(), eCommunications);
			SAFE_RELEASE(pPolicyConfig);
		}
		else
		{
			ERROR_LOG(AUDIO, "Failed to switch default audio output device: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		}
	}

	SAFE_RELEASE(pEnumerator);
}
void WASAPIStream::RestoreDefaultAudioOutputDevice()
{
	if (m_pending_audio_device_switch_back)
	{
		// Restore the sacrificial endpoint's volume before switching back, so it returns to
		// its original level as soon as other apps resume playing through it.
		if (m_should_restore_sacrificial_volume)
		{
			std::vector<AudioDevice> audioDevices = GetAudioDevices(eRender);
			// The sacrificial device is the one currently set as the system default.
			IMMDeviceEnumerator *pEnum = NULL;
			IMMDevice *pSacrificialDevice = NULL;
			CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&pEnum);
			if (pEnum)
			{
				pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pSacrificialDevice);
				SAFE_RELEASE(pEnum);
			}
			if (pSacrificialDevice)
			{
				IAudioEndpointVolume *pSacrifVol = nullptr;
				if (SUCCEEDED(pSacrificialDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL,
														   (void **)&pSacrifVol)) && pSacrifVol)
				{
					if (SUCCEEDED(pSacrifVol->SetMasterVolumeLevel(m_sacrificial_device_original_volume_db, nullptr)))
						INFO_LOG(AUDIO, "Sacrificial endpoint volume restored to %.2f dB", m_sacrificial_device_original_volume_db);
					else
						WARN_LOG(AUDIO, "Failed to restore sacrificial endpoint volume.");
					SAFE_RELEASE(pSacrifVol);
				}
				SAFE_RELEASE(pSacrificialDevice);
			}
			m_should_restore_sacrificial_volume = false;
		}

		IPolicyConfig *pPolicyConfig = NULL;
		HRESULT hr = CoCreateInstance(_uuidof(CPolicyConfigClient), NULL, CLSCTX_INPROC_SERVER, _uuidof(IPolicyConfig),
									  (LPVOID *)&pPolicyConfig);
		if (FAILED(hr))
		{
			ERROR_LOG(AUDIO, "Failed to switch default audio output device: HRESULT %s",
					  wasapi_hresult_to_string(hr).c_str());
			return;
		}

		pPolicyConfig->SetDefaultEndpoint(m_default_audio_device_id_prior_to_switch.c_str(), eConsole);
		pPolicyConfig->SetDefaultEndpoint(m_default_audio_device_id_prior_to_switch.c_str(), eMultimedia);
		pPolicyConfig->SetDefaultEndpoint(m_default_audio_device_id_prior_to_switch.c_str(), eCommunications);

		m_pending_audio_device_switch_back = false;

		SAFE_RELEASE(pPolicyConfig);
	}
}

// Attempts to apply db_change to the master volume of m_mm_device.
// Returns true if the full requested change was applied (i.e. the target was within range),
// false if the device was already at its limit and the change could not be fully applied.
bool WASAPIStream::AlterVolumeOfAudioDevice(float db_change, bool store_original_and_set_flag)
{
	if (!m_mm_device)
	{
		WARN_LOG(AUDIO, "AlterVolumeOfAudioDevice: m_mm_device not set.");
		return false;
	}

	IAudioEndpointVolume *pEndpointVolume = nullptr;
	HRESULT hr =
		m_mm_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, (void **)&pEndpointVolume);

	if (FAILED(hr) || !pEndpointVolume)
	{
		ERROR_LOG(AUDIO, "Could not activate IAudioEndpointVolume. Volume won't be adjusted. HRESULT: %s",
				  wasapi_hresult_to_string(hr).c_str());
		SAFE_RELEASE(pEndpointVolume);
		return false;
	}

	float currentVolumeDB, minDB, maxDB, stepDB;

	hr = pEndpointVolume->GetMasterVolumeLevel(&currentVolumeDB);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "Could not get current master volume level (dB). Volume won't be adjusted. HRESULT: %s",
				  wasapi_hresult_to_string(hr).c_str());
		SAFE_RELEASE(pEndpointVolume);
		return false;
	}

	hr = pEndpointVolume->GetVolumeRange(&minDB, &maxDB, &stepDB);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "Could not get device volume range (dB). Volume won't be adjusted. HRESULT: %s",
				  wasapi_hresult_to_string(hr).c_str());
		SAFE_RELEASE(pEndpointVolume);
		return false;
	}

	INFO_LOG(AUDIO, "Device volume range: min=%.2fdB, max=%.2fdB, step=%.2fdB", minDB, maxDB, stepDB);

	if (store_original_and_set_flag)
	{
		m_original_volume_db = currentVolumeDB;
		INFO_LOG(AUDIO, "Stored original device volume: %.2fdB", m_original_volume_db);
	}

	float targetVolumeDB = currentVolumeDB + db_change;
	bool full_change_achievable = (targetVolumeDB <= maxDB);

	targetVolumeDB = std::max(minDB, std::min(targetVolumeDB, maxDB));

	if (stepDB > 0.0f)
		targetVolumeDB = std::round(targetVolumeDB / stepDB) * stepDB;

	if (std::abs(targetVolumeDB - currentVolumeDB) > (stepDB / 2.0f))
	{
		hr = pEndpointVolume->SetMasterVolumeLevel(targetVolumeDB, nullptr);
		if (SUCCEEDED(hr))
		{
			if (store_original_and_set_flag)
				INFO_LOG(AUDIO, "Device volume compensation: old=%.2fdB, new=%.2fdB", m_original_volume_db, targetVolumeDB);
			else
				INFO_LOG(AUDIO, "Device volume restored to: %.2fdB", targetVolumeDB);
		}
		else
		{
			WARN_LOG(AUDIO, "Failed to set device volume to %.2fdB. HRESULT: %s", targetVolumeDB,
					 wasapi_hresult_to_string(hr).c_str());
			SAFE_RELEASE(pEndpointVolume);
			return false;
		}
	}
	else
	{
		INFO_LOG(AUDIO, "Device volume already at target or very close (Current: %.2fdB, Target: %.2fdB), no change made.",
				 currentVolumeDB, targetVolumeDB);
	}

	if (store_original_and_set_flag)
		m_should_switch_volume_back = true;

	SAFE_RELEASE(pEndpointVolume);
	return full_change_achievable;
}

void WASAPIStream::RestoreVolumeIfNeeded()
{
	if (m_should_switch_volume_back)
	{
		if (m_mm_device)
		{
			DEBUG_LOG(AUDIO, "Attempting to restore original device volume of %.2fdB.", m_original_volume_db);

			IAudioEndpointVolume *pEndpointVolume = nullptr;
			HRESULT hr = m_mm_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL,
			                                   (void **)&pEndpointVolume);
			if (FAILED(hr) || !pEndpointVolume)
			{
				ERROR_LOG(AUDIO,
				          "RestoreVolume: Could not activate IAudioEndpointVolume. Original volume may not be "
				          "restored. HRESULT: %s",
				          wasapi_hresult_to_string(hr).c_str());
				SAFE_RELEASE(pEndpointVolume);
				m_should_switch_volume_back = false;
				return;
			}

			hr = pEndpointVolume->SetMasterVolumeLevel(m_original_volume_db, nullptr);
			if (SUCCEEDED(hr))
			{
				INFO_LOG(AUDIO, "Successfully restored original device volume to %.2fdB.", m_original_volume_db);
			}
			else
			{
				WARN_LOG(AUDIO, "Failed to restore original device volume to %.2fdB. HRESULT: %s", m_original_volume_db,
				         wasapi_hresult_to_string(hr).c_str());
			}
			SAFE_RELEASE(pEndpointVolume);
		}
		else
		{
			WARN_LOG(AUDIO, "RestoreVolume: No m_mm_device available to restore volume.");
		}
		m_should_switch_volume_back = false;
	}
}