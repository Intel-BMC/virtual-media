#pragma once

#include "configuration.hpp"
#include "resources.hpp"

struct BasicState;

namespace interfaces
{

struct MountPointStateMachine
{
    struct Target
    {
        std::string imgUrl;
        bool rw;
        std::unique_ptr<resource::Mount> mountPoint;
        std::unique_ptr<utils::CredentialsProvider> credentials;
    };

    virtual ~MountPointStateMachine() = default;

    virtual std::string_view getName() const = 0;
    virtual Configuration::MountPoint& getConfig() = 0;
    virtual std::optional<Target>& getTarget() = 0;
    virtual BasicState& getState() = 0;
    virtual int& getExitCode() = 0;
    virtual boost::asio::io_context& getIoc() = 0;

    virtual void emitRegisterDBusEvent(
        std::shared_ptr<sdbusplus::asio::connection> bus,
        std::shared_ptr<sdbusplus::asio::object_server> objServer) = 0;
    virtual void emitMountEvent(std::optional<Target>) = 0;
    virtual void emitUnmountEvent() = 0;
    virtual void emitSubprocessStoppedEvent() = 0;
    virtual void emitUdevStateChangeEvent(const NBDDevice& dev,
                                          StateChange devState) = 0;
};

} // namespace interfaces