#pragma once

// Identity of the virtual camera source COM object, shared by the DLL and the app.
namespace awc {

// {8F3C7A21-5D64-4E9B-A17C-2B0E9D4F6A83}
constexpr wchar_t kSourceClsidString[] = L"{8F3C7A21-5D64-4E9B-A17C-2B0E9D4F6A83}";
constexpr wchar_t kCameraFriendlyName[] = L"AndroidWebCam";
constexpr wchar_t kSourceDescription[] = L"AndroidWebCam Virtual Camera Source";

} // namespace awc
