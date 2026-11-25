#pragma once
#include "sfx/platform/win32/clamav_installation.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include <functional>
#include <stdexcept>

namespace sfx::apps::setup {
inline constexpr std::uint64_t kClamAvDownloadBytes=platform::win32::kClamAvPortableBytes;
class ClamAvDownloadCancelled final:public std::runtime_error {
public:ClamAvDownloadCancelled():std::runtime_error("Portable ClamAV download cancelled. Your installation was not changed."){}
};
// Native test transport seam. The immutable size/hash/archive verification is
// always performed, including when a qualification transport supplies bytes.
class ClamAvDownloadReader {
public:
    virtual ~ClamAvDownloadReader()=default;
    virtual std::optional<std::uint64_t> content_length()const=0;
    virtual std::size_t read(std::uint8_t*,std::size_t)=0;
};
using ClamAvDownloadProgress=std::function<void(const std::wstring&)>;
using ClamAvDownloadPump=std::function<bool()>;
class DownloadedClamAv final {
public:
    ~DownloadedClamAv();
    DownloadedClamAv(const DownloadedClamAv&)=delete;
    const std::filesystem::path& path()const noexcept;
    static std::unique_ptr<DownloadedClamAv> fetch(const ClamAvDownloadProgress&,
        const ClamAvDownloadPump&);
    static std::unique_ptr<DownloadedClamAv> receive(ClamAvDownloadReader&,
        const ClamAvDownloadProgress&,const ClamAvDownloadPump&);
private:
    struct Impl;DownloadedClamAv();std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::setup
