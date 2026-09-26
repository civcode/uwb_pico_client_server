#include "uwb/client/notification_queue.hpp"

namespace uwb::client {

const char *notificationKindName(NotificationKind kind) noexcept {
    switch (kind) {
    case NotificationKind::DeviceSeen:
        return "device_seen";
    case NotificationKind::DeviceStateChanged:
        return "device_state";
    case NotificationKind::DeviceError:
        return "device_error";
    case NotificationKind::Measurement:
        return "measurement";
    case NotificationKind::LocalPosition:
        return "local_position";
    case NotificationKind::StreamStateChanged:
        return "stream_state";
    case NotificationKind::DeviceRemoved:
        return "device_removed";
    case NotificationKind::CommandFinished:
        return "command_finished";
    }
    return "unknown";
}

} // namespace uwb::client
