#include <windows.h>
#include <objbase.h>

#include <initguid.h>   // makes the DEFINE_GUID below emit a definition

// {8F3C7A21-5D64-4E9B-A17C-2B0E9D4F6A83} - keep in sync with CameraIds.h
DEFINE_GUID(CLSID_AwcVirtualCameraSource,
            0x8f3c7a21, 0x5d64, 0x4e9b, 0xa1, 0x7c, 0x2b, 0x0e, 0x9d, 0x4f, 0x6a, 0x83);
