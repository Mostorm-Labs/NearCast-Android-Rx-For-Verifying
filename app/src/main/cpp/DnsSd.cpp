#include "ServiceRecord.h"
#include "dns_sd.h"

#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <new>

using nearcast::android::CaptureDnsSdRecords;
using nearcast::android::ServiceRecord;
namespace {
using Bytes = std::vector<std::uint8_t>;
thread_local std::vector<ServiceRecord>* activeCapture = nullptr;
}

CaptureDnsSdRecords::CaptureDnsSdRecords(std::vector<ServiceRecord>& records)
    : previous_(activeCapture) {
    activeCapture = &records;
}

CaptureDnsSdRecords::~CaptureDnsSdRecords() {
    activeCapture = previous_;
}

struct _DNSServiceRef_t {};

DNSServiceErrorType DNSServiceRegister(DNSServiceRef* ref, DNSServiceFlags, uint32_t,
    const char* name, const char* type, const char*, const char*, uint16_t port, uint16_t length, const void* data,
    DNSServiceRegisterReply, void*) {
    if (!ref || !activeCapture || !name || !type || !port || (length && !data)) {
        return kDNSServiceErr_BadParam;
    }
    *ref = nullptr;
    try {
        ServiceRecord record{type, name, ntohs(port), {}};
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < length;) {
            const auto size = bytes[i++];
            if (!size || i + size > length) return kDNSServiceErr_BadParam;
            record.txt.emplace_back(reinterpret_cast<const char*>(bytes + i), size);
            i += size;
        }
        auto registration = std::make_unique<_DNSServiceRef_t>();
        activeCapture->push_back(std::move(record));
        *ref = registration.release();
        return kDNSServiceErr_NoError;
    } catch (...) {
        return kDNSServiceErr_NoMemory;
    }
}

void DNSServiceRefDeallocate(DNSServiceRef ref) { delete ref; }
int DNSServiceRefSockFD(DNSServiceRef) { return -1; }
DNSServiceErrorType DNSServiceProcessResult(DNSServiceRef) { return kDNSServiceErr_BadParam; }

void TXTRecordCreate(TXTRecordRef* record, uint16_t, void*) {
    record->data = new (std::nothrow) Bytes;
}

void TXTRecordDeallocate(TXTRecordRef* record) {
    delete static_cast<Bytes*>(record->data);
    record->data = nullptr;
}

DNSServiceErrorType TXTRecordSetValue(TXTRecordRef* record, const char* key, uint8_t size, const void* value) {
    auto* bytes = static_cast<Bytes*>(record->data);
    if (!bytes) return kDNSServiceErr_NoMemory;
    if (!key || !*key || std::strchr(key, '=') || (size && !value)) return kDNSServiceErr_BadParam;
    const auto keyLength = std::strlen(key);
    const auto length = keyLength + (value ? 1 + size : 0);
    if (length > 255 || bytes->size() + length + 1 > UINT16_MAX) return kDNSServiceErr_BadParam;
    try {
        bytes->reserve(bytes->size() + length + 1);
        bytes->push_back(static_cast<std::uint8_t>(length));
        bytes->insert(bytes->end(), key, key + keyLength);
        if (value) {
            bytes->push_back('=');
            const auto* data = static_cast<const std::uint8_t*>(value);
            bytes->insert(bytes->end(), data, data + size);
        }
        return kDNSServiceErr_NoError;
    } catch (...) {
        return kDNSServiceErr_NoMemory;
    }
}

uint16_t TXTRecordGetLength(const TXTRecordRef* record) {
    const auto* bytes = static_cast<const Bytes*>(record->data);
    return bytes ? static_cast<uint16_t>(bytes->size()) : 0;
}

const void* TXTRecordGetBytesPtr(const TXTRecordRef* record) {
    const auto* bytes = static_cast<const Bytes*>(record->data);
    return bytes ? bytes->data() : nullptr;
}
