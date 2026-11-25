# Linux-only private HTTP/JSON dependencies. Source is pinned by upstream commit
# and exact file hashes; no configure-time network or system package mutation.
foreach(package IN ITEMS "cpp-httplib/0.65.0" "nlohmann-json/3.12.0")
    set(root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${package}")
    file(READ "${root}/SOURCE.json" manifest)
    string(JSON count LENGTH "${manifest}" files)
    math(EXPR last "${count}-1")
    foreach(index RANGE 0 ${last})
        string(JSON path GET "${manifest}" files ${index} path)
        string(JSON expected GET "${manifest}" files ${index} sha256)
        file(SHA256 "${root}/${path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Pinned Linux web input changed: ${package}/${path}")
        endif()
    endforeach()
endforeach()
add_library(sfx_linux_web_dependencies INTERFACE)
target_include_directories(sfx_linux_web_dependencies SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib/0.65.0/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json/3.12.0/include")
target_compile_definitions(sfx_linux_web_dependencies INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(sfx_linux_web_dependencies INTERFACE OpenSSL::SSL Threads::Threads)
