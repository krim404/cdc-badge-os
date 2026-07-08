/**
 * \file PluginManifest.h
 * \brief In-memory representation of a plugin's meta.json.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace cdc::plugin_manager {

struct PrerequisiteSpec {
    std::string name;
    std::map<std::string, std::string> params;
    std::string on_fail;
};

struct PluginCapabilities {
    bool wifi = false;
    bool ble = false;
    bool http = false;
    bool socket = false;
    bool ui_exclusive = false;
    bool display_lowlevel = false;
    bool sao = false;
    bool grove = false;
    bool pixel_strip = false;
    /**
     * \brief Keep running and ticking in the background after the user leaves
     * the plugin's view, instead of being unloaded. Not a boot flag: the user
     * still starts the plugin manually. See \ref autoload for boot loading.
     */
    bool background = false;
    bool usb_cdc = false;
    bool prevent_sleep = false;
    /**
     * \brief Allow sandboxed file access on the plugins FAT partition via the
     * host_fs_* API. The plugin can only touch files in its own private folder
     * (/plugins/data/<id>/); paths are confined host-side.
     */
    bool vfat = false;
    /**
     * \brief Allow read access to the badge's own vCard and the received
     * vCard store via the host_vcard_* API.
     */
    bool vcard = false;
    /**
     * \brief Allow the plugin to run an inbound TCP listener via the
     * host_net_* API (the plugin picks the port). Accepted connections are
     * driven through the host_socket_* read/write/close API.
     */
    bool net_listen = false;
    /**
     * \brief Start this plugin as a resident background instance at badge boot.
     * Plugins without this flag stay unloaded until started manually.
     * Orthogonal to \ref background, which only governs survival after the user
     * leaves the view.
     */
    bool autoload = false;

    std::vector<std::string> rmem;
    std::vector<std::string> ecc;
    std::vector<std::string> ble_service_uuids;
    /// MIME types this plugin handles for badge-to-badge message transfer.
    /// A non-empty list implies messaging; sending also requires `ble`.
    std::vector<std::string> message_types;
    /// Named external features this plugin provides to other plugins
    /// (e.g. "thermo_print"). Other plugins invoke them via
    /// host_ext_feature_use; the provider must register a handler with
    /// host_ext_feature_register_handler in plugin_init.
    std::vector<std::string> provides;
    std::vector<uint8_t> gpio_pins;
    std::vector<uint8_t> pwm_pins;
    std::vector<uint8_t> adc_pins;
    std::vector<uint8_t> i2c_bus;
    std::string nvs_namespace;
};

struct LocalizedString {
    std::map<std::string, std::string> by_lang;
};

struct PluginManifest {
    std::string id;
    std::string version;
    std::string author;
    std::string icon;
    std::string host_api_level_min;
    uint16_t    api_level_major = 0;
    uint16_t    api_level_minor = 0;
    uint32_t    linear_memory_kb = 64;
    std::string default_language;
    std::map<std::string, LocalizedString> i18n_meta;
    std::map<std::string, LocalizedString> i18n_strings;
    PluginCapabilities capabilities;
    std::vector<PrerequisiteSpec> prerequisites;

    /**
     * \brief Parse `meta.json` content. Returns false on schema errors.
     */
    [[nodiscard]] static bool parse(const char* json, size_t len, PluginManifest& out);
};

}  // namespace cdc::plugin_manager
