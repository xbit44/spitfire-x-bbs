# Shared hash-checked SSH inputs. Build outputs remain platform-specific.
set(SFX_SSH_THIRD_PARTY "${CMAKE_CURRENT_BINARY_DIR}/third-party/ssh")
set(SFX_OPENSSL_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl/3.5.9/artifacts/openssl-3.5.9.tar.gz")
set(SFX_DEUCESSH_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deucessh/ed49634/artifacts/DeuceSSH-ed49634.zip")
file(SHA256 "${SFX_OPENSSL_ARCHIVE}" SFX_OPENSSL_SHA256)
file(SHA256 "${SFX_DEUCESSH_ARCHIVE}" SFX_DEUCESSH_SHA256)
if(NOT SFX_OPENSSL_SHA256 STREQUAL "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a")
    message(FATAL_ERROR "Pinned OpenSSL archive hash mismatch")
endif()
if(NOT SFX_DEUCESSH_SHA256 STREQUAL "04ae1f08d17d221cefaff2ee67530f56d84f97307b64822c6192b22508fc828a")
    message(FATAL_ERROR "Pinned DeuceSSH archive hash mismatch")
endif()
set(SFX_OPENSSL_SOURCE "${SFX_SSH_THIRD_PARTY}/source/openssl-3.5.9")
set(SFX_DEUCESSH_SOURCE "${SFX_SSH_THIRD_PARTY}/source/DeuceSSH-ed49634ce5dbab8c236313358076f325da28fb5e")
foreach(component OPENSSL DEUCESSH)
    if(NOT EXISTS "${SFX_${component}_SOURCE}/CMakeLists.txt" AND
            NOT EXISTS "${SFX_${component}_SOURCE}/Configure")
        file(ARCHIVE_EXTRACT INPUT "${SFX_${component}_ARCHIVE}"
            DESTINATION "${SFX_SSH_THIRD_PARTY}/source")
    endif()
endforeach()
# Explicit owner/DR-reviewed downstream overlay. Never edit the archive or its
# extracted original source. Both review artifact and resulting source are pinned.
set(SFX_DEUCESSH_CREDIT_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deucessh/ed49634/patches/receive-window-v2/receive-credit-and-liveness.patch")
set(SFX_DEUCESSH_CREDIT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deucessh/ed49634/patches/receive-window-v2/ssh-conn.c")
file(SHA256 "${SFX_DEUCESSH_SOURCE}/ssh-conn.c" SFX_DEUCESSH_ORIGINAL_CONN_SHA256)
file(SHA256 "${SFX_DEUCESSH_CREDIT_PATCH}" SFX_DEUCESSH_CREDIT_PATCH_SHA256)
file(SHA256 "${SFX_DEUCESSH_CREDIT_SOURCE}" SFX_DEUCESSH_CREDIT_SOURCE_SHA256)
if(NOT SFX_DEUCESSH_ORIGINAL_CONN_SHA256 STREQUAL "3eec29d82ff4542c97993e4f9c9bce79fec2f2181aaf2648ac0de43caded876a" OR
   NOT SFX_DEUCESSH_CREDIT_PATCH_SHA256 STREQUAL "a3149a4982f2b1bd3ff74ab94f7c973a55318f9c27f7b333f6b667a9c5cc5e31" OR
   NOT SFX_DEUCESSH_CREDIT_SOURCE_SHA256 STREQUAL "2e6ac42217032431e6400c6b53842955caa191d98603b9739b5d24e055a44dfb")
    message(FATAL_ERROR "Reviewed SSH receive-credit overlay identity mismatch")
endif()
set(SFX_DEUCESSH_OVERLAY "${SFX_SSH_THIRD_PARTY}/reviewed-window-2e6ac4221703")
file(COPY "${SFX_DEUCESSH_SOURCE}/" DESTINATION "${SFX_DEUCESSH_OVERLAY}")
configure_file("${SFX_DEUCESSH_CREDIT_SOURCE}" "${SFX_DEUCESSH_OVERLAY}/ssh-conn.c" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SFX_DEUCESSH_CREDIT_PATCH}" "${SFX_DEUCESSH_CREDIT_SOURCE}")
# Owner-authorized bounded streaming overlay, built fresh with matching headers.
set(SFX_DEUCESSH_BOUNDED "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deucessh/ed49634/patches/bounded-stream-v3")
file(SHA256 "${SFX_DEUCESSH_BOUNDED}/ssh-conn.c" SFX_BOUNDED_HASH)
if(NOT SFX_BOUNDED_HASH STREQUAL "27472a62d961714e33dd6431e3baad927eb2ede89c6b41f92dc6b5dfb4ee87f5")
    message(FATAL_ERROR "Bounded SSH overlay identity mismatch: ssh-conn.c")
endif()
file(SHA256 "${SFX_DEUCESSH_BOUNDED}/deucessh-conn.h" SFX_BOUNDED_HASH)
if(NOT SFX_BOUNDED_HASH STREQUAL "5c43bf9b580a17ef399428b71c87e9c954dc795cb41e5f5d49d4da18432167b1")
    message(FATAL_ERROR "Bounded SSH overlay identity mismatch: deucessh-conn.h")
endif()
file(SHA256 "${SFX_DEUCESSH_BOUNDED}/ssh-internal.h" SFX_BOUNDED_HASH)
if(NOT SFX_BOUNDED_HASH STREQUAL "b262522670fefa5d93a458b786cd9bf5333de9212a7ff5716081ce8fd7189774")
    message(FATAL_ERROR "Bounded SSH overlay identity mismatch: ssh-internal.h")
endif()
file(SHA256 "${SFX_DEUCESSH_BOUNDED}/ssh-trans.c" SFX_BOUNDED_HASH)
if(NOT SFX_BOUNDED_HASH STREQUAL "5a19d02ede576aff9f442c7c1313bc81d69ded86f3dea88eead435ec1488d8ee")
    message(FATAL_ERROR "Bounded SSH overlay identity mismatch: ssh-trans.c")
endif()
file(SHA256 "${SFX_DEUCESSH_BOUNDED}/bounded-stream.patch" SFX_BOUNDED_HASH)
if(NOT SFX_BOUNDED_HASH STREQUAL "e5cf451622350e4e5d2b1dc40f9a9b5b7b357ae85552a6b5ced070800de1ef5e")
    message(FATAL_ERROR "Bounded SSH overlay identity mismatch: bounded-stream.patch")
endif()
set(SFX_DEUCESSH_OVERLAY "${SFX_SSH_THIRD_PARTY}/bounded-stream-df263417b430")
# Never transiently overwrite patched headers during an automatic reconfigure.
# Parallel build projects may still be compiling the matching overlay source.
file(COPY "${SFX_DEUCESSH_SOURCE}/" DESTINATION "${SFX_DEUCESSH_OVERLAY}"
    PATTERN "ssh-conn.c" EXCLUDE PATTERN "deucessh-conn.h" EXCLUDE
    PATTERN "ssh-internal.h" EXCLUDE PATTERN "ssh-trans.c" EXCLUDE)
foreach(name ssh-conn.c deucessh-conn.h ssh-internal.h ssh-trans.c)
    configure_file("${SFX_DEUCESSH_BOUNDED}/${name}" "${SFX_DEUCESSH_OVERLAY}/${name}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SFX_DEUCESSH_BOUNDED}/${name}")
endforeach()

