# Evidence ledger: Settings, Power & Wi-Fi pages

Each row backs one claim in `guide/settings.md`, `guide/power-sleep.md`, or
`guide/wifi-time.md`. Tags: VERIFIED (read in source), GAP (could not verify).

| Claim | Source (path:line) | Tag |
|-------|--------------------|-----|
| **SETTINGS PAGE** | | |
| Settings menu reached from Main Menu fixed "Settings" entry | components/cdc_os_ui/src/AppUi.cpp:464, 564-565 | VERIFIED |
| Settings menu items, in order: Brightness, Language, Timezone, Sleep Interval, Badge Text, Set Date, Set Time, Change PIN | components/cdc_os_ui/src/AppUi.cpp:69-79 (SettingsMenuIdx enum), 524-531 | VERIFIED |
| Settings labels map to i18n keys core.brightness/language/timezone/auto_sleep/badge_text/set_date/set_time/change_pin | components/cdc_os_ui/src/AppUi.cpp:524-531 | VERIFIED |
| Brightness is a SliderView | components/cdc_os_ui/src/AppUi.cpp:603-604, 901-906 | VERIFIED |
| Brightness slider range 0..100, unit "%" | components/cdc_os_ui/src/AppUi.cpp:903 (init(...,0,100,...,"%")) | VERIFIED |
| Brightness value*10 applied to backlight (so 0..100 -> 0..1000) | components/cdc_os_ui/src/SettingsHandlers.cpp:68-73, 80-84 | VERIFIED |
| Brightness preview live on change, persists on save (Y) | components/cdc_os_ui/src/SettingsHandlers.cpp:68-84; AppUi.cpp:904-906 | VERIFIED |
| Slider keys: 4 decrease, 6 increase, Y save, N cancel | components/cdc_views/include/cdc_views/SliderView.h:13-19 | VERIFIED |
| Language is a ListView picker | components/cdc_os_ui/src/AppUi.cpp:606-609, 931-932 | VERIFIED |
| Language list = English (in-code) + one entry per lang_<code>.json overlay | components/cdc_os_ui/src/AppUi.cpp:641-669 (rebuildLanguageMenu); 655-658 | VERIFIED |
| Overlay languages discovered by scanning /vfat/system/i18n for lang_*.json | components/cdc_ui/src/I18n.cpp (scanAvailableLanguages); OVERLAY_DIR | VERIFIED |
| Each overlay labelled by its own core.lang_name endonym; falls back to code | components/cdc_ui/src/I18n.cpp:407, 419-422, 554-564 | VERIFIED |
| English entry's name from in-code core.lang_name = "English" | components/cdc_ui/src/I18n.cpp:556-558; I18n.cpp:100 (core.lang_name "English") | VERIFIED |
| Selecting a language applies it and persists (NVS) | components/cdc_os_ui/src/AppUi.cpp:676-683; I18n.cpp:566-579 (NVS langc) | VERIFIED |
| Up to 16 languages shown (MAX_LANGUAGES) | components/cdc_os_ui/src/AppUi.cpp:82 | VERIFIED |
| Timezone is a SliderView, slider range 0..26, unit "h", display offset -12 | components/cdc_os_ui/src/AppUi.cpp:610, 919-927 | VERIFIED |
| Timezone displayed -12..+14 (slider value - 12) | components/cdc_os_ui/src/AppUi.cpp:923-925; SettingsHandlers.cpp:119-124 | VERIFIED |
| Timezone offset stored via RTC setTimezoneOffset; lock clock refreshed | components/cdc_os_ui/src/SettingsHandlers.cpp:119-137 | VERIFIED |
| Sleep Interval is a SliderView, range 0..60, unit core.minutes ("min") | components/cdc_os_ui/src/AppUi.cpp:613, 909-916 | VERIFIED |
| Sleep Interval value 0 shows "Never" (core.never) | components/cdc_os_ui/src/AppUi.cpp:915 (setZeroLabel core.never) | VERIFIED |
| Sleep Interval is in MINUTES; stored as value*60 seconds via setLightSleepInterval | components/cdc_os_ui/src/SettingsHandlers.cpp:108-112; AppUi.cpp:911-913 | VERIFIED |
| This slider sets the light-sleep timer interval, NOT the inactivity-to-sleep delay | components/cdc_hal/include/cdc_hal/ISleepController.h:100-109 (setLightSleepInterval = timer wakeup interval) | VERIFIED |
| Badge Text edits via T9 wizard, 3 steps: Name, Info, Info 2 | components/cdc_os_ui/src/AppUi.cpp:616-617; SettingsHandlers.cpp:194-231, 25-29 | VERIFIED |
| Badge Text fields persisted to NVS namespace "display" | components/cdc_os_ui/src/SettingsHandlers.cpp:272-280 | VERIFIED |
| Badge Text max length = LockScreenView::MAX_TEXT_LEN = 64 | components/cdc_os_ui/src/SettingsHandlers.cpp:230; LockScreenView.h:60 | VERIFIED |
| Badge text labels: core.name, core.info, core.info2 | components/cdc_os_ui/src/SettingsHandlers.cpp:212-225 | VERIFIED |
| Set Date is a DateInputView, day/month/year fields | components/cdc_os_ui/src/AppUi.cpp:619, 937-942 | VERIFIED |
| DateInput keys: 0-9 digits, 4 prev field, 6 next field, N clear/cancel, Y confirm | components/cdc_views/include/cdc_views/DateInputView.h:11-17 | VERIFIED |
| Set Date applies to system clock via settimeofday | components/cdc_os_ui/src/SettingsHandlers.cpp:146-158 | VERIFIED |
| Set Time is a TimeInputView, hour/minute fields | components/cdc_os_ui/src/AppUi.cpp:622, 944-948 | VERIFIED |
| TimeInput keys: 0-9 digits, 4 prev, 6 next, N clear/cancel, Y confirm | components/cdc_views/include/cdc_views/TimeInputView.h:11-17 | VERIFIED |
| Set Time applies to system clock via settimeofday | components/cdc_os_ui/src/SettingsHandlers.cpp:166-178 | VERIFIED |
| Change PIN opens PinChangeView, badge PIN length 4..8 digits | components/cdc_os_ui/src/AppUi.cpp:625-630, 951-953; PinManager.h:49-50 (BADGE_PIN_MIN 4, BADGE_PIN_MAX 8) | VERIFIED |
| **POWER / SLEEP PAGE** | | |
| Light sleep entered from lock screen after idle timeout | components/cdc_os_ui/src/SleepManager.cpp:69-101; AppUi.cpp:1123-1128 | VERIFIED |
| Light sleep idle timeout LIGHT_SLEEP_TIMEOUT_MS = 120000 ms (2 minutes) | components/cdc_os_ui/include/cdc_os_ui/SleepManager.h:17 | VERIFIED |
| Light sleep NOT entered while USB connected (timer reset, icon removed) | components/cdc_os_ui/src/SleepManager.cpp:81-88 | VERIFIED |
| Light sleep NOT entered while sleep is inhibited (caffeinated) | components/cdc_os_ui/src/SleepManager.cpp:91-94 | VERIFIED |
| Light sleep only checked at lock screen (stack depth == 1) | components/cdc_os_ui/src/SleepManager.cpp:70-71; AppUi.cpp:1123-1128 | VERIFIED |
| Light sleep wakeup sources: GPIO (key) and TIMER | components/cdc_os_ui/src/SleepManager.cpp:144-196; ISleepController.h:20-27, 61-67 | VERIFIED |
| Timer wakeup: refresh clock, keep light-sleep icon, re-enter sleep | components/cdc_os_ui/src/SleepManager.cpp:159-189 | VERIFIED |
| GPIO/key wakeup: clear icon, reset timer, refresh display, exit loop | components/cdc_os_ui/src/SleepManager.cpp:146-157 | VERIFIED |
| USB connect during light sleep exits light-sleep mode | components/cdc_os_ui/src/SleepManager.cpp:181-187 | VERIFIED |
| Light-sleep timer wakeup interval = the Sleep Interval setting (setLightSleepInterval) | components/cdc_hal/include/cdc_hal/ISleepController.h:100-109; SettingsHandlers.cpp:108-112 | VERIFIED |
| Light sleep shows StatusIcon::LIGHT_SLEEP on lock screen | components/cdc_os_ui/src/SleepManager.cpp:114; LockScreenView.h:26 | VERIFIED |
| Deep sleep triggered by holding N on lock screen for DEEP_SLEEP_HOLD_MS = 5000 ms | components/cdc_os_ui/src/views/LockScreenView.cpp:408-450; LockScreenView.h:14-15 (DEEP_SLEEP_HOLD_MS 5000) | VERIFIED |
| Deep sleep trigger only active on lock screen (LockScreenView::onTick) | components/cdc_os_ui/src/views/LockScreenView.cpp:399-402 | VERIFIED |
| Deep sleep: render sleep screen, backlight off, 2s delay, then enterDeepSleep | components/cdc_os_ui/src/views/LockScreenView.cpp:421-440 | VERIFIED |
| Deep sleep causes RESET on wake; only GPIO wakeup configured (no timer) | components/cdc_hal/include/cdc_hal/ISleepController.h:11-15, 69-82 | VERIFIED |
| Deep sleep does not return (noreturn) | components/cdc_hal/include/cdc_hal/ISleepController.h:82 | VERIFIED |
| Deep-sleep screen footer hint = core.deep_sleep ("Deep Sleep") | components/cdc_os_ui/src/views/LockScreenView.cpp:456-459; I18n.cpp:81 | VERIFIED |
| Battery percent shown on lock screen | components/cdc_os_ui/src/AppUi.cpp:254-279, 852-853; LockScreenView.h:99-101 | VERIFIED |
| Battery percent sampled at most every BATTERY_SAMPLE_INTERVAL_MS = 30000 ms | components/cdc_os_ui/src/AppUi.cpp:149-150, 273-278 | VERIFIED |
| Battery percent range 0-100 from getBatteryPercent | components/cdc_hal/include/cdc_hal/IPowerManager.h:40-43 | VERIFIED |
| No-battery sentinel: NO_BATTERY icon + strike-through, percent set 0 | components/cdc_os_ui/src/AppUi.cpp:258-266; LockScreenView.cpp:493-497 | VERIFIED |
| Charging icon shown for FAST_CHARGE or PRE_CHARGE | components/cdc_os_ui/src/AppUi.cpp:288-291, 301-302 | VERIFIED |
| USB icon shown when USB connected | components/cdc_os_ui/src/AppUi.cpp:287, 301 | VERIFIED |
| Lock screen power status refreshed pre-render (power->refresh + updatePowerStatusIcons) | components/cdc_os_ui/src/AppUi.cpp:284-308, 825-830 | VERIFIED |
| Sleep inhibitors (caffeinated) prevent light sleep; up to MAX_SLEEP_INHIBITORS = 8 | components/cdc_os_ui/src/SleepManager.cpp:91-94, 208-231; SleepManager.h:20, 49-66 | VERIFIED |
| Caffeinated icon shown while >=1 inhibitor active | components/cdc_os_ui/src/SleepManager.cpp:265-273; LockScreenView.h:32 | VERIFIED |
| Inactivity (away from lock screen) auto-locks after INACTIVITY_TIMEOUT_MS = 300000 ms (5 min) | components/cdc_os_ui/src/AppUi.cpp:65, 973; 407-422 (onInactivityTimeout) | VERIFIED |
| No auto-lock while a prevent_sleep plugin holds foreground | components/cdc_os_ui/src/AppUi.cpp:409-413 | VERIFIED |
| README power table corroborates Light/Deep sleep triggers and ship mode | README.md:309-318 | VERIFIED |
| Lock-screen 3 key opens menu; any other key unlocks | components/cdc_os_ui/src/views/LockScreenView.cpp:355-392; I18n.cpp:80 | VERIFIED |
| **WI-FI / TIME PAGE** | | |
| Wi-Fi menu reached from Tools menu (core.wifi_menu) | components/cdc_os_ui/src/AppUi.cpp:484-489 (kToolsFixed showWifiMainMenu); WifiMenuUi.cpp:239-249 | VERIFIED |
| Wi-Fi menu items: Connect/Disconnect, WiFi Setup, Details, Sync Time | components/cdc_os_ui/src/WifiMenuUi.cpp:38-44, 202-234 | VERIFIED |
| Connect entry label: Connect (config), Disconnect (connected), or "No WiFi configured" (no config, disabled) | components/cdc_os_ui/src/WifiMenuUi.cpp:208-214 | VERIFIED |
| WiFi Setup runs a scan-then-wizard flow | components/cdc_os_ui/src/WifiMenuUi.cpp:337-340, 361-431 | VERIFIED |
| Scan timeout WIFI_SCAN_TIMEOUT_MS = 10000 ms | components/cdc_os_ui/include/cdc_os_ui/WifiHandlers.h:12; WifiMenuUi.cpp:379 | VERIFIED |
| Scan list sorted by RSSI desc, shows SSID + signal bars + lock for secured | components/cdc_os_ui/src/WifiMenuUi.cpp:345-356, 159-197 | VERIFIED |
| Scan list has "+ Add Manual" entry (core.wifi_add_manual) for manual SSID | components/cdc_os_ui/src/WifiMenuUi.cpp:420-421, 442-450 | VERIFIED |
| Secured networks prompt for password (T9, max 64 chars) | components/cdc_os_ui/src/WifiMenuUi.cpp:456-462, 510-512 | VERIFIED |
| Open networks skip password | components/cdc_os_ui/src/WifiMenuUi.cpp:456-461, 499-503 | VERIFIED |
| Manual entry asks for encryption: WPA2, WPA/WPA2, WPA3, WPA, Open, WEP | components/cdc_os_ui/src/WifiMenuUi.cpp:46-54, 79-81, 467-505 | VERIFIED |
| IP mode step: DHCP (Auto) or Static IP | components/cdc_os_ui/src/WifiMenuUi.cpp:56-60, 527-538; I18n.cpp:134-135 | VERIFIED |
| Static IP wizard collects IP, Gateway, Netmask (validated IPv4) | components/cdc_os_ui/src/WifiMenuUi.cpp:545-614; WifiHandlers.cpp:50-55 | VERIFIED |
| Default netmask 255.255.255.0 | components/cdc_os_ui/include/cdc_os_ui/WifiHandlers.h:23; WifiHandlers.cpp:24 | VERIFIED |
| After wizard, config saved to NVS and connection attempted | components/cdc_os_ui/src/WifiMenuUi.cpp:619-628 | VERIFIED |
| Connect timeout default 15000 ms, range 3000..60000 ms (NVS key tout) | components/cdc_os_ui/include/cdc_os_ui/WifiHandlers.h:9-11; WifiHandlers.cpp:73-99 | VERIFIED |
| Details view shows Status, SSID, MAC, IP, Signal (dBm) when connected | components/cdc_os_ui/src/WifiMenuUi.cpp:633-679 | VERIFIED |
| Details view shows saved SSID + DHCP/Static when disconnected with config | components/cdc_os_ui/src/WifiMenuUi.cpp:670-677 | VERIFIED |
| Sync Time (NTP) requires a saved config or active connection | components/cdc_os_ui/src/WifiMenuUi.cpp:685-691; WifiHandlers.cpp:297-302 | VERIFIED |
| Sync Time connects Wi-Fi first if not connected | components/cdc_os_ui/src/WifiMenuUi.cpp:693-704; WifiHandlers.cpp:297-316 | VERIFIED |
| NTP servers: pool.ntp.org, time.google.com | components/cdc_os_ui/src/WifiHandlers.cpp:322-323 | VERIFIED |
| NTP sync timeout NTP_SYNC_TIMEOUT_MS = 10000 ms | components/cdc_os_ui/include/cdc_os_ui/WifiHandlers.h:13; WifiHandlers.cpp:334 | VERIFIED |
| On NTP success RTC time marked set | components/cdc_os_ui/src/WifiHandlers.cpp:348-355 | VERIFIED |
| Sync Time outcome toasts: core.ntp_success / core.ntp_timeout | components/cdc_os_ui/src/WifiMenuUi.cpp:710-714 | VERIFIED |
| Wi-Fi intent persisted (NVS key ena) and restored at boot | components/cdc_os_ui/src/WifiHandlers.cpp:404-410, 463-468 | VERIFIED |
| Wi-Fi connection icon on lock screen when connected | components/cdc_os_ui/src/AppUi.cpp:292-293, 303; LockScreenView.h:28 | VERIFIED |
| Max scan results = IWifiController::MAX_SCAN_RESULTS | components/cdc_os_ui/src/WifiMenuUi.cpp:26 | VERIFIED |
| TOTP / clock-dependent features need a valid time | inference linking Sync Time + RTC isTimeSet (WifiHandlers.cpp:369-399). Generic TOTP-needs-clock specifics not located in cited sources | GAP |
| WPA/WPA2 selection maps internally to WPA2_PSK | components/cdc_os_ui/src/WifiMenuUi.cpp:491-492 | VERIFIED |
