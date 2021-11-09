#pragma once

#include "logger.hpp"
#include "utils.hpp"

#include <sys/mount.h>

#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

class SmbShare
{
  public:
    SmbShare(const fs::path& mountDir) : mountDir(mountDir)
    {
    }

    bool mount(const fs::path& remote, bool rw,
               const std::unique_ptr<utils::CredentialsProvider>& credentials)
    {
        LogMsg(Logger::Debug, "Trying to mount remote : ", remote);

        const std::string params = "sec=ntlmsspi,seal";
        const std::string perm = rw ? "rw" : "ro";
        std::string options = params + "," + perm;
        std::string credentialsOpt;

        if (!credentials)
        {
            LogMsg(Logger::Info, "Mounting as Guest");
            credentialsOpt = "guest,username=OpenBmc";
        }
        else
        {
            if (!validateUsername(credentials->user()))
            {
                LogMsg(Logger::Error,
                       "Username for CIFS share can't contain ',' character");
                return false;
            }
            credentials->escapeCommas();
            credentialsOpt = "username=" + credentials->user() +
                             ",password=" + credentials->password();
        }
        options += "," + credentialsOpt;

        std::string versionOpt = "vers=3.1.1";
        auto ec = mountWithSmbVers(remote, options, versionOpt);

        if (ec)
        {
            // vers=3 will negotiate max version from 3.02 and 3.0
            versionOpt = "vers=3";
            ec = mountWithSmbVers(remote, options, versionOpt);
        }

        utils::secureCleanup(options);
        utils::secureCleanup(credentialsOpt);

        if (ec)
        {
            return false;
        }
        return true;
    }

  private:
    std::string mountDir;

    /* Check if username does not contain comma (,) character */
    bool validateUsername(const std::string& username)
    {
        return username.find(',') == std::string::npos;
    }

    int mountWithSmbVers(const fs::path& remote, std::string options,
                         const std::string& version)
    {
        options += "," + version;

        auto ec = ::mount(remote.c_str(), mountDir.c_str(), "cifs", 0,
                          options.c_str());
        utils::secureCleanup(options);

        if (ec)
        {
            LogMsg(Logger::Info, "Mount failed for ", version,
                   " with ec = ", ec, " errno = ", errno);
        }

        return ec;
    }
};
