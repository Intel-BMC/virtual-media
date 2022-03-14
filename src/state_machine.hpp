#pragma once
#include "interfaces/mount_point_state_machine.hpp"
#include "state/initial_state.hpp"
#include "utils.hpp"

#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>
#include <sdbusplus/asio/object_server.hpp>
#include <system_error>

struct MountPointStateMachine : public interfaces::MountPointStateMachine
{
    MountPointStateMachine(boost::asio::io_context& ioc,
                           DeviceMonitor& devMonitor, const std::string& name,
                           const Configuration::MountPoint& config) :
        ioc{ioc},
        name{name}, config{config}
    {
        devMonitor.addDevice(config.nbdDevice);
    }

    MountPointStateMachine& operator=(MountPointStateMachine&&) = delete;

    std::string_view getName() const override
    {
        return name;
    }

    Configuration::MountPoint& getConfig() override
    {
        return config;
    }

    std::optional<Target>& getTarget() override
    {
        return target;
    }

    BasicState& getState() override
    {
        return *state;
    }

    int& getExitCode() override
    {
        return exitCode;
    }

    boost::asio::io_context& getIoc() override
    {
        return ioc;
    }

    void changeState(std::unique_ptr<BasicState> newState)
    {
        state = std::move(newState);
        LogMsg(Logger::Info, name, " state changed to ", state->getStateName());
        if ((newState = state->onEnter()))
        {
            changeState(std::move(newState));
        }
    }

    template <class EventT>
    void emitEvent(EventT&& event)
    {
        LogMsg(Logger::Info, name, " received ", event.eventName, " while in ",
               state->getStateName());

        if (auto newState = state->handleEvent(std::move(event)))
        {
            changeState(std::move(newState));
        }
    }

    void emitRegisterDBusEvent(
        std::shared_ptr<sdbusplus::asio::connection> bus,
        std::shared_ptr<sdbusplus::asio::object_server> objServer) override
    {
        emitEvent(RegisterDbusEvent(bus, objServer));
    }

    void emitMountEvent(std::optional<Target> newTarget) override
    {
        emitEvent(MountEvent(std::move(newTarget)));
    }

    void emitUnmountEvent() override
    {
        emitEvent(UnmountEvent());
    }

    void emitSubprocessStoppedEvent() override
    {
        emitEvent(SubprocessStoppedEvent());
    }

    void emitUdevStateChangeEvent(const NBDDevice& dev,
                                  StateChange devState) override
    {
        if (config.nbdDevice == dev)
        {
            emitEvent(UdevStateChangeEvent(devState));
        }
        else
        {
            LogMsg(Logger::Debug, name, " Ignoring request.");
        }
    }

    virtual void
        notificationInitialize(std::shared_ptr<sdbusplus::asio::connection> con,
                               const std::string& svc, const std::string& iface,
                               const std::string& name) override
    {
        auto signal = std::make_unique<utils::SignalSender>(std::move(con), svc,
                                                            iface, name);

        auto timer = std::make_unique<boost::asio::steady_timer>(ioc);

        completionNotification = std::make_unique<utils::NotificationWrapper>(
            std::move(signal), std::move(timer));
    }
    void notificationStart()
    {
        auto notificationHandler = [this](const boost::system::error_code& ec) {
            if (ec == boost::system::errc::operation_canceled)
            {
                return;
            }

            LogMsg(Logger::Error,
                   "[App] timedout when waiting for target state");

            completionNotification->notify(
                std::make_error_code(std::errc::device_or_resource_busy));
        };

        LogMsg(Logger::Debug, "Started notification");
        completionNotification->start(
            std::move(notificationHandler),
            std::chrono::seconds(
                config.timeout.value_or(
                    Configuration::MountPoint::defaultTimeout) +
                5));
    }

    virtual void notify(const std::error_code& ec = {}) override
    {
        completionNotification->notify(ec);
    }

    boost::asio::io_context& ioc;
    std::string name;
    Configuration::MountPoint config;
    std::unique_ptr<utils::NotificationWrapper> completionNotification;

    std::optional<Target> target;
    std::unique_ptr<BasicState> state = std::make_unique<InitialState>(*this);
    int exitCode = -1;
};
