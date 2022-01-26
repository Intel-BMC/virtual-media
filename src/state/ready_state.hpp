#pragma once

#include "activating_state.hpp"
#include "basic_state.hpp"
#include "logger.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

struct ReadyState : public BasicStateT<ReadyState>
{
    static std::string_view stateName()
    {
        return "ReadyState";
    }

    struct Error
    {
        std::errc code;
        std::string message;
    };

    ReadyState(interfaces::MountPointStateMachine& machine) :
        BasicStateT(machine){};

    ReadyState(interfaces::MountPointStateMachine& machine, const std::errc& ec,
               const std::string& message) :
        BasicStateT(machine),
        error{{ec, message}}
    {
        LogMsg(Logger::Error, machine.getName(),
               " Errno = ", static_cast<int>(ec), " : ", message);
    }

    std::unique_ptr<BasicState> onEnter() override
    {
        // Cleanup after previously mounted device
        LogMsg(Logger::Debug, "exitCode: ", machine.getExitCode());
        machine.getTarget() = std::nullopt;
        machine.getConfig().remainingInactivityTimeout =
            std::chrono::seconds(0);
        return nullptr;
    }

    std::unique_ptr<BasicState> handleEvent(MountEvent event)
    {
        if (event.target)
        {
            machine.getTarget() = std::move(event.target);
        }
        return std::make_unique<ActivatingState>(machine);
    }

    [[noreturn]] std::unique_ptr<BasicState> handleEvent(UnmountEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        throw sdbusplus::exception::SdBusError(
            EPERM, "Operation not permitted in ready state");
    }

    template <class AnyEvent>
    std::unique_ptr<BasicState> handleEvent(AnyEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        return nullptr;
    }

    std::optional<Error> error;
};
