#include "ipc/ipc_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr char kCallerCwdSeparator = '\x1e';

  std::string resolveSocketPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') {
      runtime = "/tmp";
    }
    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      display = "wayland-0";
    }
    return std::string(runtime) + "/helix-" + display + ".sock";
  }

} // namespace

bool IpcClient::exchange(const std::string& line, std::string& response, std::string& error,
                         const std::string& socketPath) {
  const std::string path = socketPath.empty() ? resolveSocketPath() : socketPath;

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    error = std::string("socket() failed: ") + std::strerror(errno);
    return false;
  }

  // Set connect/send/recv timeout to 2 seconds
  timeval tv{};
  tv.tv_sec = 2;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    error = "socket path too long";
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    error = "helix is not running";
    ::close(fd);
    return false;
  }

  std::size_t sent = 0;
  while (sent < line.size()) {
    const auto written = ::write(fd, line.data() + sent, line.size() - sent);
    if (written < 0) {
      error = std::string("write() failed: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }

  if (::shutdown(fd, SHUT_WR) < 0) {
    error = std::string("shutdown() failed: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }

  // Read response until EOF (server closes connection after writing)
  response.clear();
  char buf[4096];
  for (;;) {
    const auto n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::string("read() failed: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (n == 0) {
      break;
    }
    response.append(buf, static_cast<std::size_t>(n));
  }

  ::close(fd);
  return true;
}

int IpcClient::send(const std::string& command) {
  // Prefix the caller cwd so the daemon resolves relative paths correctly.
  std::string line;
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec && cwd.is_absolute()) {
    line = cwd.string();
    line += kCallerCwdSeparator;
  }
  line += command;

  std::string response;
  std::string error;
  if (!exchange(line, response, error)) {
    std::println(stderr, "error: {}", error);
    return 1;
  }

  std::fputs(response.c_str(), stdout);

  // Return 1 if the response indicates an error
  return (response.starts_with("error:")) ? 1 : 0;
}
