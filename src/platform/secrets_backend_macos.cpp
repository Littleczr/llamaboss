// secrets_backend_macos.cpp — macOS Keychain implementation
#include "secrets_backend.h"

#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string>
#include <vector>
#include <mutex>

namespace secrets_backend {

static const char* kServiceName = "LlamaBoss";
static std::mutex g_mutex;

static CFStringRef ToCFString(const std::string& s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

static std::string FromCFString(CFStringRef cf) {
    if (!cf) return "";
    CFIndex len = CFStringGetLength(cf);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8);
    std::string result(maxSize, '\0');
    CFStringGetCString(cf, &result[0], maxSize + 1, kCFStringEncodingUTF8);
    result.resize(strlen(result.c_str()));
    return result;
}

bool Initialize() {
    // Keychain is always available on macOS
    return true;
}

bool Set(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lk(g_mutex);

    CFStringRef service = ToCFString(kServiceName);
    CFStringRef account = ToCFString(std::string(key));
    CFStringRef data = ToCFString(std::string(value));

    // Delete existing item first
    CFMutableDictionaryRef deleteQuery = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(deleteQuery, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(deleteQuery, kSecAttrService, service);
    CFDictionaryAddValue(deleteQuery, kSecAttrAccount, account);
    SecItemDelete(deleteQuery);
    CFRelease(deleteQuery);

    // Add new item
    CFMutableDictionaryRef addQuery = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(addQuery, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(addQuery, kSecAttrService, service);
    CFDictionaryAddValue(addQuery, kSecAttrAccount, account);
    CFDictionaryAddValue(addQuery, kSecValueData, data);
    CFDictionaryAddValue(addQuery, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);

    OSStatus status = SecItemAdd(addQuery, nullptr);
    CFRelease(addQuery);
    CFRelease(service);
    CFRelease(account);
    CFRelease(data);

    return status == errSecSuccess;
}

std::optional<std::string> Get(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);

    CFStringRef service = ToCFString(kServiceName);
    CFStringRef account = ToCFString(std::string(key));

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, service);
    CFDictionaryAddValue(query, kSecAttrAccount, account);
    CFDictionaryAddValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);

    if (status != errSecSuccess) {
        if (result) CFRelease(result);
        return std::nullopt;
    }

    CFDataRef data = static_cast<CFDataRef>(result);
    CFIndex len = CFDataGetLength(data);
    const UInt8* bytes = CFDataGetBytePtr(data);
    std::string value(reinterpret_cast<const char*>(bytes), len);
    CFRelease(data);

    return value;
}

bool Delete(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);

    CFStringRef service = ToCFString(kServiceName);
    CFStringRef account = ToCFString(std::string(key));

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, service);
    CFDictionaryAddValue(query, kSecAttrAccount, account);

    OSStatus status = SecItemDelete(query);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);

    return status == errSecSuccess || status == errSecItemNotFound;
}

std::vector<std::string> ListKeys() {
    std::lock_guard<std::mutex> lk(g_mutex);

    CFStringRef service = ToCFString(kServiceName);

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, service);
    CFDictionaryAddValue(query, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitAll);

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(service);

    std::vector<std::string> keys;
    if (status != errSecSuccess) return keys;

    CFArrayRef array = static_cast<CFArrayRef>(result);
    CFIndex count = CFArrayGetCount(array);
    for (CFIndex i = 0; i < count; ++i) {
        CFDictionaryRef item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(array, i));
        CFStringRef account = static_cast<CFStringRef>(CFDictionaryGetValue(item, kSecAttrAccount));
        if (account) {
            keys.push_back(FromCFString(account));
        }
    }
    CFRelease(array);
    return keys;
}

} // namespace secrets_backend