#pragma once

#include "basic_state.hpp"
#include "deactivating_state.hpp"

struct ActiveState : public BasicStateT<ActiveState>
{
    static std::string_view stateName()
    {
        return "ActiveState";
    }

    ActiveState(interfaces::MountPointStateMachine& machine,
                std::unique_ptr<resource::Process> process,
                std::unique_ptr<resource::Gadget> gadget) :
        BasicStateT(machine),
        process(std::move(process)), gadget(std::move(gadget))
    {
        machine.notify();
    };

    virtual std::unique_ptr<BasicState> onEnter()
    {
        handler = [this](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }

            auto now = std::chrono::steady_clock::now();

            auto stats = UsbGadget::getStats(std::string(machine.getName()));
            if (stats && (*stats != lastStats))
            {
                lastStats = std::move(*stats);
                lastAccess = now;
            }

            auto timeSinceLastAccess =
                std::chrono::duration_cast<std::chrono::seconds>(now -
                                                                 lastAccess);
            if (timeSinceLastAccess >= Configuration::inactivityTimeout)
            {
                LogMsg(Logger::Info, machine.getName(),
                       " Inactivity timer expired (",
                       Configuration::inactivityTimeout.count(),
                       "s) - Unmounting");
                // unmount media & stop retriggering timer
                boost::asio::post(machine.getIoc(), [&machine = machine]() {
                    machine.emitUnmountEvent();
                });
                return;
            }
            else
            {
                machine.getConfig().remainingInactivityTimeout =
                    Configuration::inactivityTimeout - timeSinceLastAccess;
            }

            timer.expires_from_now(std::chrono::seconds(1));
            timer.async_wait(handler);
        };
        timer.expires_from_now(std::chrono::seconds(1));
        timer.async_wait(handler);

        return nullptr;
    }

    std::unique_ptr<BasicState> handleEvent(UdevStateChangeEvent event)
    {
        return std::make_unique<DeactivatingState>(
            machine, std::move(process), std::move(gadget), std::move(event));
    }

    std::unique_ptr<BasicState> handleEvent(SubprocessStoppedEvent event)
    {
        return std::make_unique<DeactivatingState>(
            machine, std::move(process), std::move(gadget), std::move(event));
    }

    std::unique_ptr<BasicState> handleEvent([[maybe_unused]] UnmountEvent event)
    {
        machine.notificationStart();
        return std::make_unique<DeactivatingState>(machine, std::move(process),
                                                   std::move(gadget));
    }

    [[noreturn]] std::unique_ptr<BasicState> handleEvent(MountEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        throw sdbusplus::exception::SdBusError(
            EPERM, "Operation not permitted in active state");
    }

    template <class AnyEvent>
    [[noreturn]] std::unique_ptr<BasicState> handleEvent(AnyEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        throw sdbusplus::exception::SdBusError(
            EOPNOTSUPP, "Operation not supported in active state");
    }

  private:
    boost::asio::steady_timer timer{machine.getIoc()};
    std::unique_ptr<resource::Process> process;
    std::unique_ptr<resource::Gadget> gadget;
    std::function<void(const boost::system::error_code&)> handler;
    std::chrono::time_point<std::chrono::steady_clock> lastAccess;
    std::string lastStats;
};
