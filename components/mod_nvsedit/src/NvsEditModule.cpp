/**
 * \file
 * \brief GUI module for browsing and deleting NVS entries.
 */

#include "mod_nvsedit/NvsEditModule.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/feature_flags.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ContextMenuView.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_views/InfoView.h"
#include "cdc_views/ToastView.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <cstring>
#include <cstdio>

using namespace cdc::ui;
using namespace cdc::core;

static const char* TAG = "NvsEdit";

namespace cdc::mod_nvsedit {

/** \brief Returns the NVS editor entry view callback target. */
static IView* getNvsEditorView();

/** \brief Rebuilds key list labels into s_keyLabels and refreshes the key list view. */
static void rebuildKeyListView();

/** \brief Upper bounds for list sizes and value preview buffers. */

static constexpr uint8_t MAX_NAMESPACES = 32;
static constexpr uint8_t MAX_KEYS = 48;
static constexpr size_t MAX_VALUE_DISPLAY = 256;

/** \brief Runtime state for namespace/key browsing context. */

static char s_namespaces[MAX_NAMESPACES][16] = {};
static uint8_t s_namespaceCount = 0;
static char s_selectedNamespace[16] = {};

static char s_keys[MAX_KEYS][16] = {};
static nvs_type_t s_keyTypes[MAX_KEYS] = {};
static uint8_t s_keyCount = 0;
static char s_selectedKey[16] = {};

/** \brief Lazily created views and backing list item arrays. */
static ListView* s_namespaceListView = nullptr;
static ListView* s_keyListView = nullptr;
static InfoView* s_valueView = nullptr;
static ListItem s_namespaceItems[MAX_NAMESPACES];
static ListItem s_keyItems[MAX_KEYS];

/**
 * \brief Indicates whether destructive delete actions are enabled.
 * \return `true` when feature flag permits deletes, otherwise `false`.
 */
static bool deleteEnabled() {
    return FEATURE_NVS_EDIT != 0;
}

/**
 * \brief Shows a toast describing that delete actions are disabled.
 */
static void showDeleteDisabled() {
    showToastError("Delete disabled");
}

/**
 * \brief Converts NVS value type enum to a short display string.
 * \param type NVS entry type.
 * \return String representation of the type.
 */
static const char* nvsTypeToString(nvs_type_t type) {
    switch (type) {
        case NVS_TYPE_U8:   return "u8";
        case NVS_TYPE_I8:   return "i8";
        case NVS_TYPE_U16:  return "u16";
        case NVS_TYPE_I16:  return "i16";
        case NVS_TYPE_U32:  return "u32";
        case NVS_TYPE_I32:  return "i32";
        case NVS_TYPE_U64:  return "u64";
        case NVS_TYPE_I64:  return "i64";
        case NVS_TYPE_STR:  return "str";
        case NVS_TYPE_BLOB: return "blob";
        default:            return "?";
    }
}

/**
 * \brief Loads unique namespace names from the default NVS partition.
 */
static void loadNamespaces() {
    s_namespaceCount = 0;
    memset(s_namespaces, 0, sizeof(s_namespaces));

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);

    char lastNs[16] = {};
    while (err == ESP_OK && s_namespaceCount < MAX_NAMESPACES) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        // Add namespace if not already in list
        if (strcmp(info.namespace_name, lastNs) != 0) {
            strncpy(s_namespaces[s_namespaceCount], info.namespace_name, 15);
            s_namespaces[s_namespaceCount][15] = '\0';
            strncpy(lastNs, info.namespace_name, 15);
            s_namespaceCount++;
        }

        err = nvs_entry_next(&it);
    }

    if (it) nvs_release_iterator(it);
    LOG_I(TAG, "Found %d namespaces", s_namespaceCount);
}

/**
 * \brief Loads keys and types for one namespace.
 * \param ns Namespace name.
 */
static void loadKeys(const char* ns) {
    s_keyCount = 0;
    memset(s_keys, 0, sizeof(s_keys));

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);

    while (err == ESP_OK && s_keyCount < MAX_KEYS) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        strncpy(s_keys[s_keyCount], info.key, 15);
        s_keys[s_keyCount][15] = '\0';
        s_keyTypes[s_keyCount] = info.type;
        s_keyCount++;

        err = nvs_entry_next(&it);
    }

    if (it) nvs_release_iterator(it);
    LOG_I(TAG, "Found %d keys in '%s'", s_keyCount, ns);
}

/**
 * \brief Deletes a single key from a namespace.
 * \param ns Namespace name.
 * \param key Key name.
 * \return `true` on success, otherwise `false`.
 */
static bool deleteKey(const char* ns, const char* key) {
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);

    LOG_I(TAG, "Deleted key '%s' from '%s': %s", key, ns, esp_err_to_name(err));
    return err == ESP_OK;
}

/**
 * \brief Deletes all keys in a namespace.
 * \param ns Namespace name.
 * \return `true` on success, otherwise `false`.
 */
static bool deleteNamespace(const char* ns) {
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);

    LOG_I(TAG, "Deleted namespace '%s': %s", ns, esp_err_to_name(err));
    return err == ESP_OK;
}

/**
 * \brief Formats one NVS value into a readable preview string.
 * \param ns Namespace name.
 * \param key Key name.
 * \param type Stored NVS type.
 * \param buf Destination text buffer.
 * \param bufLen Size of `buf` in bytes.
 */
static void formatValue(const char* ns, const char* key, nvs_type_t type, char* buf, size_t bufLen) {
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) {
        snprintf(buf, bufLen, "(error opening)");
        return;
    }

    esp_err_t err = ESP_FAIL;

    switch (type) {
        case NVS_TYPE_U8: {
            uint8_t val;
            err = nvs_get_u8(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%u (0x%02X)", val, val);
            break;
        }
        case NVS_TYPE_I8: {
            int8_t val;
            err = nvs_get_i8(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%d", val);
            break;
        }
        case NVS_TYPE_U16: {
            uint16_t val;
            err = nvs_get_u16(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%u (0x%04X)", val, val);
            break;
        }
        case NVS_TYPE_I16: {
            int16_t val;
            err = nvs_get_i16(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%d", val);
            break;
        }
        case NVS_TYPE_U32: {
            uint32_t val;
            err = nvs_get_u32(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%lu (0x%08lX)", (unsigned long)val, (unsigned long)val);
            break;
        }
        case NVS_TYPE_I32: {
            int32_t val;
            err = nvs_get_i32(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%ld", (long)val);
            break;
        }
        case NVS_TYPE_U64: {
            uint64_t val;
            err = nvs_get_u64(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%llu", (unsigned long long)val);
            break;
        }
        case NVS_TYPE_I64: {
            int64_t val;
            err = nvs_get_i64(handle, key, &val);
            if (err == ESP_OK) snprintf(buf, bufLen, "%lld", (long long)val);
            break;
        }
        case NVS_TYPE_STR: {
            size_t len = 0;
            err = nvs_get_str(handle, key, nullptr, &len);
            if (err == ESP_OK && len > 0 && len < bufLen) {
                err = nvs_get_str(handle, key, buf, &len);
            } else if (err == ESP_OK) {
                snprintf(buf, bufLen, "(str len=%zu)", len);
            }
            break;
        }
        case NVS_TYPE_BLOB: {
            size_t len = 0;
            err = nvs_get_blob(handle, key, nullptr, &len);
            if (err == ESP_OK) {
                if (len <= 32) {
                    uint8_t data[32];
                    err = nvs_get_blob(handle, key, data, &len);
                    if (err == ESP_OK) {
                        char* p = buf;
                        for (size_t i = 0; i < len && (p - buf) < (int)(bufLen - 4); i++) {
                            p += snprintf(p, bufLen - (p - buf), "%02X ", data[i]);
                        }
                    }
                } else {
                    snprintf(buf, bufLen, "(blob %zu bytes)", len);
                }
            }
            break;
        }
        default:
            snprintf(buf, bufLen, "(unknown type)");
            break;
    }

    if (err != ESP_OK) {
        snprintf(buf, bufLen, "(read error)");
    }

    nvs_close(handle);
}

/**
 * \brief Deletes the currently selected namespace via context menu action.
 */
static void onDeleteNamespace() {
    if (!deleteEnabled()) {
        showDeleteDisabled();
        hideContextMenu();
        return;
    }
    if (s_selectedNamespace[0] == '\0') return;

    if (deleteNamespace(s_selectedNamespace)) {
        showToastInfo("Deleted");
        loadNamespaces();
        // Update list
        for (uint8_t i = 0; i < s_namespaceCount; i++) {
            s_namespaceItems[i] = {s_namespaces[i], 0, false, nullptr};
        }
        if (s_namespaceListView) {
            s_namespaceListView->init("NVS Namespaces", s_namespaceItems, s_namespaceCount);
        }
    } else {
        showToastError("Delete failed");
    }
    hideContextMenu();
}

/**
 * \brief Deletes the currently selected key via context menu action.
 */
static void onDeleteKey() {
    if (!deleteEnabled()) {
        showDeleteDisabled();
        hideContextMenu();
        return;
    }
    if (s_selectedNamespace[0] == '\0' || s_selectedKey[0] == '\0') return;

    if (deleteKey(s_selectedNamespace, s_selectedKey)) {
        showToastInfo("Deleted");
        loadKeys(s_selectedNamespace);
        rebuildKeyListView();
        // Go back if no more keys
        if (s_keyCount == 0) {
            ViewStack::instance().pop();
        }
    } else {
        showToastError("Delete failed");
    }
    hideContextMenu();
}

static ContextMenuItem s_nsContextItems[] = {
    {"Delete NS", onDeleteNamespace}
};

static ContextMenuItem s_keyContextItems[] = {
    {"Delete Key", onDeleteKey}
};

/** \brief Shows key list view for the selected namespace. */
static void showKeyListView(const char* ns);
/** \brief Shows detailed value view for a selected key. */
static void showValueView(const char* ns, const char* key, nvs_type_t type);

/**
 * \brief Handles namespace list selection.
 * \param index Selected namespace index.
 * \param userData Optional callback user data.
 */
static void onNamespaceSelect(uint16_t index, void* userData) {
    (void)userData;
    if (index >= s_namespaceCount) return;

    strncpy(s_selectedNamespace, s_namespaces[index], sizeof(s_selectedNamespace) - 1);
    showKeyListView(s_selectedNamespace);
}

/**
 * \brief Opens namespace context menu for selected item.
 * \param index Selected namespace index.
 * \param userData Optional callback user data.
 */
static void onNamespaceMenu(uint16_t index, void* userData) {
    (void)userData;
    if (index < s_namespaceCount) {
        strncpy(s_selectedNamespace, s_namespaces[index], sizeof(s_selectedNamespace) - 1);
        if (!deleteEnabled()) {
            showDeleteDisabled();
            return;
        }
        showContextMenu("Namespace", s_nsContextItems, 1);
    }
}

/**
 * \brief Creates and shows the namespace list view.
 */
static void showNamespaceListView() {
    loadNamespaces();

    if (!s_namespaceListView) {
        s_namespaceListView = new ListView();
        s_namespaceListView->setOnSelect(onNamespaceSelect);
        s_namespaceListView->setOnMenu(onNamespaceMenu);
    }

    for (uint8_t i = 0; i < s_namespaceCount; i++) {
        s_namespaceItems[i] = {s_namespaces[i], 0, false, nullptr};
    }

    s_namespaceListView->init("NVS Namespaces", s_namespaceItems, s_namespaceCount);
    ViewStack::instance().push(s_namespaceListView);
}

/** \brief Persistent key label storage used by list items. */
EXT_RAM_BSS_ATTR static char s_keyLabels[MAX_KEYS][24];

/**
 * \brief Handles key selection and opens detailed value view.
 * \param index Selected key index.
 * \param userData Optional callback user data.
 */
static void onKeySelect(uint16_t index, void* userData) {
    (void)userData;
    if (index >= s_keyCount) return;

    strncpy(s_selectedKey, s_keys[index], sizeof(s_selectedKey) - 1);
    showValueView(s_selectedNamespace, s_selectedKey, s_keyTypes[index]);
}

/**
 * \brief Opens key context menu for the selected key.
 * \param index Selected key index.
 * \param userData Optional callback user data.
 */
static void onKeyMenu(uint16_t index, void* userData) {
    (void)userData;
    if (index < s_keyCount) {
        strncpy(s_selectedKey, s_keys[index], sizeof(s_selectedKey) - 1);
        if (!deleteEnabled()) {
            showDeleteDisabled();
            return;
        }
        showContextMenu("Key", s_keyContextItems, 1);
    }
}

/**
 * \brief Rebuilds key list labels and refreshes the key list view.
 */
static void rebuildKeyListView() {
    for (uint8_t i = 0; i < s_keyCount; i++) {
        snprintf(s_keyLabels[i], sizeof(s_keyLabels[i]), "%s [%s]",
                 s_keys[i], nvsTypeToString(s_keyTypes[i]));
        s_keyItems[i] = {s_keyLabels[i], 0, false, nullptr};
    }
    if (s_keyListView) {
        s_keyListView->init(s_selectedNamespace, s_keyItems, s_keyCount);
    }
}

/**
 * \brief Creates and shows key list view for a namespace.
 * \param ns Namespace name.
 */
static void showKeyListView(const char* ns) {
    loadKeys(ns);

    if (!s_keyListView) {
        s_keyListView = new ListView();
        s_keyListView->setOnSelect(onKeySelect);
        s_keyListView->setOnMenu(onKeyMenu);
    }

    rebuildKeyListView();
    ViewStack::instance().push(s_keyListView);
}

static char s_valueBuffer[MAX_VALUE_DISPLAY];
static char s_valueTitle[32];

/**
 * \brief Shows formatted value preview for one namespace key.
 * \param ns Namespace name.
 * \param key Key name.
 * \param type NVS value type.
 */
static void showValueView(const char* ns, const char* key, nvs_type_t type) {
    formatValue(ns, key, type, s_valueBuffer, sizeof(s_valueBuffer));

    snprintf(s_valueTitle, sizeof(s_valueTitle), "%s [%s]", key, nvsTypeToString(type));

    if (!s_valueView) {
        s_valueView = new InfoView();
    }

    s_valueView->init(s_valueTitle, s_valueBuffer);
    ViewStack::instance().push(s_valueView);
}

/**
 * \brief Confirm callback that opens namespace browser after warning dialog.
 * \param userData Optional callback user data.
 */
static void onNvsEditorConfirm(void* userData) {
    (void)userData;
    showNamespaceListView();
    if (!deleteEnabled()) {
        showToastInfo("Read-only mode");
    }
}

/**
 * \brief Opens privileged confirmation dialog before entering NVS editor view.
 * \return Always `nullptr` because the view is pushed asynchronously.
 */
static IView* getNvsEditorView() {
    const char* msg = deleteEnabled()
        ? "NVS Editor is privileged. Deletes are irreversible. Continue?"
        : "NVS Browser is read-only. Continue?";
    showConfirm(msg, onNvsEditorConfirm, nullptr, ConfirmView::Icon::WARNING, nullptr);
    return nullptr;  // We pushed view ourselves
}

/**
 * \brief Initializes NVS editor module state.
 * \return `true` on success.
 */
bool NvsEditModule::init() {
    state_ = ServiceState::INITIALIZED;
    LOG_I(TAG, "NVS Editor module initialized");
    return true;
}

/**
 * \brief Exposes NVS editor entry in the tools menu.
 * \param items Destination array for menu items.
 * \param maxItems Capacity of `items`.
 * \return Number of populated menu entries.
 */
uint8_t NvsEditModule::getMenuItems(ModuleMenuItem* items, uint8_t maxItems) {
    if (maxItems < 1) return 0;

    items[0].label = deleteEnabled() ? "NVS Editor" : "NVS Browser";
    items[0].priority = 50;
    items[0].getView = getNvsEditorView;
    items[0].isVisible = nullptr;
    items[0].moduleName = getName();
    items[0].location = MenuLocation::TOOLS_MENU;
    items[0].onSelect = nullptr;

    return 1;
}

/** \brief Static module instance used by registration callback. */
static NvsEditModule s_module;

} // namespace cdc::mod_nvsedit

/**
 * \brief Registers NVS editor initializer with module registry.
 */
extern "C" void mod_nvsedit_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_nvsedit::s_module;
        module.init();
    });
}
