#pragma once

#include <cstddef>
#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sfx::platform::network {
#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket invalid_socket = INVALID_SOCKET;
inline constexpr int socket_error = SOCKET_ERROR;
inline constexpr int shutdown_send = SD_SEND;
inline constexpr int shutdown_both = SD_BOTH;
inline std::uint64_t monotonic_millis() noexcept { return GetTickCount64(); }
inline bool retry_receive_error() noexcept {
    const auto error = WSAGetLastError();
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}
inline int send_bytes(Socket socket, const char* bytes, int count) noexcept {
    return ::send(socket, bytes, count, 0);
}
inline int receive_bytes(Socket socket, char* bytes, int count) noexcept {
    return ::recv(socket, bytes, count, 0);
}
inline bool set_timeout(Socket socket, int option, std::uint32_t millis) noexcept {
    const DWORD value = millis;
    return setsockopt(socket, SOL_SOCKET, option,
        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}
inline bool set_nonblocking(Socket socket,bool enabled) noexcept {
    u_long value=enabled?1UL:0UL;return ioctlsocket(socket,FIONBIO,&value)==0;
}
inline int readable_now(Socket socket) noexcept {
    fd_set ready;FD_ZERO(&ready);FD_SET(socket,&ready);timeval immediate{};
    return ::select(0,&ready,nullptr,nullptr,&immediate);
}
#else
using Socket = int;
inline constexpr Socket invalid_socket = -1;
inline constexpr int socket_error = -1;
inline constexpr int shutdown_send = SHUT_WR;
inline constexpr int shutdown_both = SHUT_RDWR;
inline std::uint64_t monotonic_millis() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline bool retry_receive_error() noexcept {
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
}
inline int send_bytes(Socket socket, const char* bytes, int count) noexcept {
    return static_cast<int>(::send(socket, bytes, static_cast<std::size_t>(count), MSG_NOSIGNAL));
}
inline int receive_bytes(Socket socket, char* bytes, int count) noexcept {
    return static_cast<int>(::recv(socket, bytes, static_cast<std::size_t>(count), 0));
}
inline bool set_timeout(Socket socket, int option, std::uint32_t millis) noexcept {
    const timeval value{static_cast<time_t>(millis / 1000U),
        static_cast<suseconds_t>((millis % 1000U) * 1000U)};
    return setsockopt(socket, SOL_SOCKET, option, &value, sizeof(value)) == 0;
}
inline bool set_nonblocking(Socket socket,bool enabled) noexcept {
    const int flags=fcntl(socket,F_GETFL,0);
    return flags>=0&&fcntl(socket,F_SETFL,enabled?(flags|O_NONBLOCK):(flags&~O_NONBLOCK))==0;
}
inline int readable_now(Socket socket) noexcept {
    pollfd request{socket,POLLIN,0};
    const int result=poll(&request,1,0);
    if(result<0&&errno==EINTR)return 0;
    return result;
}
#endif
} // namespace sfx::platform::network
