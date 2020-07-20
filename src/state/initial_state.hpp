#include "active_state.hpp"
#include "basic_state.hpp"
#include "ready_state.hpp"

struct InitialState : public BasicStateT<InitialState>
{
    static std::string_view stateName()
    {
        return "InitialState";
    }

    InitialState(interfaces::MountPointStateMachine& machine) :
        BasicStateT(machine){};

    std::unique_ptr<BasicState> handleEvent(RegisterDbusEvent event)
    {
        const bool isLegacy =
            (machine.getConfig().mode == Configuration::Mode::legacy);

#if !LEGACY_MODE_ENABLED
        if (isLegacy)
        {
            return std::make_unique<ReadyState>(machine,
                                                std::errc::invalid_argument,
                                                "Legacy mode is not supported");
        }
#endif
        addMountPointInterface(event);
        addProcessInterface(event);
        addServiceInterface(event, isLegacy);

        return std::make_unique<ReadyState>(machine);
    }

    template <class AnyEvent>
    std::unique_ptr<BasicState> handleEvent(AnyEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        return nullptr;
    }

  private:
    static std::string
        getObjectPath(interfaces::MountPointStateMachine& machine)
    {
        LogMsg(Logger::Debug, "getObjectPath entry()");
        std::string objPath;
        if (machine.getConfig().mode == Configuration::Mode::proxy)
        {
            objPath = "/xyz/openbmc_project/VirtualMedia/Proxy/";
        }
        else
        {
            objPath = "/xyz/openbmc_project/VirtualMedia/Legacy/";
        }
        return objPath;
    }

    void addProcessInterface(const RegisterDbusEvent& event)
    {
        std::string objPath = getObjectPath(machine);

        auto processIface = event.objServer->add_interface(
            objPath + std::string(machine.getName()),
            "xyz.openbmc_project.VirtualMedia.Process");

        processIface->register_property(
            "Active", bool(false),
            [](const bool& req, bool& property) { return 0; },
            [& machine = machine](const bool& property) -> bool {
                return machine.getState().get_if<ActiveState>();
            });
        processIface->register_property(
            "ExitCode", int32_t(0),
            [](const int32_t& req, int32_t& property) { return 0; },
            [& machine = machine](const int32_t& property) {
                return machine.getExitCode();
            });
        processIface->initialize();
    }

    void addMountPointInterface(const RegisterDbusEvent& event)
    {
        std::string objPath = getObjectPath(machine);

        auto iface = event.objServer->add_interface(
            objPath + std::string(machine.getName()),
            "xyz.openbmc_project.VirtualMedia.MountPoint");
        iface->register_property("Device",
                                 machine.getConfig().nbdDevice.to_string());
        iface->register_property("EndpointId", machine.getConfig().endPointId);
        iface->register_property("Socket", machine.getConfig().unixSocket);
        iface->register_property(
            "RemainingInactivityTimeout", 0,
            [](const int& req, int& property) {
                throw sdbusplus::exception::SdBusError(
                    EPERM, "Setting RemainingInactivityTimeout property is "
                           "not allowed");
                return -1;
            },
            [& config = machine.getConfig()](const int& property) -> int {
                return config.remainingInactivityTimeout.count();
            });

        iface->initialize();
    }

    void addServiceInterface(const RegisterDbusEvent& event,
                             const bool isLegacy)
    {
        const std::string name = "xyz.openbmc_project.VirtualMedia." +
                                 std::string(isLegacy ? "Legacy" : "Proxy");

        const std::string path =
            getObjectPath(machine) + std::string(machine.getName());

        auto iface = event.objServer->add_interface(path, name);

        const auto timerPeriod = std::chrono::milliseconds(100);
        const auto duration = std::chrono::seconds(
            machine.getConfig().timeout.value_or(
                Configuration::MountPoint::defaultTimeout) +
            5);
        const auto waitCnt =
            std::chrono::duration_cast<std::chrono::milliseconds>(duration) /
            timerPeriod;
        LogMsg(Logger::Debug, "[App] waitCnt == ", waitCnt);

        // Common unmount
        iface->register_method(
            "Unmount", [& machine = machine, waitCnt,
                        timerPeriod](boost::asio::yield_context yield) {
                LogMsg(Logger::Info, "[App]: Unmount called on ",
                       machine.getName());
                try
                {
                    machine.emitUnmountEvent();
                }
                catch (const std::exception& e)
                {
                    LogMsg(Logger::Error, e.what());
                    throw sdbusplus::exception::SdBusError(EPERM, e.what());
                    return false;
                }

                auto repeats = waitCnt;
                boost::asio::steady_timer timer(machine.getIoc());
                while (repeats > 0)
                {
                    if (machine.getState().get_if<ReadyState>())
                    {
                        LogMsg(Logger::Debug, "[App] Unmount ok");
                        return true;
                    }
                    boost::system::error_code ignored_ec;
                    timer.expires_from_now(timerPeriod);
                    timer.async_wait(yield[ignored_ec]);
                    repeats--;
                }
                LogMsg(Logger::Error,
                       "[App] timedout when waiting for ReadyState");
                return false;
            });

        // Common mount
        const auto handleMount =
            [waitCnt, timerPeriod](
                boost::asio::yield_context yield,
                interfaces::MountPointStateMachine& machine,
                std::optional<interfaces::MountPointStateMachine::Target>
                    target) {
                try
                {
                    machine.emitMountEvent(std::move(target));
                }
                catch (const std::exception& e)
                {
                    LogMsg(Logger::Error, e.what());
                    throw sdbusplus::exception::SdBusError(EPERM, e.what());
                    return false;
                }

                auto repeats = waitCnt;
                boost::asio::steady_timer timer(machine.getIoc());
                while (repeats > 0)
                {
                    if (auto s = machine.getState().get_if<ReadyState>())
                    {
                        if (s->error)
                        {
                            LogMsg(Logger::Error, s->error->message.c_str());
                            throw sdbusplus::exception::SdBusError(
                                static_cast<int>(s->error->code),
                                s->error->message.c_str());
                        }
                        LogMsg(Logger::Error, "[App] Mount failed");
                        return false;
                    }
                    if (machine.getState().get_if<ActiveState>())
                    {
                        LogMsg(Logger::Debug, "[App] Mount ok");
                        return true;
                    }
                    boost::system::error_code ignored_ec;
                    timer.expires_from_now(timerPeriod);
                    timer.async_wait(yield[ignored_ec]);
                    repeats--;
                }
                LogMsg(Logger::Error,
                       "[App] timedout when waiting for ActiveState");
                return false;
            };

        // Mount specialization
        if (isLegacy)
        {
            using sdbusplus::message::unix_fd;
            using optional_fd = std::variant<int, unix_fd>;

            iface->register_method(
                "Mount", [& machine = machine, handleMount](
                             boost::asio::yield_context yield,
                             std::string imgUrl, bool rw, optional_fd fd) {
                    LogMsg(Logger::Info, "[App]: Mount called on ",
                           getObjectPath(machine), machine.getName());

                    interfaces::MountPointStateMachine::Target target = {imgUrl,
                                                                         rw};

                    if (std::holds_alternative<unix_fd>(fd))
                    {
                        LogMsg(Logger::Debug, "[App] Extra data available");

                        // Open pipe and prepare output buffer
                        boost::asio::posix::stream_descriptor secretPipe(
                            machine.getIoc(), dup(std::get<unix_fd>(fd).fd));
                        std::array<char, utils::secretLimit> buf;

                        // Read data
                        auto size = secretPipe.async_read_some(
                            boost::asio::buffer(buf), yield);

                        // Validate number of NULL delimiters, ensures
                        // further operations are safe
                        auto nullCount =
                            std::count(buf.begin(), buf.begin() + size, '\0');
                        if (nullCount != 2)
                        {
                            throw sdbusplus::exception::SdBusError(
                                EINVAL, "Malformed extra data");
                        }

                        // First 'part' of payload
                        std::string user(buf.begin());
                        // Second 'part', after NULL delimiter
                        std::string pass(buf.begin() + user.length() + 1);

                        // Encapsulate credentials into safe buffer
                        target.credentials =
                            std::make_unique<utils::CredentialsProvider>(
                                std::move(user), std::move(pass));

                        // Cover the tracks
                        utils::secureCleanup(buf);
                    }

                    try
                    {
                        auto ret =
                            handleMount(yield, machine, std::move(target));
                        if (machine.getTarget())
                        {
                            machine.getTarget()->credentials.reset();
                        }
                        LogMsg(Logger::Debug, "[App]: mount completed ", ret);
                        return ret;
                    }
                    catch (const std::exception& e)
                    {
                        LogMsg(Logger::Error, e.what());
                        if (machine.getTarget())
                        {
                            machine.getTarget()->credentials.reset();
                        }
                        throw;
                        return false;
                    }
                    catch (...)
                    {
                        if (machine.getTarget())
                        {
                            machine.getTarget()->credentials.reset();
                        }
                        throw;
                        return false;
                    }
                });
        }
        else
        {
            iface->register_method(
                "Mount", [& machine = machine,
                          handleMount](boost::asio::yield_context yield) {
                    LogMsg(Logger::Info, "[App]: Mount called on ",
                           getObjectPath(machine), machine.getName());

                    return handleMount(yield, machine, std::nullopt);
                });
        }

        iface->initialize();
    }
};