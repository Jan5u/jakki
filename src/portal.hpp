#pragma once

#include <cstdint>

#include <libportal/portal.h>
#include <libportal/remote.h>

class Portal {
  public:
    Portal();
    ~Portal();

    Portal(const Portal &) = delete;
    Portal &operator=(const Portal &) = delete;

    bool openScreenCastPortal(uint32_t &nodeId, int &pipewireFd);
    void close();

  private:
    XdpPortal *m_portal = nullptr;
    XdpSession *m_session = nullptr;
};