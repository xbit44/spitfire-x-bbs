#pragma once
#include "sfx/platform/win32/desktop_theme.hpp"
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>
#include <string_view>
namespace sfx::file_manager {
std::wstring normalized_form_lines(std::wstring_view);
std::wstring native_form_lines(std::wstring_view);
struct PickerResult {std::vector<std::filesystem::path> paths;bool cancelled{};std::wstring error;};
PickerResult pick_import_sources(HWND,const std::filesystem::path& initial,bool folder);
bool show_readonly(HWND,platform::win32::DesktopAppearance&,const std::wstring& title,
    const std::wstring& body,bool confirmation=false);
enum class FormFieldKind { text,multiline,checkbox,choice };
struct FormField {
    std::wstring label,value;
    FormFieldKind kind{FormFieldKind::text};
    bool checked{};
    std::vector<std::pair<std::int64_t,std::wstring>> choices;
    std::int64_t selected{};
};
using FormValidation=std::function<std::optional<std::wstring>(const std::vector<FormField>&)>;
std::optional<std::vector<FormField>> show_form(HWND,platform::win32::DesktopAppearance&,
    std::wstring title,std::wstring explanation,std::vector<FormField>,
    FormValidation = {});
} // namespace sfx::file_manager
