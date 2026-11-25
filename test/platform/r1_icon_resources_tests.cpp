// Resource-only checks: no app entry point, windows, input injection, install
// access, or UAC. Read the exact shipping EXEs as data-only resource modules.
#include "sfx/platform/win32/window_icon.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <shlwapi.h>
#include <wincodec.h>

namespace {
unsigned checks{};
void require(bool value, const char* text) {
    ++checks;
    if (!value) throw std::runtime_error(text);
}
void dimensions(HICON icon, int size) {
    ICONINFO info{};
    require(icon && GetIconInfo(icon, &info), "native icon load/info");
    BITMAP bitmap{};
    const auto bytes = GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    require(bytes == sizeof(bitmap) && bitmap.bmWidth == size && bitmap.bmHeight == size &&
        bitmap.bmBitsPixel == 32, "native size/color selection");
}
void sizes(HINSTANCE instance) {
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        sfx::platform::win32::WindowIcon icon(instance, 1, size, size);
        dimensions(icon.get(), size);
    }
    // Ordinary 96-DPI and 144-DPI title/task-switcher resource pairs; this is
    // native resource selection, not a claim of a high-DPI desktop UI run.
    for (const auto pair : {std::array<int, 2>{16, 32}, std::array<int, 2>{24, 48}}) {
        sfx::platform::win32::WindowIcon large_icon(instance, 1, pair[1], pair[1]);
        sfx::platform::win32::WindowIcon small_icon(instance, 1, pair[0], pair[0]);
        require(large_icon.get() != small_icon.get(), "independent size ownership");
        dimensions(large_icon.get(), pair[1]); dimensions(small_icon.get(), pair[0]);
    }
}
void about(HINSTANCE instance) {
    const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(203), RT_RCDATA);
    require(resource != nullptr, "About resource present");
    const auto size = SizeofResource(instance, resource);
    const auto loaded = LoadResource(instance, resource);
    const auto* bytes = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    require(bytes != nullptr && size > 0, "About resource readable");
    auto* stream = SHCreateMemStream(bytes, size);
    require(stream != nullptr, "About stream");
    IWICImagingFactory* factory{}; IWICBitmapDecoder* decoder{}; IWICBitmapFrameDecode* frame{};
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromStream(stream, nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    UINT width{}, height{};
    if (SUCCEEDED(result)) result = frame->GetSize(&width, &height);
    if (frame) frame->Release(); if (decoder) decoder->Release();
    if (factory) factory->Release(); stream->Release();
    require(SUCCEEDED(result) && width == 1254 && height == 1254,
        "owner About artwork decodes at exact dimensions");
}
}
int wmain(int count, wchar_t** args) {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialized");
        require(count == 2, "shipping resource directory argument");
        const auto directory = std::filesystem::canonical(args[1]);
        for (const auto* name : {L"SFXControlPanel.exe", L"SFX-BBS-Setup.exe"}) {
            const auto module = LoadLibraryExW((directory / name).c_str(), nullptr,
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            require(module != nullptr, "data-only resource module");
            try {
                sizes(module); // Warm the resource loader before lifetime baseline.
                if (std::wstring_view(name) == L"SFXControlPanel.exe") about(module);
                const auto before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                const auto user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
                for (unsigned repeat = 0; repeat < 20; ++repeat) sizes(module);
                require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before,
                    "no GDI object growth");
                require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == user_before,
                    "no icon/user object growth");
            } catch (...) { FreeLibrary(module); throw; }
            require(FreeLibrary(module), "resource module released");
        }
        CoUninitialize();
        std::cout << "PASS: " << checks << " checks; 0 failed; 0 skipped; 14 native entries; "
            "exact 1254x1254 About decode; 96/144-DPI size pairs; no GDI/USER growth; no app launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
