// secrets_backend_linux.cpp — Linux libsecret implementation
#include "secrets_backend.h"

#include <secret/secret.h>
#include <string>
#include <vector>
#include <mutex>
#include <glib.h>

namespace secrets_backend {

static const char* kSchemaName = "org.llamaboss.Secrets";
static const char* kKeyAttr = "key";
static std::mutex g_mutex;

static SecretSchema* GetSchema() {
    static SecretSchema schema = {
        kSchemaName,
        SECRET_SCHEMA_NONE,
        {
            {kKeyAttr, SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}
        }
    };
    return &schema;
}

bool Initialize() {
    // libsecret is available if gnome-keyring or kwallet is running
    return true;
}

bool Set(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lk(g_mutex);

    GError* error = nullptr;
    gboolean result = secret_password_store_sync(
        GetSchema(),
        SECRET_COLLECTION_DEFAULT,
        std::string("LlamaBoss: " + std::string(key)).c_str(),
        std::string(value).c_str(),
        nullptr, &error,
        kKeyAttr, std::string(key).c_str(),
        nullptr
    );

    if (!result && error) {
        g_error_free(error);
        return false;
    }
    return true;
}

std::optional<std::string> Get(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);

    GError* error = nullptr;
    gchar* value = secret_password_lookup_sync(
        GetSchema(),
        nullptr, &error,
        kKeyAttr, std::string(key).c_str(),
        nullptr
    );

    if (!value) {
        if (error) g_error_free(error);
        return std::nullopt;
    }

    std::string result(value);
    g_free(value);
    return result;
}

bool Delete(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);

    GError* error = nullptr;
    gboolean result = secret_password_clear_sync(
        GetSchema(),
        nullptr, &error,
        kKeyAttr, std::string(key).c_str(),
        nullptr
    );

    if (!result && error) {
        g_error_free(error);
        return false;
    }
    return true;
}

std::vector<std::string> ListKeys() {
    std::lock_guard<std::mutex> lk(g_mutex);

    GError* error = nullptr;
    GList* results = secret_service_search_sync(
        secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error),
        GetSchema(),
        nullptr, &error,
        nullptr
    );

    std::vector<std::string> keys;
    if (!results) return keys;

    for (GList* l = results; l; l = l->next) {
        SecretItem* item = static_cast<SecretItem*>(l->data);
        GVariant* attributes = secret_item_get_attributes(item);
        if (attributes) {
            GVariant* keyVariant = g_variant_lookup_value(attributes, kKeyAttr, G_VARIANT_TYPE_STRING);
            if (keyVariant) {
                const gchar* keyStr = g_variant_get_string(keyVariant, nullptr);
                if (keyStr) keys.push_back(keyStr);
                g_variant_unref(keyVariant);
            }
            g_variant_unref(attributes);
        }
        g_object_unref(item);
    }
    g_list_free(results);
    return keys;
}

} // namespace secrets_backend