#include "ServiceRecord.h"
#include "dns_sd.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <iostream>

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

int main() {
    using namespace nearcast::android;
    TXTRecordRef txt{};
    TXTRecordCreate(&txt, 0, nullptr);
    check(TXTRecordSetValue(&txt, "features", 5, "0xabc") == 0, "TXT feature must be accepted");
    check(TXTRecordSetValue(&txt, "pw", 5, "false") == 0, "TXT password hint must be accepted");
    check(TXTRecordGetLength(&txt) == 24, "DNS-SD TXT must use wire-format character strings");
    std::vector<ServiceRecord> records;
    CaptureDnsSdRecords capture(records);
    DNSServiceRef ref{};
    check(DNSServiceRegister(&ref, 0, 0, "Demo", "_airplay._tcp", "local.", nullptr, htons(7012),
        TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), nullptr, nullptr) == 0,
        "UxPlay registration request must be captured");
    TXTRecordDeallocate(&txt);
    check(records.size() == 1 && records[0].type == "_airplay._tcp" && records[0].port == 7012 &&
        records[0].name == "Demo" && records[0].txt == std::vector<std::string>{"features=0xabc", "pw=false"},
        "Android NSD must receive its listener port and owned UxPlay TXT data");
    DNSServiceRefDeallocate(ref);
}
