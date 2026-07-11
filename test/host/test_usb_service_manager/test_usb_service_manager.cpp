// Host unit tests for the UsbServiceManager toggle registry:
//   - registration, lookup, duplicate rejection
//   - core-service toggle with callbacks, persistence and failure classification
//   - persisted-off applied at registration time
//   - module-backed delegation to ModuleRegistry (enable/start, stop/disable)
//   - Busy on suspended services, SlotBusy on shared-slot conflicts
// Run with: pio test -e native
//
// Uses the UsbManager shim (test_scard_usb/shim) plus local ModuleRegistry and
// Raii (in-memory NVS) shims. The manager singleton keeps its registrations
// across tests; setUp() only resets knobs and dynamic state.

#include <unity.h>

#include "../../../components/cdc_core/src/UsbServiceManager.cpp"

// Stub for declarations pulled in by cdc_log.h.
extern "C" void log_write(log_level_t, const char*, const char*, ...) {}

using namespace cdc::core;

static UsbServiceManager& svc() { return UsbServiceManager::instance(); }
static UsbManager& usb() { return UsbManager::instance(); }
static ModuleRegistry& mods() { return ModuleRegistry::instance(); }

// Fake core service ("aux") with observable callbacks.
static bool g_auxOn = true;
static bool g_auxEnableResult = true;
static bool g_auxSuspended = false;
static int g_auxEnableCalls = 0;
static int g_auxDisableCalls = 0;

static bool auxEnable() {
    g_auxEnableCalls++;
    if (!g_auxEnableResult) return false;
    g_auxOn = true;
    return true;
}

static void auxDisable() {
    g_auxDisableCalls++;
    g_auxOn = false;
}

static UsbServiceState auxState() {
    if (g_auxSuspended) return UsbServiceState::Suspended;
    return g_auxOn ? UsbServiceState::On : UsbServiceState::Off;
}

void setUp() {
    usb().reset();
    nvs_fake::reset();
    g_auxOn = true;
    g_auxEnableResult = true;
    g_auxSuspended = false;
    g_auxEnableCalls = 0;
    g_auxDisableCalls = 0;

    // Keep the fake modules registered (the manager references them by name)
    // but normalize their dynamic state.
    mods().startResult = true;
    mods().failure = ModuleStartFailure::Generic;
    mods().startCalls = 0;
    mods().setEnabledCalls = 0;
    for (uint8_t i = 0; i < mods().count; i++) {
        mods().enabled[i] = false;
        mods().modules[i].state = ServiceState::INITIALIZED;
        mods().modules[i].stopCalls = 0;
    }
}

void tearDown() {}

// --- registration & lookup ---

static void test_registration_and_lookup() {
    TEST_ASSERT_EQUAL_UINT8(5, svc().count());
    TEST_ASSERT_NOT_NULL(svc().find("cdc"));
    TEST_ASSERT_NOT_NULL(svc().find("aux"));
    TEST_ASSERT_NOT_NULL(svc().find("kbd"));
    TEST_ASSERT_NOT_NULL(svc().find("otphid"));
    TEST_ASSERT_NOT_NULL(svc().find("msc"));
    TEST_ASSERT_NULL(svc().find("nope"));

    // "cdc" was registered first by init().
    TEST_ASSERT_EQUAL_STRING("cdc", svc().at(0)->id);
}

static void test_duplicate_id_rejected() {
    UsbServiceDesc dup = {};
    dup.id = "cdc";
    dup.labelKey = "x";
    TEST_ASSERT_FALSE(svc().registerService(dup));
    TEST_ASSERT_EQUAL_UINT8(5, svc().count());
}

// --- core service toggles ---

static void test_cdc_toggle_calls_usbmanager_and_persists() {
    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Ok, svc().setEnabled("cdc", false));
    TEST_ASSERT_EQUAL_INT(1, usb().setCdcCalls);
    TEST_ASSERT_FALSE(usb().cdcEnabled());

    uint8_t stored = 0xFF;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_get_u8(1, "cdc", &stored));
    TEST_ASSERT_EQUAL_UINT8(0, stored);

    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Ok, svc().setEnabled("cdc", true));
    TEST_ASSERT_TRUE(usb().cdcEnabled());
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_get_u8(1, "cdc", &stored));
    TEST_ASSERT_EQUAL_UINT8(1, stored);
}

static void test_toggle_to_current_state_is_noop() {
    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Ok, svc().setEnabled("aux", true));
    TEST_ASSERT_EQUAL_INT(0, g_auxEnableCalls);
}

static void test_core_enable_failure_classified_as_budget() {
    g_auxOn = false;
    g_auxEnableResult = false;
    usb().usage = {USB_EP_BUDGET_MAX_IN, 0};  // aux cost {1,1} cannot fit

    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::BudgetFull, svc().setEnabled("aux", true));
    TEST_ASSERT_EQUAL_INT(1, g_auxEnableCalls);

    // Failure must not be persisted.
    uint8_t stored = 0xFF;
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, nvs_get_u8(1, "aux", &stored));
}

static void test_core_enable_failure_with_free_budget_is_generic() {
    g_auxOn = false;
    g_auxEnableResult = false;
    usb().usage = {0, 0};

    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Failed, svc().setEnabled("aux", true));
}

static void test_suspended_service_is_busy() {
    g_auxSuspended = true;
    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Busy, svc().setEnabled("aux", false));
    TEST_ASSERT_EQUAL_INT(0, g_auxDisableCalls);
}

static void test_unknown_id_not_found() {
    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::NotFound, svc().setEnabled("nope", true));
}

// --- persisted state applied at registration ---

static void test_persisted_off_applied_on_registration() {
    nvs_set_u8(1, "late", 0);

    static bool lateOn = true;
    static int lateDisableCalls = 0;
    UsbServiceDesc late = {};
    late.id = "late";
    late.labelKey = "x";
    late.enable = []() { lateOn = true; return true; };
    late.disable = []() { lateDisableCalls++; lateOn = false; };
    late.getState = []() { return lateOn ? UsbServiceState::On : UsbServiceState::Off; };

    TEST_ASSERT_TRUE(svc().registerService(late));
    TEST_ASSERT_EQUAL_INT(1, lateDisableCalls);
    TEST_ASSERT_FALSE(lateOn);
}

// --- module-backed services ---

static void test_module_enable_delegates_to_registry() {
    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Ok, svc().setEnabled("kbd", true));
    TEST_ASSERT_EQUAL_INT(1, mods().startCalls);
    TEST_ASSERT_TRUE(mods().isModuleEnabled(0));
    TEST_ASSERT_EQUAL(UsbServiceState::On, svc().state(2));  // kbd registered third
}

static void test_module_disable_stops_started_module() {
    svc().setEnabled("kbd", true);
    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Ok, svc().setEnabled("kbd", false));
    TEST_ASSERT_EQUAL_INT(1, mods().modules[0].stopCalls);
    TEST_ASSERT_FALSE(mods().isModuleEnabled(0));
}

static void test_module_start_failure_reverts_enable() {
    mods().startResult = false;
    mods().failure = ModuleStartFailure::UsbBudgetFull;

    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::BudgetFull, svc().setEnabled("kbd", true));
    TEST_ASSERT_FALSE(mods().isModuleEnabled(0));
}

static void test_shared_slot_conflict_rejected() {
    svc().setEnabled("kbd", true);

    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::SlotBusy, svc().setEnabled("otphid", true));
    TEST_ASSERT_EQUAL_INT(1, mods().startCalls);  // only the kbd start ran

    // After kbd is off, otphid may claim the slot.
    svc().setEnabled("kbd", false);
    TEST_ASSERT_EQUAL(UsbServiceManager::ToggleResult::Ok, svc().setEnabled("otphid", true));
}

static void test_module_state_mapping() {
    // Disabled module -> Off.
    TEST_ASSERT_EQUAL(UsbServiceState::Off, svc().state(2));

    // Enabled but not started -> Unavailable.
    mods().enabled[0] = true;
    TEST_ASSERT_EQUAL(UsbServiceState::Unavailable, svc().state(2));

    // Started -> On.
    mods().modules[0].state = ServiceState::STARTED;
    TEST_ASSERT_EQUAL(UsbServiceState::On, svc().state(2));
}

int main(int, char**) {
    // One-time setup mirroring boot: init() registers "cdc", then core and
    // module services register. The singleton keeps these across all tests.
    svc().init();

    UsbServiceDesc aux = {};
    aux.id = "aux";
    aux.labelKey = "x";
    aux.cost = {1, 1};
    aux.enable = auxEnable;
    aux.disable = auxDisable;
    aux.getState = auxState;
    svc().registerService(aux);

    mods().addModule("mod_usbhid");
    mods().addModule("mod_otphid");
    mods().addModule("mod_msc");
    svc().registerModuleService("kbd", "x", "mod_usbhid", {1, 0}, "otphid");
    svc().registerModuleService("otphid", "x", "mod_otphid", {1, 0}, "kbd");
    svc().registerModuleService("msc", "x", "mod_msc", {1, 1});

    UNITY_BEGIN();
    RUN_TEST(test_registration_and_lookup);
    RUN_TEST(test_duplicate_id_rejected);
    RUN_TEST(test_cdc_toggle_calls_usbmanager_and_persists);
    RUN_TEST(test_toggle_to_current_state_is_noop);
    RUN_TEST(test_core_enable_failure_classified_as_budget);
    RUN_TEST(test_core_enable_failure_with_free_budget_is_generic);
    RUN_TEST(test_suspended_service_is_busy);
    RUN_TEST(test_unknown_id_not_found);
    RUN_TEST(test_persisted_off_applied_on_registration);
    RUN_TEST(test_module_enable_delegates_to_registry);
    RUN_TEST(test_module_disable_stops_started_module);
    RUN_TEST(test_module_start_failure_reverts_enable);
    RUN_TEST(test_shared_slot_conflict_rejected);
    RUN_TEST(test_module_state_mapping);
    return UNITY_END();
}
