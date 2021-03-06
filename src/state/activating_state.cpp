#include "activating_state.hpp"

#include "active_state.hpp"

#include <sys/mount.h>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/process.hpp>
#include <boost/system/detail/error_code.hpp>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

ActivatingState::ActivatingState(interfaces::MountPointStateMachine& machine) :
    BasicStateT(machine)
{
}

std::unique_ptr<BasicState> ActivatingState::onEnter()
{
    // Reset previous exit code
    machine.getExitCode() = -1;

    if (machine.getConfig().mode == Configuration::Mode::proxy)
    {
        return activateProxyMode();
    }
    return activateLegacyMode();
}

std::unique_ptr<BasicState>
    ActivatingState::handleEvent(UdevStateChangeEvent event)
{
    if (event.devState == StateChange::inserted)
    {
        gadget = std::make_unique<resource::Gadget>(machine, event.devState);
        return std::make_unique<ActiveState>(machine, std::move(process),
                                             std::move(gadget));
    }

    return std::make_unique<DeactivatingState>(machine, std::move(process),
                                               std::move(gadget), event);
}

std::unique_ptr<BasicState> ActivatingState::handleEvent([
    [maybe_unused]] SubprocessStoppedEvent event)
{
    LogMsg(Logger::Error, "Process ended prematurely");
    return std::make_unique<ReadyState>(machine, std::errc::connection_refused,
                                        "Process ended prematurely");
}

std::unique_ptr<BasicState> ActivatingState::activateProxyMode()
{
    process = std::make_unique<resource::Process>(
        machine, std::make_shared<::Process>(
                     machine.getIoc(), machine.getName(),
                     "/usr/sbin/nbd-client", machine.getConfig().nbdDevice));

    if (!process->spawn(Configuration::MountPoint::toArgs(machine.getConfig()),
                        [&machine = machine](int exitCode) {
                            LogMsg(Logger::Info, machine.getName(),
                                   " process ended.");
                            machine.getExitCode() = exitCode;
                            machine.emitSubprocessStoppedEvent();
                        }))
    {
        return std::make_unique<ReadyState>(
            machine, std::errc::operation_canceled, "Failed to spawn process");
    }

    return nullptr;
}

std::unique_ptr<BasicState> ActivatingState::activateLegacyMode()
{
    LogMsg(Logger::Info, machine.getName(),
           " Mount requested on address: ", machine.getTarget()->imgUrl,
           " ; RW: ", machine.getTarget()->rw);

    std::filesystem::path socketPath(machine.getConfig().unixSocket);
    if (!std::filesystem::exists(socketPath.parent_path()))
    {
        LogMsg(Logger::Info, machine.getName(),
               " Parent path for the socket does not exist, ",
               socketPath.parent_path());

        std::error_code errc;
        std::filesystem::create_directories(socketPath.parent_path(), errc);
        if (errc)
        {
            LogMsg(Logger::Error, machine.getName(),
                   " Failed to create parent directory for socket", errc);
            return std::make_unique<ReadyState>(
                machine, static_cast<std::errc>(errc.value()),
                "Failed to create parent directory for socket");
        }
        std::filesystem::permissions(socketPath.parent_path(),
                                     std::filesystem::perms::owner_all, errc);
        if (errc)
        {
            LogMsg(Logger::Info, machine.getName(),
                   " Failed to set parent directory permissions for socket",
                   errc);
            return std::make_unique<ReadyState>(
                machine, static_cast<std::errc>(errc.value()),
                "Failed to set parent permissions directory for socket");
        }
    }

    if (isCifsUrl(machine.getTarget()->imgUrl))
    {
        return mountSmbShare();
    }
    if (isHttpsUrl(machine.getTarget()->imgUrl))
    {
        return mountHttpsShare();
    }

    return std::make_unique<ReadyState>(machine, std::errc::invalid_argument,
                                        "URL not recognized");
}

std::unique_ptr<BasicState> ActivatingState::mountSmbShare()
{
    try
    {
        auto mountDir =
            std::make_unique<resource::Directory>(machine.getName());

        SmbShare smb(mountDir->getPath());
        fs::path remote = getImagePath(machine.getTarget()->imgUrl);
        auto remoteParent = "/" + remote.parent_path().string();
        auto localFile = mountDir->getPath() / remote.filename();

        LogMsg(Logger::Info, machine.getName(), " Remote name: ", remote,
               "\n Remote parent: ", remoteParent,
               "\n Local file: ", localFile);

        machine.getTarget()->mountPoint = std::make_unique<resource::Mount>(
            std::move(mountDir), smb, remoteParent, machine.getTarget()->rw,
            machine.getTarget()->credentials);

        process = spawnNbdKit(machine, localFile);
        if (!process)
        {
            return std::make_unique<ReadyState>(machine,
                                                std::errc::operation_canceled,
                                                "Unable to setup NbdKit");
        }

        return nullptr;
    }
    catch (const resource::Error& e)
    {
        return std::make_unique<ReadyState>(machine, e.errorCode, e.what());
    }
}

std::unique_ptr<BasicState> ActivatingState::mountHttpsShare()
{
    process = spawnNbdKit(machine, machine.getTarget()->imgUrl);
    if (!process)
    {
        return std::make_unique<ReadyState>(machine,
                                            std::errc::invalid_argument,
                                            "Failed to mount HTTPS share");
    }

    return nullptr;
}

std::unique_ptr<resource::Process>
    ActivatingState::spawnNbdKit(interfaces::MountPointStateMachine& machine,
                                 std::unique_ptr<utils::VolatileFile>&& secret,
                                 const std::vector<std::string>& params)
{
    // Investigate
    auto process = std::make_unique<resource::Process>(
        machine, std::make_shared<::Process>(
                     machine.getIoc(), std::string(machine.getName()),
                     "/usr/sbin/nbdkit", machine.getConfig().nbdDevice));

    // Cleanup of previous socket
    if (fs::exists(machine.getConfig().unixSocket))
    {
        LogMsg(Logger::Debug, machine.getName(),
               " Removing previously mounted socket: ",
               machine.getConfig().unixSocket);
        if (!fs::remove(machine.getConfig().unixSocket))
        {
            LogMsg(Logger::Error, machine.getName(),
                   " Unable to remove pre-existing socket :",
                   machine.getConfig().unixSocket);
            return {};
        }
    }

    std::string nbdClient =
        "/usr/sbin/nbd-client " +
        boost::algorithm::join(
            Configuration::MountPoint::toArgs(machine.getConfig()), " ");

    std::vector<std::string> args = {
        // Listen for client on this unix socket...
        "--unix",
        machine.getConfig().unixSocket,

        // ... then connect nbd-client to served image
        "--run",
        nbdClient,

#if VM_VERBOSE_NBDKIT_LOGS
        "--verbose", // swarm of debug logs - only for brave souls
#endif
    };

    if (!machine.getTarget()->rw)
    {
        args.emplace_back("--readonly");
    }

    // Insert extra params
    args.insert(args.end(), params.begin(), params.end());

    if (!process->spawn(args, [&machine = machine,
                               secret = std::move(secret)](int exitCode) {
            LogMsg(Logger::Info, machine.getName(), " process ended.");
            machine.getExitCode() = exitCode;
            machine.emitSubprocessStoppedEvent();
        }))
    {
        LogMsg(Logger::Error, machine.getName(),
               " Failed to spawn Process for: ", machine.getName());
        return {};
    }

    return process;
}

std::unique_ptr<resource::Process>
    ActivatingState::spawnNbdKit(interfaces::MountPointStateMachine& machine,
                                 const fs::path& file)
{
    return spawnNbdKit(machine, {},
                       {// Use file plugin ...
                        "file",
                        // ... to mount file at this location
                        "file=" + file.string()});
}

std::unique_ptr<resource::Process>
    ActivatingState::spawnNbdKit(interfaces::MountPointStateMachine& machine,
                                 const std::string& url)
{
    std::unique_ptr<utils::VolatileFile> secret;
    std::vector<std::string> params = {
        // Use curl plugin ...
        "curl",
        // ... to mount http resource at url
        "url=" + url,
        // custom OpenBMC path for CA
        "cainfo=", "capath=/etc/ssl/certs/authority", "ssl-version=tlsv1.2",
        "followlocation=false",
        "ssl-cipher-list="
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES256-GCM-SHA384",
        "tls13-ciphers="
        "TLS_AES_256_GCM_SHA384"};

    // Authenticate if needed
    if (machine.getTarget()->credentials)
    {
        // Pack password into buffer
        utils::CredentialsProvider::SecureBuffer buff =
            machine.getTarget()->credentials->pack(
                []([[maybe_unused]] const std::string& user,
                   const std::string& pass, std::vector<char>& buff) {
                    std::copy(pass.begin(), pass.end(),
                              std::back_inserter(buff));
                });

        // Prepare file to provide the password with
        secret = std::make_unique<utils::VolatileFile>(std::move(buff));

        params.push_back("user=" + machine.getTarget()->credentials->user());
        params.push_back("password=+" + secret->path());
    }

    return spawnNbdKit(machine, std::move(secret), params);
}

bool ActivatingState::checkUrl(const std::string& urlScheme,
                               const std::string& imageUrl)
{
    return (urlScheme.compare(imageUrl.substr(0, urlScheme.size())) == 0);
}

bool ActivatingState::getImagePathFromUrl(const std::string& urlScheme,
                                          const std::string& imageUrl,
                                          std::string* imagePath)
{
    if (checkUrl(urlScheme, imageUrl))
    {
        if (imagePath != nullptr)
        {
            *imagePath = imageUrl.substr(urlScheme.size() - 1);
            return true;
        }

        LogMsg(Logger::Error, "Invalid parameter provied");
        return false;
    }

    LogMsg(Logger::Error, "Provided url does not match scheme");
    return false;
}

bool ActivatingState::isHttpsUrl(const std::string& imageUrl)
{
    return checkUrl("https://", imageUrl);
}

bool ActivatingState::getImagePathFromHttpsUrl(const std::string& imageUrl,
                                               std::string* imagePath)
{
    return getImagePathFromUrl("https://", imageUrl, imagePath);
}

bool ActivatingState::isCifsUrl(const std::string& imageUrl)
{
    return checkUrl("smb://", imageUrl);
}

bool ActivatingState::getImagePathFromCifsUrl(const std::string& imageUrl,
                                              std::string* imagePath)
{
    return getImagePathFromUrl("smb://", imageUrl, imagePath);
}

fs::path ActivatingState::getImagePath(const std::string& imageUrl)
{
    std::string imagePath;

    if (isHttpsUrl(imageUrl) && getImagePathFromHttpsUrl(imageUrl, &imagePath))
    {
        return {imagePath};
    }
    if (isCifsUrl(imageUrl) && getImagePathFromCifsUrl(imageUrl, &imagePath))
    {
        return {imagePath};
    }

    LogMsg(Logger::Error, "Unrecognized url's scheme encountered");
    return {""};
}
