#pragma once

#include "smb.hpp"
#include "system.hpp"

namespace interfaces
{
struct MountPointStateMachine;
}

namespace resource
{

class Error : public std::runtime_error
{
  public:
    Error(std::errc errorCode, std::string message) :
        std::runtime_error(message), errorCode(errorCode)
    {
    }

    const std::errc errorCode;
};

class Directory
{
  public:
    Directory() = delete;
    Directory(const Directory&) = delete;
    Directory(Directory&& other) = delete;
    Directory& operator=(const Directory&) = delete;
    Directory& operator=(Directory&& other) = delete;

    explicit Directory(std::filesystem::path name) :
        path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ec;

        if (!std::filesystem::create_directory(path, ec))
        {
            LogMsg(Logger::Error, ec,
                   " : Unable to create mount directory: ", path);
            throw Error(std::errc::io_error,
                        "Failed to create mount directory");
        }
    }

    ~Directory()
    {
        std::error_code ec;

        if (!std::filesystem::remove(path, ec))
        {
            LogMsg(Logger::Error, ec, " : Unable to remove directory ", path);
        }
    }

    std::filesystem::path getPath() const
    {
        return path;
    }

  private:
    std::filesystem::path path;
};

class Mount
{
  public:
    Mount() = delete;
    Mount(const Mount&) = delete;
    Mount(Mount&& other) = delete;
    Mount& operator=(const Mount&) = delete;
    Mount& operator=(Mount&& other) = delete;

    explicit Mount(
        std::unique_ptr<Directory> directory, SmbShare& smb,
        const std::filesystem::path& remote, bool rw,
        const std::unique_ptr<utils::CredentialsProvider>& credentials) :
        directory(std::move(directory))
    {
        if (!smb.mount(remote, rw, credentials))
        {
            throw Error(std::errc::invalid_argument,
                        "Failed to mount CIFS share");
        }
    }

    ~Mount()
    {
        if (int result = ::umount(directory->getPath().string().c_str()))
        {
            LogMsg(Logger::Error, result, " : Unable to unmout directory ",
                   directory->getPath());
        }
    }

    std::filesystem::path getPath() const
    {
        return directory->getPath();
    }

  private:
    std::unique_ptr<Directory> directory;
};

class Process
{
  public:
    Process() = delete;
    Process(const Process&) = delete;
    Process(Process&& other) = delete;
    Process& operator=(const Process&) = delete;
    Process& operator=(Process&& other) = delete;
    Process(interfaces::MountPointStateMachine& machine,
            std::shared_ptr<::Process> process) :
        machine(&machine),
        process(std::move(process))
    {
        if (!this->process)
        {
            throw Error(std::errc::io_error, "Failed to create process");
        }
    }

    ~Process();

    template <class... Args>
    auto spawn(Args&&... args)
    {
        if (process->spawn(std::forward<Args>(args)...))
        {
            spawned = true;
            return true;
        }
        return false;
    }

  private:
    interfaces::MountPointStateMachine* machine;
    std::shared_ptr<::Process> process = nullptr;
    bool spawned = false;
};

class Gadget
{
  public:
    Gadget() = delete;
    Gadget& operator=(const Gadget&) = delete;
    Gadget& operator=(Gadget&& other) = delete;
    Gadget(const Gadget&) = delete;
    Gadget(Gadget&& other) = delete;

    Gadget(interfaces::MountPointStateMachine& machine, StateChange devState);
    ~Gadget();

  private:
    interfaces::MountPointStateMachine* machine;
    int32_t status;
};

} // namespace resource
