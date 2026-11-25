#pragma once
#include "client_worker.hpp"
#include <windows.h>
namespace sfx::file_manager {
int run_file_manager_ui(HINSTANCE,const std::filesystem::path&,const std::wstring&,
    ClientWorker::Factory = platform::win32::open_file_administration_client);
} // namespace sfx::file_manager
