// Kept outside the board so Windows can open removal after the board folder
// has been deleted. Installations for the same account and Setup image share it.
Path uninstall_cache_path(const std::wstring& owner, const std::string& digest) {
    const std::string owner_bytes = ascii(owner);
    const auto owner_tag = sha256(std::vector<std::uint8_t>(owner_bytes.begin(), owner_bytes.end())).substr(0, 12);
    const auto prefix = L"SFX-Uninstall-" + wide(owner_tag) + L"-" + wide(digest.substr(0, 16));
    return canonical_root(known_folder(CSIDL_PROGRAM_FILESX86) / (prefix + L".exe"));
}

std::wstring app_registration_key(const Receipt& receipt) {
    return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Spitfire-X BBS." + receipt.id;
}

struct UninstallRegistryKey {
    HKEY value{};
    ~UninstallRegistryKey() { if (value) RegCloseKey(value); }
};

std::wstring uninstall_registry_string(HKEY key, const wchar_t* name) {
    std::array<wchar_t, 1024> buffer{};
    DWORD bytes = sizeof(buffer), type{};
    require(RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) == ERROR_SUCCESS &&
        type == REG_SZ && bytes >= sizeof(wchar_t) && bytes % sizeof(wchar_t) == 0 &&
        buffer[bytes / sizeof(wchar_t) - 1] == L'\0' &&
        wcslen(buffer.data()) + 1 == bytes / sizeof(wchar_t), "setup_uninstall_registry_value_invalid");
    return buffer.data();
}

void verify_app_registration(HKEY key, const Receipt& receipt) {
    DWORD subkeys{}, type{}, bytes{};
    require(RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && !subkeys &&
        RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, &bytes) == ERROR_FILE_NOT_FOUND &&
        uninstall_registry_string(key, L"SfxInstallationId") == receipt.id &&
        uninstall_registry_string(key, L"SfxOwner") == receipt.owner &&
        !_wcsicmp(uninstall_registry_string(key, L"InstallLocation").c_str(), receipt.root.c_str()),
        "setup_uninstall_registry_identity_mismatch");
}

void unregister_app(const Receipt& receipt) {
    UninstallRegistryKey key;
    const auto name = app_registration_key(receipt);
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, name.c_str(), REG_OPTION_OPEN_LINK,
        KEY_READ | KEY_WOW64_64KEY, &key.value);
    if (status == ERROR_FILE_NOT_FOUND) return;
    require(status == ERROR_SUCCESS, "setup_uninstall_registry_open_failed");
    verify_app_registration(key.value, receipt);
    require(RegDeleteKeyExW(HKEY_LOCAL_MACHINE, name.c_str(), KEY_WOW64_64KEY, 0) == ERROR_SUCCESS,
        "setup_uninstall_registry_remove_failed");
}

Path local_uninstall_link(const Receipt& receipt) {
    return receipt.root / L"Uninstall Spitfire-X BBS.lnk";
}

struct UninstallLink { Path target; std::wstring id; };
std::optional<UninstallLink> read_uninstall_link(const Path& path) {
    auto held = open_object(path, false);
    IShellLinkW* link{};
    require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, reinterpret_cast<void**>(&link))), "setup_uninstall_link_open_failed");
    struct ReleaseLink { IShellLinkW* value; ~ReleaseLink(){value->Release();} } release{link};
    IPersistFile* persistence{};
    require(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistence))),
        "setup_uninstall_link_query_failed");
    const auto loaded = persistence->Load(path.c_str(), STGM_READ);
    persistence->Release();
    std::array<wchar_t, MAX_PATH> program{}, args{};
    if (FAILED(loaded) || FAILED(link->GetPath(program.data(), static_cast<int>(program.size()), nullptr, SLGP_RAWPATH)) ||
        FAILED(link->GetArguments(args.data(), static_cast<int>(args.size())))) return {};
    const std::wstring arguments(args.data());
    constexpr std::wstring_view prefix=L"--uninstall ";
    if (arguments.compare(0,prefix.size(),prefix)!=0) return {};
    auto id=arguments.substr(prefix.size());
    if(!transaction_id(id)) return {};
    return UninstallLink{Path(program.data()),std::move(id)};
}

bool selected_uninstall_link(const Path& path, const Receipt& receipt, Path* executable = nullptr) {
    const auto link=read_uninstall_link(path);
    if(!link||link->id!=receipt.id)return false;
    const auto& target=link->target;
    if (target.filename().native().rfind(L"SFX-Uninstall-", 0) != 0 ||
        _wcsicmp(target.parent_path().c_str(), known_folder(CSIDL_PROGRAM_FILESX86).c_str()) || absent(target)) return false;
    const auto protected_file = open_object(target, false);
    const auto security = structural_descriptor(receipt.owner);
    if (!exact_security(protected_file.get(), security.get()) ||
        _wcsicmp(uninstall_cache_path(receipt.owner, file_sha256(target)).c_str(), target.c_str())) return false;
    if (executable) *executable = target;
    return true;
}

void publish_uninstall_entry(const Receipt& receipt, HANDLE ordinary_token) {
    const auto media = read_file(module_path(), maximum_program_bytes);
    const auto digest = sha256(media);
    const auto cache = uninstall_cache_path(receipt.owner, digest);
    const auto anchors = protect_ancestors(cache, ordinary_token);
    const auto cache_parent = protect_installer_cache_parent(cache, ordinary_token);
    const auto security = structural_descriptor(receipt.owner);
    if (absent(cache)) create_file(cache, media, security.get());
    auto cache_file = open_object(cache, false);
    require(exact_security(cache_file.get(), security.get()) && file_sha256(cache) == digest,
        "setup_uninstall_cache_collision_preserved");

    const auto args = L"--uninstall " + receipt.id;
    const auto command = L"\"" + cache.native() + L"\" " + args;
    const auto app_key = app_registration_key(receipt);
    UninstallRegistryKey key;
    DWORD disposition{};
    require(RegCreateKeyExW(HKEY_LOCAL_MACHINE, app_key.c_str(), 0, nullptr, 0,
        KEY_READ | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key.value, &disposition) == ERROR_SUCCESS,
        "setup_uninstall_registry_create_failed");
    if (disposition == REG_OPENED_EXISTING_KEY) verify_app_registration(key.value, receipt);
    const auto set = [&](const wchar_t* name, const std::wstring& value) {
        const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        require(RegSetValueExW(key.value, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS, "setup_uninstall_registry_write_failed");
    };
    set(L"SfxInstallationId", receipt.id);
    set(L"SfxOwner", receipt.owner);
    set(L"InstallLocation", receipt.root.native());
    set(L"DisplayName", L"Spitfire-X BBS (" + receipt.root.native() + L")");
    set(L"DisplayVersion", SFX_PRODUCT_VERSION_SHORT_WSTRING);
    set(L"Publisher", L"Spitfire-X");
    set(L"UninstallString", command);
    const DWORD one = 1;
    for (const auto* name : {L"NoModify", L"NoRepair"})
        require(RegSetValueExW(key.value, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS,
            "setup_uninstall_registry_write_failed");
    verify_app_registration(key.value, receipt);
    require(uninstall_registry_string(key.value, L"UninstallString") == command &&
        RegFlushKey(key.value) == ERROR_SUCCESS, "setup_uninstall_registry_readback_failed");

    const auto link = local_uninstall_link(receipt);
    const bool replacing = !absent(link);
    if (replacing) {
        Path earlier_cache;
        if (selected_uninstall_link(link, receipt, &earlier_cache)) {
            if (!_wcsicmp(earlier_cache.c_str(), cache.c_str())) return;
        } else {
            // A copied board may carry a launcher for its earlier receipt.
            // Verify only that protected record before replacing the link.
            const auto earlier=read_uninstall_link(link);
            require(earlier && earlier->id!=receipt.id,"setup_uninstall_link_collision_preserved");
            const auto records=registered_installations({earlier->id},true);
            require(records.size()==1 && records.front().receipt.committed &&
                selected_uninstall_link(link,records.front().receipt),"setup_uninstall_link_collision_preserved");
        }
    }
    const auto staged = receipt.root / (L"SFX-uninstall-stage-" + unique_id() + L".lnk");
    create_shortcut(staged, cache, args);
    require(selected_uninstall_link(staged, receipt), "setup_uninstall_link_staging_verification_failed");
    require(MoveFileExW(staged.c_str(), link.c_str(), MOVEFILE_WRITE_THROUGH |
        (replacing ? MOVEFILE_REPLACE_EXISTING : 0)), "setup_uninstall_link_publication_failed");
    require(selected_uninstall_link(link, receipt), "setup_uninstall_link_readback_failed");
}

void require_missing_installation(const Receipt& receipt) {
    const auto parent_path = receipt.root.parent_path();
    auto parent = open_object(parent_path, true);
    require(volume_serial(parent.get()) == receipt.volume && absent(receipt.root),
        "setup_uninstall_missing_original_volume_or_folder_changed");
}

void remove_board_tree(const Path& path, const std::function<void()>& tick, bool erase, unsigned depth = 0) {
    require(depth < 64 && path.native().size() < MAX_PATH - 3, "setup_full_removal_path_limit");
    auto directory = open_object(path, true, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE);
    std::vector<std::pair<Path, DWORD>> children;
    WIN32_FIND_DATAW item{};
    const auto raw = FindFirstFileW((path / L"*").c_str(), &item);
    if (raw != INVALID_HANDLE_VALUE) {
        struct Search { HANDLE value; ~Search(){FindClose(value);} } search{raw};
        do {
            if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L"..")) continue;
            children.emplace_back(path / item.cFileName, item.dwFileAttributes);
        } while (FindNextFileW(raw, &item));
        require(GetLastError() == ERROR_NO_MORE_FILES, "setup_full_removal_inventory_failed");
    } else require(GetLastError() == ERROR_FILE_NOT_FOUND, "setup_full_removal_inventory_failed");
    for (const auto& [child, attributes] : children) {
        tick();
        require(child.native().size() < MAX_PATH && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "setup_full_removal_redirected_or_long_entry_preserved");
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_board_tree(child, tick, erase, depth + 1);
            continue;
        }
        require((depth == 1 && !_wcsicmp(path.filename().c_str(), L"Bin")) ||
            _wcsicmp(child.filename().c_str(), L"installation.id") != 0, "setup_full_removal_nested_installation_preserved");
        require(!(attributes & FILE_ATTRIBUTE_READONLY), "setup_full_removal_read_only_entry_preserved");
        auto file = open_object(child, false, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ);
        if (erase) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            require(SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)),
                "setup_full_removal_file_failed");
        }
    }
    if (erase) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        require(SetFileInformationByHandle(directory.get(), FileDispositionInfo, &disposition, sizeof(disposition)),
            "setup_full_removal_directory_failed");
    }
}
