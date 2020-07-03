#pragma once

#include "basic_state.hpp"

struct ActivatingState : public BasicStateT<ActivatingState>
{
    static std::string_view stateName()
    {
        return "ActivatingState";
    }

    ActivatingState(interfaces::MountPointStateMachine& machine);

    std::unique_ptr<BasicState> onEnter() override;

    std::unique_ptr<BasicState> handleEvent(UdevStateChangeEvent event);
    std::unique_ptr<BasicState> handleEvent(SubprocessStoppedEvent event);

    template <class AnyEvent>
    std::unique_ptr<BasicState> handleEvent(AnyEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        return nullptr;
    }

  private:
    std::unique_ptr<BasicState> activateProxyMode();
    std::unique_ptr<BasicState> activateLegacyMode();
    std::unique_ptr<BasicState> mountSmbShare();
    std::unique_ptr<BasicState> mountHttpsShare();

    static std::unique_ptr<resource::Process>
        spawnNbdKit(interfaces::MountPointStateMachine& machine,
                    std::unique_ptr<utils::VolatileFile>&& secret,
                    const std::vector<std::string>& params);
    static std::unique_ptr<resource::Process>
        spawnNbdKit(interfaces::MountPointStateMachine& machine,
                    const fs::path& file);
    static std::unique_ptr<resource::Process>
        spawnNbdKit(interfaces::MountPointStateMachine& machine,
                    const std::string& url);

    static bool checkUrl(const std::string& urlScheme,
                         const std::string& imageUrl);
    static bool getImagePathFromUrl(const std::string& urlScheme,
                                    const std::string& imageUrl,
                                    std::string* imagePath);
    static bool isHttpsUrl(const std::string& imageUrl);
    static bool getImagePathFromHttpsUrl(const std::string& imageUrl,
                                         std::string* imagePath);

    static bool isCifsUrl(const std::string& imageUrl);
    static bool getImagePathFromCifsUrl(const std::string& imageUrl,
                                        std::string* imagePath);
    static fs::path getImagePath(const std::string& imageUrl);

    std::unique_ptr<resource::Process> process;
    std::unique_ptr<resource::Gadget> gadget;
};
