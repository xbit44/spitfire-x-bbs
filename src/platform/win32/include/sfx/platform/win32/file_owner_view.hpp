#pragma once
#include "sfx/application/file_management.hpp"
#include <filesystem>
#include <functional>
#include <memory>
namespace sfx::platform::win32 {
// A single immutable native source owned by the authenticated owner worker.
// Never executes content, invokes a shell, extracts entries, or modifies files.
class WindowsFileOwnerView final {
public:
    using Progress=std::function<bool(std::uint64_t,std::uint64_t)>;
    WindowsFileOwnerView(application::IFileStorage&,std::filesystem::path,
        std::wstring,application::FileId,bool archive,const Progress& = {},application::FileConfigurationActor actor={true,{}});
    ~WindowsFileOwnerView();
    WindowsFileOwnerView(const WindowsFileOwnerView&)=delete;
    application::FileManagementViewPage page(std::uint16_t);
    application::FileManagementViewPage member(std::uint16_t,const Progress& = {});
    application::FileManagementViewPage archive_index(std::uint16_t);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::win32
