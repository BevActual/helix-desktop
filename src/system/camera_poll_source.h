#pragma once

#include "app/poll_source.h"
#include "system/camera_service.h"

class CameraPollSource final : public PollSource {
public:
  explicit CameraPollSource(CameraService& service) : m_service(service) {}

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (m_service.fd() >= 0 && (fds[startIdx].revents & POLLIN) != 0) {
      m_service.dispatch();
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (m_service.fd() >= 0) {
      fds.push_back({.fd = m_service.fd(), .events = POLLIN, .revents = 0});
    }
  }

private:
  CameraService& m_service;
};
