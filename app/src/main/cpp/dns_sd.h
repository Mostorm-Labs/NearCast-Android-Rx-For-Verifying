#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The subset of the DNS-SD ABI used by the pinned UxPlay core.
typedef struct _DNSServiceRef_t* DNSServiceRef;
typedef uint32_t DNSServiceFlags;
typedef int32_t DNSServiceErrorType;
typedef struct { void* data; } TXTRecordRef;
typedef void (*DNSServiceRegisterReply)(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType,
    const char*, const char*, const char*, void*);

enum {
    kDNSServiceErr_NoError = 0,
    kDNSServiceErr_BadParam = -65540,
    kDNSServiceErr_NoMemory = -65539,
    kDNSServiceErr_NameConflict = -65548,
};

DNSServiceErrorType DNSServiceRegister(DNSServiceRef*, DNSServiceFlags, uint32_t,
    const char*, const char*, const char*, const char*, uint16_t, uint16_t, const void*,
    DNSServiceRegisterReply, void*);
void DNSServiceRefDeallocate(DNSServiceRef);
int DNSServiceRefSockFD(DNSServiceRef);
DNSServiceErrorType DNSServiceProcessResult(DNSServiceRef);
void TXTRecordCreate(TXTRecordRef*, uint16_t, void*);
void TXTRecordDeallocate(TXTRecordRef*);
DNSServiceErrorType TXTRecordSetValue(TXTRecordRef*, const char*, uint8_t, const void*);
uint16_t TXTRecordGetLength(const TXTRecordRef*);
const void* TXTRecordGetBytesPtr(const TXTRecordRef*);

#ifdef __cplusplus
}
#endif
