#pragma once

#ifdef _WIN32

#include <mmdeviceapi.h>
#include <windows.h>

interface IPolicyConfig : public IUnknown
{
  public:
	virtual HRESULT GetMixFormat(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [out] */ WAVEFORMATEX * *ppFormat) = 0;

	virtual HRESULT GetDeviceFormat(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ BOOL bDefault,
	    /* [out] */ WAVEFORMATEX * *ppFormat) = 0;

	virtual HRESULT ResetDeviceFormat(
	    /* [in] */ PCWSTR pszDeviceName) = 0;

	virtual HRESULT SetDeviceFormat(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ WAVEFORMATEX * pEndpointFormat,
	    /* [in] */ WAVEFORMATEX * pMixFormat) = 0;

	virtual HRESULT GetProcessingPeriod(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ BOOL bDefault,
	    /* [out] */ PINT64 pmftDefaultPeriod,
	    /* [out] */ PINT64 pmftMinimumPeriod) = 0;

	virtual HRESULT SetProcessingPeriod(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ PINT64 pmftPeriod) = 0;

	virtual HRESULT GetShareMode(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [out] */ struct DeviceShareMode * pMode) = 0;

	virtual HRESULT SetShareMode(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ struct DeviceShareMode * pMode) = 0;

	virtual HRESULT GetPropertyValue(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ const PROPERTYKEY &key,
	    /* [out] */ PROPVARIANT *pv) = 0;

	virtual HRESULT SetPropertyValue(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ const PROPERTYKEY &key,
	    /* [in] */ PROPVARIANT *pv) = 0;

	virtual HRESULT SetDefaultEndpoint(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ ERole role) = 0; // This is the key method we need

	virtual HRESULT SetEndpointVisibility(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ BOOL bVisible) = 0;

	// These methods were added in later versions (e.g., Windows 8/10+)
	// If targeting only Windows 7, these might not be callable or the interface
	// version providing them might not be available, but the object typically
	// still provides the older methods via a compatible interface.
	// The IID {0xF8679F50-...} is for this newer version of the interface.

	virtual HRESULT GetLastSessionVolume(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ ULONG unknown1, // Appears to be session ID or similar
	    /* [out] */ float *volume) = 0;

	virtual HRESULT SetLastSessionVolume(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ ULONG unknown1,
	    /* [in] */ float volume,
	    /* [in] */ LPCGUID EventContext) = 0;

	virtual HRESULT GetMuteOrigin(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [out] */ GUID * guidOrigin) = 0;

	virtual HRESULT SetDefaultEndpointExtension(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ ERole role,
	    /* [in] */ LUID MergeLuid) = 0;

	virtual HRESULT GetDefaultEndpointExtension(
	    /* [in] */ PCWSTR pszDeviceName,
	    /* [in] */ ERole role,
	    /* [out] */ LUID * pMergeLuid) = 0;
};

interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig;
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

#endif