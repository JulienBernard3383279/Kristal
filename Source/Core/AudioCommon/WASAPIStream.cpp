// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "AudioCommon/WASAPIStream.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "VideoCommon/OnScreenDisplay.h"

#include <mmdeviceapi.h>
#include <avrt.h>

#include <locale>

static std::string wasapi_hresult_to_string(HRESULT res)
{
	switch(res)
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

#define SAFE_RELEASE(p) { if ((p)) { (p)->Release(); (p)=nullptr; } }

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


// TODO
// Ensure volume control works in both modes
// Ensure change of output device works in shared mode (see Windows SDK samples linked in https://learn.microsoft.com/en-us/windows/win32/coreaudio/stream-routing)

bool WASAPIStream::Start()
{
	HRESULT hr = S_OK;
	IMMDeviceEnumerator* mm_device_enumerator;

	hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator),
		(void**)&mm_device_enumerator
	);

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Error @ CoCreateInstance of MMDeviceEnumerator");
		return false;
	}

	IMMDevice* mm_device = nullptr;

	std::string lower_device = "";
	for(char c : m_selected_device)
		lower_device += std::tolower(c, std::locale::classic());

	if(lower_device.find("default") != std::string::npos)
		hr = mm_device_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &mm_device);
	else
	{
		IMMDeviceCollection* devices = nullptr;
		hr = mm_device_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);

		UINT device_count = 0;
		devices->GetCount(&device_count);

		for(UINT i = 0; i < device_count; i++)
		{
			IMMDevice* device;
			devices->Item(i, &device);

			IPropertyStore* pstore;
			device->OpenPropertyStore(STGM_READ, &pstore);

			PROPVARIANT name_prop;
			PropVariantInit(&name_prop);

			pstore->GetValue(PKEY_Device_FriendlyName, &name_prop);

			char name_cstr[2048];
			size_t ret;

			ret = wcstombs(name_cstr, name_prop.pwszVal, sizeof(name_cstr));
			if(ret == 2048) name_cstr[2047] = '\0';

			std::string name_stdstr = name_cstr;

			for(int i = 0; i <= 9; i++)
			{
				if(name_stdstr.substr(0, std::string("0 - ").size()) == std::to_string(i) + " - ")
					name_stdstr = name_stdstr.substr(std::string("0 - ").size()) + " [" + std::to_string(i) + "]";
			}

			// if(name_stdstr.size() > 40)
			//     name_stdstr = name_stdstr.substr(0, 40) + "...";
			// needs to be preserved for uniqueness

			if(name_stdstr == m_selected_device)
				mm_device = device;

			PropVariantClear(&name_prop);

			pstore->Release();

			if(mm_device != device)
				device->Release();
		}

		devices->Release();
	}

	if(mm_device == nullptr)
	{
		OSD::AddMessage("Invalid audio device \"" + m_selected_device + "\" selected for WASAPI. Check your backend settings.", 6000U);
		return false;
	}

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Error @ MMDeviceEnumerator::GetDefaultAudioEndpoint");
		return false;
	}

	hr = mm_device->Activate(
		__uuidof(IAudioClient),
		CLSCTX_ALL, NULL,
		(void**)&m_audio_client
	);

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Error @ MMDeviceEnumerator -> IAudioClient");
		return false;
	}

	LPWSTR id;
	mm_device->GetId(&id);

	char buffer[2048];
	size_t ret;

	ret = wcstombs(buffer, id, sizeof(buffer));
	if(ret == 2048) buffer[2047] = '\0';

	INFO_LOG(AUDIO, "WASAPIStream: Using device %s", buffer);

	fmt = { 0 };
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

	/* WAVEFORMATEX* fmtex;
	m_audio_client->GetMixFormat(&fmtex);
	fmt = *reinterpret_cast<PWAVEFORMATEXTENSIBLE>(fmtex); */

	#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
	#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000


	REFERENCE_TIME default_legacy_device_period;
	REFERENCE_TIME minimum_legacy_device_period;
	hr = m_audio_client->GetDevicePeriod(&default_legacy_device_period, &minimum_legacy_device_period);

	REFERENCE_TIME exclusive_device_period = minimum_legacy_device_period;

	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get minimum device period.");

		m_audio_client->Release();
		m_audio_client = nullptr;

		mm_device_enumerator->Release();
		mm_device->Release();

		return false;
	}

	DWORD exclusiveStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
	DWORD sharedStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	auto format = reinterpret_cast<WAVEFORMATEX *>(&fmt);

	if (m_exclusive_mode)
	{
		// Exclusive mode => Use AudioClient1 => legacy device periods (can get short buffers anyway)

		exclusive_device_period += SConfig::GetInstance().iLatency * (10000 / fmt.Format.nChannels);

		hr = m_audio_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveStreamFlags, exclusive_device_period,
		                                exclusive_device_period, format, nullptr);

		if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
			OSD::AddMessage("Your current audio device doesn't support 16-bit 48000 hz PCM audio. WASAPI exclusive "
			                "mode won't work.",
			                6000U);

		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			INFO_LOG(AUDIO, "WASAPIStream: Device period not aligned, attempting to fix...");

			hr = m_audio_client->GetBufferSize(&frames_in_buffer);
			m_audio_client->Release();

			if (FAILED(hr))
			{
				ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
				ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get buffer size for alignment.");

				m_audio_client = nullptr;
				mm_device_enumerator->Release();
				mm_device->Release();

				return false;
			}

			exclusive_device_period =
			    static_cast<REFERENCE_TIME>(10000.0 * 1000 * frames_in_buffer / fmt.Format.nSamplesPerSec + 0.5) +
			    SConfig::GetInstance().iLatency * 10000;

			hr = mm_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&m_audio_client);

			if (FAILED(hr))
			{
				ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
				ERROR_LOG(AUDIO, "WASAPIStream: Error @ MMDeviceEnumerator -> IAudioClient");

				mm_device_enumerator->Release();
				mm_device->Release();

				return false;
			}

			hr = m_audio_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveStreamFlags,
			                                exclusive_device_period, exclusive_device_period, format, nullptr);
		}
	}
	else
	{
		// Shared mode => Use AudioClient3 => new device periods (Windows 10+ exclusive, enables short buffers)
		
		IAudioClient3 *audio_client_3 = nullptr;
		hr = m_audio_client->QueryInterface(__uuidof(IAudioClient3), (void **)&audio_client_3);

		if (FAILED(hr))
		{
			std::string message = "QueryInterface for IAudioClient3 failed: " + wasapi_hresult_to_string(hr) + ". Falling back to legacy Initialize.\n";
			ERROR_LOG(AUDIO, message.c_str());

			// If no IAudioClient3 interface (e.g. W7), fall back to legacy Initialize
			hr = m_audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, sharedStreamFlags, default_legacy_device_period,
			                                0, format, nullptr);
		}
		else
		{
			UINT32 defaultPeriod, fundamentalPeriod, minPeriod, maxPeriod{};
			hr = audio_client_3->GetSharedModeEnginePeriod(format, &defaultPeriod, &fundamentalPeriod, &minPeriod,
			                                               &maxPeriod);
			if (SUCCEEDED(hr))
			{
				std::ostringstream oss{};
				oss << "Shared Mode Engine Periods (frames @ " << format->nSamplesPerSec << " Hz):\n";
				oss << "  Default:     " << defaultPeriod << " ("
				    << (double)defaultPeriod * 1000.0 / format->nSamplesPerSec << " ms)\n";
				oss << "  Fundamental: " << fundamentalPeriod << " ("
				    << (double)fundamentalPeriod * 1000.0 / format->nSamplesPerSec << " ms)\n";
				oss << "  Min:         " << minPeriod << " (" << (double)minPeriod * 1000.0 / format->nSamplesPerSec
				    << " ms)\n";
				oss << "  Max:         " << maxPeriod << " (" << (double)maxPeriod * 1000.0 / format->nSamplesPerSec
				    << " ms)\n";

				INFO_LOG(AUDIO, oss.str().c_str());

				OSD::AddMessage(std::string{"Using audio engine period: " +
				                            std::to_string((double)minPeriod * 1000.0 / format->nSamplesPerSec) +
				                            " ms (Shared WASAPI)"},
				                10000U);
			}
			else
			{
				minPeriod = 0; // 0 means use default period
				ERROR_LOG(AUDIO,
				          std::string{"GetSharedModeEnginePeriod failed: " + wasapi_hresult_to_string(hr)}.c_str());
			}

			hr = audio_client_3->InitializeSharedAudioStream(sharedStreamFlags, minPeriod, format, nullptr);
			if (FAILED(hr))
			{
				ERROR_LOG(AUDIO,
				          std::string{"InitializeSharedAudioStream failed: " + wasapi_hresult_to_string(hr)}.c_str());
			}
			SAFE_RELEASE(audio_client_3);
		}
	}

	mm_device_enumerator->Release();
	mm_device->Release();

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());

		m_audio_client->Release();
		m_audio_client = nullptr;

		return false;
	}

	hr = m_audio_client->GetBufferSize(&frames_in_buffer);
	if (FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get buffer size.");

		m_audio_client = nullptr;
		m_audio_client->Release();

		return false;
	}
	else
	{
		double bufferInMs = (double)frames_in_buffer * 1000.0 / fmt.Format.nSamplesPerSec;
		INFO_LOG(AUDIO, "WASAPIStream: Buffer size: %u frames; %f ms", frames_in_buffer);

		OSD::AddMessage("Effective audio buffer size: " + std::to_string(bufferInMs) + " ms",
		                10000U);
	}

	m_need_data_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	m_audio_client->SetEventHandle(m_need_data_event);

	hr = m_audio_client->GetService(
		__uuidof(IAudioRenderClient),
		(void**)&m_renderer
	);

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't get IAudioClient renderer.");

		CloseHandle(m_need_data_event);
		m_audio_client->Release();

		m_need_data_event = nullptr;
		m_audio_client = nullptr;
		return false;
	}

	hr = m_audio_client->Start();

	if(FAILED(hr))
	{
		ERROR_LOG(AUDIO, "WASAPIStream: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
		ERROR_LOG(AUDIO, "WASAPIStream: Couldn't start audio client.");

		CloseHandle(m_need_data_event);
		m_renderer->Release();
		m_audio_client->Release();

		m_need_data_event = nullptr;
		m_renderer = nullptr;
		m_audio_client = nullptr;
		return false;
	}

	SoundStream::Start();
	return true;
}

void WASAPIStream::SoundLoop()
{
	if(m_audio_client && m_renderer && m_need_data_event)
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
			//SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		}
		else
		{
			INFO_LOG(AUDIO, "Set MM thread characteristics to Pro Audio.");
		}


		if (m_exclusive_mode)
		{
			// In event driven exclusive mode, GetCurrentPadding doesn't work and musn't be used; we're always given a buffer to fill completely.

			u8 *data = nullptr;

			m_renderer->GetBuffer(frames_in_buffer, &data);
			m_renderer->ReleaseBuffer(frames_in_buffer, AUDCLNT_BUFFERFLAGS_SILENT);

			while (threadData.load())
			{
				WaitForSingleObject(m_need_data_event, 1000);
				if (!threadData.load())
					return;

				m_renderer->GetBuffer(frames_in_buffer, &data);
				m_mixer->Mix(reinterpret_cast<s16 *>(data), frames_in_buffer);

				// Ideally we should not make a smaller signal by applying the volume here, in exclusive mode.
				// We should be sending to the device the volume we want, provided it supports volume adjustment, and send a full range signal.
				// The audio level set in windows does that, but any volume adjustment done by apps e.g. here or by chrome, will lower the signal.
				// This matters for audio quality, for example if the device has a programmable gain amplifier and it's going to take our
				// signal, pass it to an ADC and then amplify it. It's better noise-wise to low amplify a full range signal than 
				// high amplify a low range signal, and any noise that exists at this stage.
				// Since we're in exclusive mode we know we're the only stream so we could adjust the volume based on the volume set 
				// within dolphin, on top of the windows one, and undo on exit, which would be ideal.

				float volume = SConfig::GetInstance().m_IsMuted ? 0 : SConfig::GetInstance().m_Volume / 100.0f;

				for (u32 i = 0; i < frames_in_buffer * 2; i++)
					reinterpret_cast<s16 *>(data)[i] = static_cast<s16>(reinterpret_cast<s16 *>(data)[i] * volume);

				m_renderer->ReleaseBuffer(frames_in_buffer,
				                          Core::GetState() != Core::CORE_RUN ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
			}
		}
		else
		{
			HRESULT hr;

			/* // Priming half the buffer with silent data
			UINT32 initial_padding = 0;
			hr = m_audio_client->GetCurrentPadding(&initial_padding);
			if (SUCCEEDED(hr))
			{
				UINT32 initial_frames_available = frames_in_buffer - initial_padding;
				if (initial_frames_available > 0)
				{
					// Let's only prime about half the buffer or less initially to get started quickly
					UINT32 frames_to_render = initial_frames_available / 2; // Or some other reasonable starting amount
					if (frames_to_render == 0 && initial_frames_available > 0)
					{
						frames_to_render = initial_frames_available; // Fill what's available if half is zero
					}

					if (frames_to_render > 0)
					{
						BYTE *pData = nullptr;
						hr = m_renderer->GetBuffer(frames_to_render, &pData);
						if (SUCCEEDED(hr) && pData != nullptr)
						{
							m_mixer->Mix(reinterpret_cast<s16 *>(pData), frames_to_render);
							// Apply volume (consider refactoring volume application into the mixer or a helper)
							float volume =
							    SConfig::GetInstance().m_IsMuted ? 0 : SConfig::GetInstance().m_Volume / 100.0f;
							for (UINT32 i = 0; i < frames_to_render * 2; ++i)
							{
								reinterpret_cast<s16 *>(pData)[i] =
								    static_cast<s16>(reinterpret_cast<s16 *>(pData)[i] * volume);
							}
							m_renderer->ReleaseBuffer(frames_to_render, 0);
							INFO_LOG(AUDIO, "Primed buffer with %u frames.", frames_to_render);
						}
						else
						{
							ERROR_LOG(AUDIO, "Initial GetBuffer failed: HRESULT %s, pData=%p",
							          wasapi_hresult_to_string(hr).c_str(), pData);
						}
					}
				}
			}
			else
			{
				ERROR_LOG(AUDIO, "Initial GetCurrentPadding failed: HRESULT %s", wasapi_hresult_to_string(hr).c_str());
			}*/

			// Overall loop: wait for event, check padding, get buffer, mix, release buffer
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

					Sleep(1);
					continue;
				}

				// Check the pointer just in case, though a successful HRESULT should guarantee non-nullptr
				if (pData == nullptr)
				{
					// This *shouldn't* happen if hr is S_OK. Log it aggressively if it does.
					ERROR_LOG(AUDIO, "GetBuffer succeeded (HRESULT S_OK) but returned nullptr data pointer!");
					// Release the (zero-sized?) buffer anyway to maintain state? The API is unclear here.
					// Let's assume we shouldn't release if pData is null, maybe the state machine is broken.
					// Maybe break the loop?
					break; // Exit loop if state seems corrupted
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
						break; // Exit loop on invalidated device
					}
					// Other errors => break anyway
					break;
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

void WASAPIStream::Stop()
{
	SoundStream::Stop();

	if(m_need_data_event)
		CloseHandle(m_need_data_event);
	if(m_audio_client)
		m_audio_client->Stop();
	if(m_renderer)
		m_renderer->Release();
	if(m_audio_client)
		m_audio_client->Release();

	m_need_data_event = nullptr;
	m_renderer = nullptr;
	m_audio_client = nullptr;
}

std::vector<std::string> GetAudioDevices(__MIDL___MIDL_itf_mmdeviceapi_0000_0000_0001 audioDeviceMode)
{
	HRESULT hr = S_OK;
	IMMDeviceEnumerator* mm_device_enumerator;

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

		mm_device_enumerator->Release();
		return {};
	}

	UINT device_count = 0;
	devices->GetCount(&device_count);

	std::vector<std::string> results;
	for(UINT i = 0; i < device_count; i++)
	{
		IMMDevice* device;
		devices->Item(i, &device);

		IPropertyStore* pstore;
		device->OpenPropertyStore(STGM_READ, &pstore);

		PROPVARIANT name_prop;
		PropVariantInit(&name_prop);

		pstore->GetValue(PKEY_Device_FriendlyName, &name_prop);

		char name_cstr[2048];
		size_t ret;

		ret = wcstombs(name_cstr, name_prop.pwszVal, sizeof(name_cstr));
		if(ret == 2048) name_cstr[2047] = '\0';

		std::string name_stdstr = name_cstr;

		for(int i = 0; i <= 9; i++)
		{
			if(name_stdstr.substr(0, std::string("0 - ").size()) == std::to_string(i) + " - ")
				name_stdstr = name_stdstr.substr(std::string("0 - ").size()) + " [" + std::to_string(i) + "]";
		}

		// if(name_stdstr.size() > 40)
		//     name_stdstr = name_stdstr.substr(0, 40) + "...";
		// needs to be preserved for uniqueness

		results.push_back(name_stdstr);
		PropVariantClear(&name_prop);

		pstore->Release();
		device->Release();
	}

	devices->Release();
	mm_device_enumerator->Release();

	return results;
}

std::vector<std::string> WASAPIStream::GetRenderDeviceNames()
{
	return GetAudioDevices(eRender);
}

std::vector<std::string> WASAPIStream::GetCaptureDeviceNames()
{
	return GetAudioDevices(eCapture);
}
