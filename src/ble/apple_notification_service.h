#pragma once
#include <string>
#include <functional>
#include <vector>
#include <Arduino.h>

class BLEClient;

// Apple Notification Center Service (ANCS), over the same connection as AMS.
// Spec: https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/
namespace AppleNotificationService
{
    struct NotificationInfo
    {
        uint8_t     categoryId = 0;
        uint32_t    uid = 0;
        std::string appId;   // e.g. "com.apple.MobileSMS"
        std::string title;   // sender / contact
        std::string message; // body
    };

    using NotificationCb = std::function<void(const NotificationInfo &)>;

    void RegisterForNotifications(const NotificationCb &callback);
    // Recent notifications, newest first; removed on iOS Removed events. Capped.
    std::vector<NotificationInfo> GetRecent();
    const char *CategoryName(uint8_t categoryId);
    // Friendly source name from a bundle id (e.g. "ph.telegra.Telegraph" -> "Telegram").
    std::string AppName(const std::string &appId);

    bool StartNotificationService(BLEClient *client);

    // Main loop only: does the deferred Control Point write. Writing inside the
    // notify callback deadlocks the NimBLE host.
    void Process();

    // Drop cached Control Point and pending fetches; see AppleMediaService::Detach().
    void Detach();
}
