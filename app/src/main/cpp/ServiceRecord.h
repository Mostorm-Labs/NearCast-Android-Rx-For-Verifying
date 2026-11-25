#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nearcast::android {

struct ServiceRecord {
    std::string type;
    std::string name;
    std::uint16_t port;
    std::vector<std::string> txt;
};

// Captures UxPlay's registration requests on the thread starting the service.
// Android NSD owns the asynchronous registrations and their status.
class CaptureDnsSdRecords {
public:
    explicit CaptureDnsSdRecords(std::vector<ServiceRecord>& records);
    ~CaptureDnsSdRecords();
    CaptureDnsSdRecords(const CaptureDnsSdRecords&) = delete;
    CaptureDnsSdRecords& operator=(const CaptureDnsSdRecords&) = delete;

private:
    std::vector<ServiceRecord>* previous_;
};

} // namespace nearcast::android
