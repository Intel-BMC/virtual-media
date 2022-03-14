#pragma once

#include "logger.hpp"

#include <unistd.h>

#include <algorithm>
#include <boost/asio/steady_timer.hpp>
#include <boost/process/async_pipe.hpp>
#include <boost/type_traits/has_dereference.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <sdbusplus/asio/object_server.hpp>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace utils
{
constexpr const size_t secretLimit = 1024;

template <typename T>
static void secureCleanup(T& value)
{
    auto raw = const_cast<typename T::value_type*>(&value[0]);
    explicit_bzero(raw, value.size() * sizeof(*raw));
}

class Credentials
{
  public:
    Credentials(std::string&& user, std::string&& password) :
        userBuf(std::move(user)), passBuf(std::move(password))
    {
    }

    ~Credentials()
    {
        secureCleanup(userBuf);
        secureCleanup(passBuf);
    }

    const std::string& user()
    {
        return userBuf;
    }

    const std::string& password()
    {
        return passBuf;
    }

    void escapeCommas()
    {
        if (!commasEscaped)
        {
            escapeComma(passBuf);
            commasEscaped = true;
        }
    }

  private:
    Credentials() = delete;
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;

    /* escape ',' (coma) by ',,' */
    void escapeComma(std::string& s)
    {
        std::string temp;
        std::for_each(s.begin(), s.end(), [&temp](const auto& c) {
            *std::back_inserter(temp) = c;
            if (c == ',')
            {
                *std::back_inserter(temp) = c;
            }
        });
        std::swap(s, temp);
        secureCleanup(temp);
    }

    std::string userBuf;
    std::string passBuf;
    bool commasEscaped{false};
};

class CredentialsProvider
{
  public:
    template <typename T>
    struct Deleter
    {
        void operator()(T* buff) const
        {
            if (buff)
            {
                secureCleanup(*buff);
                delete buff;
            }
        }
    };

    using Buffer = std::vector<char>;
    using SecureBuffer = std::unique_ptr<Buffer, Deleter<Buffer>>;
    // Using explicit definition instead of std::function to avoid implicit
    // conversions eg. stack copy instead of reference for parameters
    using FormatterFunc = void(const std::string& username,
                               const std::string& password, Buffer& dest);

    CredentialsProvider(std::string&& user, std::string&& password) :
        credentials(std::move(user), std::move(password))
    {
    }

    void escapeCommas()
    {
        credentials.escapeCommas();
    }

    const std::string& user()
    {
        return credentials.user();
    }

    const std::string& password()
    {
        return credentials.password();
    }

    SecureBuffer pack(const FormatterFunc formatter)
    {
        SecureBuffer packed{new Buffer{}};
        if (formatter)
        {
            formatter(credentials.user(), credentials.password(), *packed);
        }
        return packed;
    }

  private:
    Credentials credentials;
};

// Wrapper for boost::async_pipe ensuring proper pipe cleanup
template <typename Buffer>
class NamedPipe
{
  public:
    using unix_fd = sdbusplus::message::unix_fd;

    NamedPipe(boost::asio::io_context& io, const std::string name,
              Buffer&& buffer) :
        name(name),
        impl(io, name), buffer{std::move(buffer)}
    {
    }

    ~NamedPipe()
    {
        // Named pipe needs to be explicitly removed
        impl.close();
        ::unlink(name.c_str());
    }

    unix_fd fd()
    {
        return unix_fd{impl.native_sink()};
    }

    const std::string& file() const
    {
        return name;
    }

    template <typename WriteHandler>
    void async_write(WriteHandler&& handler)
    {
        impl.async_write_some(data(), std::forward<WriteHandler>(handler));
    }

  private:
    // Specialization for pointer types
    template <typename B = Buffer>
    typename std::enable_if<boost::has_dereference<B>::value,
                            boost::asio::const_buffer>::type
        data()
    {
        return boost::asio::buffer(*buffer);
    }

    template <typename B = Buffer>
    typename std::enable_if<!boost::has_dereference<B>::value,
                            boost::asio::const_buffer>::type
        data()
    {
        return boost::asio::buffer(buffer);
    }

    const std::string name;
    boost::process::async_pipe impl;
    Buffer buffer;
};

class VolatileFile
{
    using Buffer = CredentialsProvider::SecureBuffer;

  public:
    VolatileFile(Buffer&& contents) : size(contents->size())
    {
        auto data = std::move(contents);
        filePath = fs::temp_directory_path().c_str();
        filePath.append("/VM-XXXXXX");
        create(data);
    }

    ~VolatileFile()
    {
        purgeFileContents();
        fs::remove(filePath);
    }

    const std::string& path()
    {
        return filePath;
    }

    class FileObject
    {
      public:
        explicit FileObject(int fd) : fd(fd){};
        FileObject() = delete;
        FileObject(const FileObject&) = delete;
        FileObject& operator=(const FileObject&) = delete;

        ssize_t write(void* data, size_t nw)
        {
            return ::write(fd, data, nw);
        }

        ~FileObject()
        {
            close(fd);
        }

      private:
        int fd;
    };

  private:
    void create(const Buffer& data)
    {
        auto fd = mkstemp(filePath.data());

        FileObject file(fd);
        if (file.write(data->data(), data->size()) !=
            static_cast<ssize_t>(data->size()))
        {
            throw sdbusplus::exception::SdBusError(
                EIO, "I/O error on temporary file");
        }
    }

    void purgeFileContents()
    {
        if (std::ofstream file(filePath); file)
        {
            std::array<char, secretLimit> buf;
            buf.fill('*');

            std::size_t bytesWritten = 0;
            while (bytesWritten < size)
            {
                std::size_t bytesToWrite =
                    std::min(secretLimit, (size - bytesWritten));
                file.write(buf.data(),
                           static_cast<std::streamsize>(bytesToWrite));
                bytesWritten += bytesToWrite;
            }
        }
    }

    std::string filePath;
    const std::size_t size;
};

class SignalSender
{
  public:
    SignalSender(std::shared_ptr<sdbusplus::asio::connection> con,
                 const std::string& obj, const std::string& iface,
                 const std::string& name) :
        con(con),
        interface(iface), object(obj), name(name){};

    SignalSender() = delete;
    SignalSender(const SignalSender&) = delete;

    void send(std::optional<const std::error_code> status)
    {
        auto msgSignal =
            con->new_signal(object.c_str(), interface.c_str(), name.c_str());

        msgSignal.append(status.value_or(std::error_code{}).value());
        LogMsg(Logger::Debug, "Sending signal: Object: ", object,
               ", Interface: ", interface, ", Name: ", name,
               "Status: ", status.value_or(std::error_code{}).value());
        msgSignal.signal_send();
    }

  private:
    std::shared_ptr<sdbusplus::asio::connection> con;
    std::string interface;
    std::string object;
    std::string name;
};

class NotificationWrapper
{
  public:
    NotificationWrapper(std::unique_ptr<SignalSender> signal,
                        std::unique_ptr<boost::asio::steady_timer> timer) :
        signal(std::move(signal)),
        timer(std::move(timer))
    {
    }

    void start(std::function<void(const boost::system::error_code&)>&& handler,
               const std::chrono::seconds& duration)
    {
        LogMsg(Logger::Debug, "Notification initiated");
        started = true;
        timer->expires_from_now(duration);
        timer->async_wait([this, handler{std::move(handler)}](
                              const boost::system::error_code& ec) {
            started = false;
            handler(ec);
        });
    }

    void notify(const std::error_code& ec)
    {
        if (started)
        {
            timer->cancel();
            if (ec)
                signal->send((ec));
            else
                signal->send(std::nullopt);
            started = false;
            return;
        }
        LogMsg(Logger::Debug, "Notification(ec) supressed (not started)");
    }

  private:
    std::unique_ptr<SignalSender> signal;
    std::unique_ptr<boost::asio::steady_timer> timer;
    bool started{false};
};

} // namespace utils
