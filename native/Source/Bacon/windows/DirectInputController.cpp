#include "Controller.h"
#include "Platform.h"
#include "../BaconInternal.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <wbemidl.h>
#include <oleauto.h>
//#include <wbemidl.h> 
#include <cstdio>
using namespace std;

#define SAFE_RELEASE(x) if(x) { x->Release(); x = NULL; } 

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

IDirectInput8* s_DirectInput = nullptr;

struct DirectInputController
{
    GUID m_Guid;
    IDirectInputDevice8* m_Device;
    int m_SupportedAxesMask;
    int m_SupportedButtonsMask;
    int m_MaxButtons;
    DIJOYSTATE m_State;
};
static DirectInputController s_Controllers[MaxControllerCount];

static struct AxisMapEntry
{
    const GUID& m_GUID;
    Bacon_Controller_Axes m_Axis;
} s_PreferredAxisMap[] = {
    { GUID_XAxis, Bacon_Controller_Axis_LeftThumbX },
    { GUID_YAxis, Bacon_Controller_Axis_LeftThumbY },
    { GUID_ZAxis, Bacon_Controller_Axis_RightThumbX },
    { GUID_RxAxis, Bacon_Controller_Axis_Axis1 },
    { GUID_RyAxis, Bacon_Controller_Axis_Axis1 },
    { GUID_RzAxis, Bacon_Controller_Axis_RightThumbY },
};

static int GetPreferredAxisIndex(GUID const& guid)
{
    for (int i = 0; i < BACON_ARRAY_COUNT(s_PreferredAxisMap); ++i)
    {
        if (IsEqualGUID(guid, s_PreferredAxisMap[i].m_GUID))
            return s_PreferredAxisMap[i].m_Axis;
    }
    return 0;
}

static bool CheckError(HRESULT hr)
{
    if (hr != S_OK)
    {
        // TODO log error
        return false;
    }
    return true;
}

static void ReleaseController(int controllerIndex)
{
    DirectInputController& controller = s_Controllers[controllerIndex];
    if (controller.m_Device)
    {
        controller.m_Device->Release();
        ZeroMemory(&s_Controllers[controllerIndex], sizeof(DirectInputController));
        Controller_SetProvider(controllerIndex, ControllerProvider_None);
    }
}

static BOOL CALLBACK AddControllerObject(const DIDEVICEOBJECTINSTANCE* instance, void* context)
{
    DirectInputController& controller = *(DirectInputController*)context;

    if (int axis = GetPreferredAxisIndex(instance->guidType))
    {
        DIPROPRANGE propertyRange;
        propertyRange.diph.dwSize = sizeof(DIPROPRANGE);
        propertyRange.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        propertyRange.diph.dwHow = DIPH_BYID;
        propertyRange.diph.dwObj = instance->dwType;
        propertyRange.lMin = -65535;
        propertyRange.lMax = +65535;

        if (CheckError(controller.m_Device->SetProperty(DIPROP_RANGE, &propertyRange.diph)))
            controller.m_SupportedAxesMask |= axis;
    }
    else if (instance->wUsagePage == 0x9) // Button
        controller.m_SupportedButtonsMask |= (Bacon_Controller_Button_Button1 << instance->wUsage);

    return DIENUM_CONTINUE;
}

// http://msdn.microsoft.com/en-us/library/windows/desktop/ee417014(v=vs.85).aspx
//-----------------------------------------------------------------------------
// Enum each PNP device using WMI and check each device ID to see if it contains 
// "IG_" (ex. "VID_045E&PID_028E&IG_00").  If it does, then it's an XInput device
// Unfortunately this information can not be found by just using DirectInput 
//-----------------------------------------------------------------------------
static BOOL IsXInputDevice( const GUID* pGuidProductFromDirectInput )
{
    IWbemLocator*           pIWbemLocator  = NULL;
    IEnumWbemClassObject*   pEnumDevices   = NULL;
    IWbemClassObject*       pDevices[20]   = {0};
    IWbemServices*          pIWbemServices = NULL;
    BSTR                    bstrNamespace  = NULL;
    BSTR                    bstrDeviceID   = NULL;
    BSTR                    bstrClassName  = NULL;
    DWORD                   uReturned      = 0;
    bool                    bIsXinputDevice= false;
    UINT                    iDevice        = 0;
    VARIANT                 var;
    HRESULT                 hr;

    // CoInit if needed
    hr = CoInitialize(NULL);
    bool bCleanupCOM = SUCCEEDED(hr);

    // Create WMI
    hr = CoCreateInstance( __uuidof(WbemLocator),
        NULL,
        CLSCTX_INPROC_SERVER,
        __uuidof(IWbemLocator),
        (LPVOID*) &pIWbemLocator);
    if( FAILED(hr) || pIWbemLocator == NULL )
        goto LCleanup;

    bstrNamespace = SysAllocString( L"\\\\.\\root\\cimv2" );if( bstrNamespace == NULL ) goto LCleanup;        
    bstrClassName = SysAllocString( L"Win32_PNPEntity" );   if( bstrClassName == NULL ) goto LCleanup;        
    bstrDeviceID  = SysAllocString( L"DeviceID" );          if( bstrDeviceID == NULL )  goto LCleanup;        

    // Connect to WMI 
    hr = pIWbemLocator->ConnectServer( bstrNamespace, NULL, NULL, 0L, 
        0L, NULL, NULL, &pIWbemServices );
    if( FAILED(hr) || pIWbemServices == NULL )
        goto LCleanup;

    // Switch security level to IMPERSONATE. 
    CoSetProxyBlanket( pIWbemServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, 
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE );                    

    hr = pIWbemServices->CreateInstanceEnum( bstrClassName, 0, NULL, &pEnumDevices ); 
    if( FAILED(hr) || pEnumDevices == NULL )
        goto LCleanup;

    // Loop over all devices
    for( ;; )
    {
        // Get 20 at a time
        hr = pEnumDevices->Next( 10000, 20, pDevices, &uReturned );
        if( FAILED(hr) )
            goto LCleanup;
        if( uReturned == 0 )
            break;

        for( iDevice=0; iDevice<uReturned; iDevice++ )
        {
            // For each device, get its device ID
            hr = pDevices[iDevice]->Get( bstrDeviceID, 0L, &var, NULL, NULL );
            if( SUCCEEDED( hr ) && var.vt == VT_BSTR && var.bstrVal != NULL )
            {
                // Check if the device ID contains "IG_".  If it does, then it's an XInput device
                // This information can not be found from DirectInput 
                if( wcsstr( var.bstrVal, L"IG_" ) )
                {
                    // If it does, then get the VID/PID from var.bstrVal
                    DWORD dwPid = 0, dwVid = 0;
                    WCHAR* strVid = wcsstr( var.bstrVal, L"VID_" );
                    if( strVid && swscanf_s( strVid, L"VID_%4X", &dwVid ) != 1 )
                        dwVid = 0;
                    WCHAR* strPid = wcsstr( var.bstrVal, L"PID_" );
                    if( strPid && swscanf_s( strPid, L"PID_%4X", &dwPid ) != 1 )
                        dwPid = 0;

                    // Compare the VID/PID to the DInput device
                    DWORD dwVidPid = MAKELONG( dwVid, dwPid );
                    if( dwVidPid == pGuidProductFromDirectInput->Data1 )
                    {
                        bIsXinputDevice = true;
                        goto LCleanup;
                    }
                }
            }   
            SAFE_RELEASE( pDevices[iDevice] );
        }
    }

LCleanup:
    if(bstrNamespace)
        SysFreeString(bstrNamespace);
    if(bstrDeviceID)
        SysFreeString(bstrDeviceID);
    if(bstrClassName)
        SysFreeString(bstrClassName);
    for( iDevice=0; iDevice<20; iDevice++ )
        SAFE_RELEASE( pDevices[iDevice] );
    SAFE_RELEASE( pEnumDevices );
    SAFE_RELEASE( pIWbemLocator );
    SAFE_RELEASE( pIWbemServices );

    if( bCleanupCOM )
        CoUninitialize();

    return bIsXinputDevice;
}

static BOOL CALLBACK OnControllerConnected(const DIDEVICEINSTANCE* instance, void* context)
{
    // Ignore devices that can be handled by XInput
    if (IsXInputDevice(&instance->guidProduct))
        return DIENUM_CONTINUE;

    // Already connected?
    for (int i = 0; i < MaxControllerCount; ++i)
    {
        if (IsEqualGUID(instance->guidInstance, s_Controllers[i].m_Guid))
            return DIENUM_CONTINUE;
    }

    // Get a controller index for new connection; abandon if exceeds max number of connections
    int controllerIndex = Controller_GetAvailableIndex(0);
    if (controllerIndex == -1)
        return DIENUM_STOP;

    // Open device
    IDirectInputDevice8* device = nullptr;
    if (!CheckError(s_DirectInput->CreateDevice(instance->guidInstance, &device, NULL)))
        goto Error;

    ZeroMemory(&s_Controllers[controllerIndex], sizeof(DirectInputController));
    s_Controllers[controllerIndex].m_Guid = instance->guidInstance;
    s_Controllers[controllerIndex].m_Device = device;
    s_Controllers[controllerIndex].m_State.rgdwPOV[0] = -1;

    DIDEVCAPS caps;
    caps.dwSize = sizeof(caps);
    if (!CheckError(device->GetCapabilities(&caps)))
        goto Error;

    s_Controllers[controllerIndex].m_MaxButtons = caps.dwButtons;

    if (!CheckError(device->SetDataFormat(&c_dfDIJoystick)))
        goto Error;

    if (!CheckError(device->SetCooperativeLevel(g_hWnd, DISCL_EXCLUSIVE | DISCL_FOREGROUND)))
        goto Error;
    
    if (!CheckError(device->EnumObjects(AddControllerObject, &s_Controllers[controllerIndex], DIDFT_ABSAXIS | DIDFT_PSHBUTTON | DIDFT_POV)))
        goto Error;

    // This actually marks controller index as in-use by DirectInput
    Controller_SetProvider(controllerIndex, ControllerProvider_DirectInput);
    return DIENUM_CONTINUE;

Error:
    ReleaseController(controllerIndex);
    return DIENUM_CONTINUE;
}

static void OnControllerDisconnected(int controller)
{
    ReleaseController(controller);
}

void DirectInputController_Init()
{
    for (int i = 0; i < MaxControllerCount; ++i)
        s_Controllers[i].m_Device = NULL;

    if (!CheckError(DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&s_DirectInput, NULL)))
        return;
}

void DirectInputController_EnumDevices()
{
    CheckError(s_DirectInput->EnumDevices(DI8DEVCLASS_GAMECTRL, OnControllerConnected, nullptr, DIEDFL_ATTACHEDONLY));
}

void DirectInputController_Shutdown()
{
    for (int i = 0; i < MaxControllerCount; ++i)
        ReleaseController(i);
    s_DirectInput->Release();
    s_DirectInput = nullptr;
}

static void SendAxisEvent(int controller, LONG DIJOYSTATE::*member, int axis, DIJOYSTATE const& state)
{
    if (s_Controllers[controller].m_State.*member != state.*member)
    {
        s_Controllers[controller].m_State.*member = state.*member;
        if (g_ControllerAxisHandler)
            g_ControllerAxisHandler(controller, axis, (float)(state.*member) / 65535.f);
    }
}

static void SendButtonEvent(int controller, int button, DIJOYSTATE const& state)
{
    if (s_Controllers[controller].m_State.rgbButtons[button] != state.rgbButtons[button])
    {
        s_Controllers[controller].m_State.rgbButtons[button] = state.rgbButtons[button];
        if (g_ControllerButtonHandler)
            g_ControllerButtonHandler(controller, Bacon_Controller_Button_Button1 << button, state.rgbButtons[button]);
    }
}

static int GetDpadButtonMaskForPov(int pov)
{
    if ((pov & 0xffff) == 0xffff)
        return 0;

    // POV is measured in hundredths of a degree clockwise from north
    int north = 0;
    int south = 180 * DI_DEGREES;
    int east = 90 * DI_DEGREES;
    int west = 270 * DI_DEGREES;

    int buttons = 0;
    if (pov > west || pov < east)
        buttons |= Bacon_Controller_Button_DpadUp;
    if (pov > north && pov < south)
        buttons |= Bacon_Controller_Button_DpadRight;
    if (pov > east && pov < west)
        buttons |= Bacon_Controller_Button_DpadDown;
    if (pov > south)
        buttons |= Bacon_Controller_Button_DpadLeft;

    return buttons;
}

static void SendDpadEvent(int controller, int button, int changedMask, int stateMask)
{
    if ((changedMask & button) && g_ControllerButtonHandler)
        g_ControllerButtonHandler(controller, button, (stateMask & button) != 0);
}

static void SendDpadEvent(int controller, DIJOYSTATE const& state)
{
    if (s_Controllers[controller].m_State.rgdwPOV[0] != state.rgdwPOV[0])
    {
        int oldButtons = GetDpadButtonMaskForPov(s_Controllers[controller].m_State.rgdwPOV[0]);
        s_Controllers[controller].m_State.rgdwPOV[0] = state.rgdwPOV[0];
        int buttons = GetDpadButtonMaskForPov(state.rgdwPOV[0]);
        int changedButtons = oldButtons ^ buttons;
        SendDpadEvent(controller, Bacon_Controller_Button_DpadUp, changedButtons, buttons);
        SendDpadEvent(controller, Bacon_Controller_Button_DpadDown, changedButtons, buttons);
        SendDpadEvent(controller, Bacon_Controller_Button_DpadLeft, changedButtons, buttons);
        SendDpadEvent(controller, Bacon_Controller_Button_DpadRight, changedButtons, buttons);
    }
}

void DirectInputController_Update()
{
    for (int controller = 0; controller < MaxControllerCount; ++controller)
    {
        if (!s_Controllers[controller].m_Device)
            continue;

        DIJOYSTATE state;

        if (s_Controllers[controller].m_Device->Poll() != S_OK)
        {
            HRESULT hr = s_Controllers[controller].m_Device->Acquire();
            if (hr == DIERR_INPUTLOST || hr == DIERR_OTHERAPPHASPRIO)
                continue;
            else if (hr != S_FALSE)
            {
                if (s_DirectInput->GetDeviceStatus(s_Controllers[controller].m_Guid) != S_OK)
                {
                    OnControllerDisconnected(controller);
                    continue;
                }
            }
        }
        s_Controllers[controller].m_Device->GetDeviceState(sizeof(state), &state);

        SendAxisEvent(controller, &DIJOYSTATE::lX, Bacon_Controller_Axis_LeftThumbX, state);
        SendAxisEvent(controller, &DIJOYSTATE::lY, Bacon_Controller_Axis_LeftThumbY, state);
        SendAxisEvent(controller, &DIJOYSTATE::lZ, Bacon_Controller_Axis_RightThumbX, state);
        SendAxisEvent(controller, &DIJOYSTATE::lRz, Bacon_Controller_Axis_RightThumbY, state);
        for (int button = 0; button < s_Controllers[controller].m_MaxButtons; ++button)
            SendButtonEvent(controller, button, state);
        SendDpadEvent(controller, state);
    }
}

static int GetDeviceStringProperty(IDirectInputDevice8* device, const GUID& propertyGuid, char* outBuffer, int* inOutBufferSize)
{
    DIPROPSTRING property;
    ZeroMemory(&property, sizeof(property));
    property.diph.dwSize = sizeof(property);
    property.diph.dwHeaderSize = sizeof(property.diph);
    property.diph.dwHow = DIPH_DEVICE;
    property.diph.dwObj = 0;

    if (device->GetProperty(propertyGuid, &property.diph) != S_OK)
        return Bacon_Error_Unknown;

    *inOutBufferSize = WideCharToMultiByte(CP_UTF8, 0, property.wsz, -1, outBuffer, *inOutBufferSize, NULL, NULL);
    if (!*inOutBufferSize)
        return Bacon_Error_Unknown;

    return Bacon_Error_None;
}

static int GetDeviceDwordProperty(IDirectInputDevice8* device, const GUID& propertyGuid, DWORD* outValue)
{
    DIPROPDWORD property;
    ZeroMemory(&property, sizeof(property));
    property.diph.dwSize = sizeof(property);
    property.diph.dwHeaderSize = sizeof(property.diph);
    property.diph.dwHow = DIPH_DEVICE;
    property.diph.dwObj = 0;

    if (device->GetProperty(propertyGuid, &property.diph) != S_OK)
        return Bacon_Error_Unknown;

    *outValue = property.dwData;
    return Bacon_Error_None;
}

int DirectInputController_GetControllerPropertyInt(int controller, int property, int* outValue)
{
    if (controller < 0 || controller >= MaxControllerCount)
        return Bacon_Error_InvalidHandle;

    if (!s_Controllers[controller].m_Device)
        return Bacon_Error_InvalidHandle;

    switch (property)
    {
        case Bacon_Controller_Property_SupportedAxesMask:
            return s_Controllers[controller].m_SupportedAxesMask;
        case Bacon_Controller_Property_SupportedButtonsMask:
            return s_Controllers[controller].m_SupportedButtonsMask;
        case Bacon_Controller_Property_VendorId:
        case Bacon_Controller_Property_ProductId:
        {
            DWORD vendorProductId;
            if (int error = GetDeviceDwordProperty(s_Controllers[controller].m_Device, DIPROP_VIDPID, &vendorProductId))
                return error;
            if (property == Bacon_Controller_Property_VendorId)
                *outValue = LOWORD(vendorProductId);
            else
                *outValue = HIWORD(vendorProductId);
            return Bacon_Error_None;
        }
        case Bacon_Controller_Property_Profile:
            return Bacon_Controller_Profile_Generic;
    }

    return Bacon_Error_InvalidArgument;
}

int DirectInputController_GetControllerPropertyString(int controller, int property, char* outBuffer, int* inOutBufferSize)
{
    if (controller < 0 || controller >= MaxControllerCount)
        return Bacon_Error_InvalidHandle;

    if (!s_Controllers[controller].m_Device)
        return Bacon_Error_InvalidHandle;

    switch (property)
    {
    case Bacon_Controller_Property_Name:
        return GetDeviceStringProperty(s_Controllers[controller].m_Device, DIPROP_INSTANCENAME, outBuffer, inOutBufferSize);
    }

    return Bacon_Error_InvalidArgument;
}
