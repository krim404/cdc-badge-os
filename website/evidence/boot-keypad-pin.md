# Evidence Ledger: First boot, Keypad & input, Lock screen & PIN

All claims in the three pages trace to a concrete `path:line` below. Paths are
relative to the firmware repo root (`/Users/krim/GIT/cdc-badge-os`).

| Claim | Source (path:line) | Tag |
|-------|--------------------|-----|
| 12-button keypad physical layout: 1 2 3 / 4 5 6 / 7 8 9 / N 0 Y | components/cdc_hal/include/cdc_hal/IKeypad.h:8-19 | keypad |
| Key codes: '1'..'9', 'N' (KEY_NO), '0', 'Y' (KEY_YES), KEY_NONE=0 | components/cdc_hal/include/cdc_hal/IKeypad.h:16-21 | keypad |
| N = Cancel/OK comment: "N=Cancel, Y=OK" | components/cdc_hal/include/cdc_hal/IKeypad.h:12 | keypad |
| Y = Confirm/OK/Save, N = Cancel/Back/Backspace | components/cdc_views/include/cdc_views/KeyCodes.h:38-42 | keypad |
| KEY_UP = '2', KEY_DOWN = '8' (vertical navigation) | components/cdc_views/include/cdc_views/KeyCodes.h:30,33 | keypad |
| KEY_SELECT = '5' (center select / digit 5) | components/cdc_views/include/cdc_views/KeyCodes.h:36 | keypad |
| KEY_MENU = '3' (open context menu) | components/cdc_views/include/cdc_views/KeyCodes.h:45 | keypad |
| 'N' also acts as backspace in text-entry views | components/cdc_views/include/cdc_views/KeyCodes.h:18 | keypad |
| Long-press detection default threshold 800 ms | components/cdc_hal/include/cdc_hal/IKeypad.h:73 | keypad |
| Long-press enabled with 800 ms at init | components/cdc_os_ui/src/AppUi.cpp:785 | keypad |
| Panic/rescue chord = N + Y held together | components/cdc_hal/include/cdc_hal/IKeypad.h:84-86 | antiblock |
| PANIC_CHORD_BITS = (KEY_BIT_NO | KEY_BIT_YES) | components/cdc_hal/src/TCA9535Keypad.cpp:71 | antiblock |
| Panic chord fires once per hold | components/cdc_hal/src/TCA9535Keypad.cpp:474-481 | antiblock |
| Panic chord callback sets s_antiBlockLockRequested | components/cdc_os_ui/src/AppUi.cpp:786-788 | antiblock |
| ui_process performs anti-block lock before key dispatch | components/cdc_os_ui/src/AppUi.cpp:1089-1091 | antiblock |
| performAntiBlockLock: unloads plugins, hides modals, pops to lock screen, dispatchLock | components/cdc_os_ui/src/AppUi.cpp:425-444 | antiblock |
| ListView: KEY_UP/KEY_DOWN navigate, KEY_YES select, KEY_MENU context, KEY_NO back | components/cdc_views/src/ListView.cpp:146-202 | list |
| ListView long-press 2 = jump to first, long-press 8 = jump to last | components/cdc_views/src/ListView.cpp:205-213 | list |
| ContextMenuView keys: 2 up, 8 down, Y select, N cancel | components/cdc_views/src/ContextMenuView.cpp:114-133 | contextmenu |
| ContextMenuView auto-dismiss timeout 60000 ms | components/cdc_views/src/ContextMenuView.cpp:40 | contextmenu |
| T9 multi-tap mapping table (per-key character sets) | components/cdc_views/src/T9InputView.cpp:34-44 | t9 |
| T9 key 0 = " 0" (space, 0) | components/cdc_views/src/T9InputView.cpp:35 | t9 |
| T9 key 2 = abc/ABC/2, key 7 = pqrs/PQRS/7, key 9 = wxyz/WXYZ/9 | components/cdc_views/src/T9InputView.cpp:37,42,44 | t9 |
| T9 commit timeout TIMEOUT_MS = 2000 ms | components/cdc_views/include/cdc_views/T9InputView.h:25 | t9 |
| T9 same-key before timeout cycles characters; new key or timeout commits | components/cdc_views/src/T9InputView.cpp:124-150 | t9 |
| T9 onTick commits character after timeout | components/cdc_views/src/T9InputView.cpp:230-241 | t9 |
| T9 keys: 0-9 input, long-press 0-9 insert digit, N backspace(short)/cancel(long), Y confirm | components/cdc_views/include/cdc_views/T9InputView.h:14-20 | t9 |
| T9 Y confirm pops then calls onSave; long-press N cancels | components/cdc_views/src/T9InputView.cpp:251-303 | t9 |
| T9 MAX_TEXT_LEN = 320 | components/cdc_views/include/cdc_views/T9InputView.h:24 | t9 |
| SliderView keys: 4 decrease, 6 increase, Y save, N cancel | components/cdc_views/include/cdc_views/SliderView.h:12-19 | slider |
| SliderView onKey '6' increase, '4' decrease, Y save, N cancel | components/cdc_views/src/SliderView.cpp:108-138 | slider |
| SliderView key repeat: initial 350 ms, period 80 ms while 4/6 held | components/cdc_views/src/SliderView.cpp:21-22,150-168 | slider |
| DateInputView: digits enter day/month/year, auto-advance, N clear/cancel, Y confirm | components/cdc_views/src/DateInputView.cpp:167-213 | date |
| DateInputView field order Day -> Month -> Year, auto-advance after 2nd digit | components/cdc_views/src/DateInputView.cpp:101-141 | date |
| DateInputView clamps year 2000..2099, month 1..12, day 1..31 | components/cdc_views/src/DateInputView.cpp:152-160 | date |
| TimeInputView: hour 0..23, minute 0..59, auto-advance hour->minute, N clear/cancel, Y confirm | components/cdc_views/src/TimeInputView.cpp:95-178 | time |
| PinEntryView keys: 0-9 add digit, N backspace(short)/cancel(empty), Y verify | components/cdc_views/include/cdc_views/PinEntryView.h:14-18 | pin |
| PinEntryView masked dots (filled circle entered, empty circle remaining) | components/cdc_views/src/PinEntryView.cpp:268-283 | pin |
| PinEntryView default minLength = PinManager::BADGE_PIN_MIN; min-length check before verify | components/cdc_views/src/PinEntryView.cpp:50,140-146 | pin |
| Badge PIN min length = 4 | components/cdc_core/include/cdc_core/PinManager.h:52 | pin |
| Badge PIN max length = 8 | components/cdc_core/include/cdc_core/PinManager.h:53 | pin |
| Lock-screen PIN entry created with maxLength 8, maxAttempts 3 | components/cdc_os_ui/src/AppUi.cpp:867-868 | pin |
| Default badge PIN = "123456" | components/cdc_core/include/cdc_core/PinManager.h:80 | pin |
| Badge retry counter starts at MAX_RETRIES = 3 | components/cdc_core/include/cdc_core/PinManager.h:266,236 | pin |
| Lockout duration = 60000 ms (60 s) | components/cdc_core/include/cdc_core/PinManager.h:99 | pin |
| Boot grants one attempt (or zero if locked), 60 s recovery timer restores retries | components/cdc_core/include/cdc_core/PinManager.h:31-37 | pin |
| isBadgeBlocked = retries 0 OR time lockout active | components/cdc_core/include/cdc_core/PinManager.h:94 | pin |
| PinEntryView lockout: ignores keys while locked, shows countdown each second | components/cdc_views/src/PinEntryView.cpp:101-117,185-208 | pin |
| Wrong PIN clears buffer, increments attempts, shows "Wrong PIN" or "Locked out" | components/cdc_views/src/PinEntryView.cpp:148-173 | pin |
| Change PIN: 3-step wizard (current, new, confirm) | components/cdc_os_ui/include/cdc_os_ui/views/PinChangeView.h:11-31 | changepin |
| Change PIN wizard min/max from PinManager BADGE_PIN_MIN/MAX (4/8) | components/cdc_os_ui/src/AppUi.cpp:626-628,951-952 | changepin |
| Change PIN step 1 verifies current PIN, step 3 requires new==confirm | components/cdc_views/src/views/PinChangeView.cpp (confirmStep) -> components/cdc_os_ui/src/views/PinChangeView.cpp:145-238 | changepin |
| Change PIN reached via Settings -> Change PIN (SETTINGS_IDX_CHANGE_PIN) | components/cdc_os_ui/src/AppUi.cpp:72,624-630 | changepin |
| Change PIN: N on empty step 1 cancels; on later steps goes back a step | components/cdc_os_ui/src/views/PinChangeView.cpp:264-286 | changepin |
| Change PIN: input rejected while locked out (REQUEST_POP) | components/cdc_os_ui/src/views/PinChangeView.cpp:256-260 | changepin |
| Inactivity auto-lock timeout = 5 * 60 * 1000 ms (5 min) | components/cdc_os_ui/src/AppUi.cpp:65 | autolock |
| Inactivity timer registered via setInactivityTimeout | components/cdc_os_ui/src/AppUi.cpp:973 | autolock |
| onInactivityTimeout pops back to lock screen (depth>1), unless prevent_sleep plugin foreground | components/cdc_os_ui/src/AppUi.cpp:405-421 | autolock |
| Any key triggers unlock; '3' opens context menu instead | components/cdc_os_ui/src/views/LockScreenView.cpp:359-405 | lockscreen |
| onUnlockRequested pushes PIN entry; ignores stale keys until release | components/cdc_os_ui/src/AppUi.cpp:339-347 | lockscreen |
| Successful PIN: replace with main menu, dispatchUnlock, backlight on | components/cdc_os_ui/src/AppUi.cpp:394-398 | lockscreen |
| Lock screen layout: clock, date, status icons, name, info lines, battery | components/cdc_os_ui/include/cdc_os_ui/views/LockScreenView.h:46-56 | lockscreen |
| StatusIcon enum: LOCK, DEEP_SLEEP, LIGHT_SLEEP, BACKLIGHT, USB, BLE, WIFI, SAO, CHARGING, NO_BATTERY, CAFFEINATED, BACKGROUND | components/cdc_os_ui/include/cdc_os_ui/views/LockScreenView.h:22-34 | lockscreen |
| Icons actually rendered: LOCK, WIFI, BLE, USB, BACKLIGHT(sun), DEEP_SLEEP(zzZ), LIGHT_SLEEP(z), CAFFEINATED(coffee), BACKGROUND(play) | components/cdc_os_ui/src/views/LockScreenView.cpp:459-573 | lockscreen |
| Battery icon: fill level, charging bolt, no-battery strike-through | components/cdc_os_ui/src/views/LockScreenView.cpp:415-451 | lockscreen |
| USB/CHARGING/WIFI/BLE/BACKGROUND icons driven by hardware state | components/cdc_os_ui/src/AppUi.cpp:284-307 | lockscreen |
| NO_BATTERY icon when no battery present | components/cdc_os_ui/src/AppUi.cpp:255-270 | lockscreen |
| Clock "HH:MM" / "--:--", date "DD.MM.YYYY", updated each minute | components/cdc_os_ui/src/AppUi.cpp:319-334; LockScreenView.cpp:120-148 | lockscreen |
| Lock-screen long-press N (5 s) enters deep sleep | components/cdc_os_ui/include/cdc_os_ui/views/LockScreenView.h:15; LockScreenView.cpp:443-489 | lockscreen |
| DEEP_SLEEP_HOLD_MS = 5000 | components/cdc_os_ui/include/cdc_os_ui/views/LockScreenView.h:15 | lockscreen |
| Lock-screen context menu (key 3): Light toggle, WiFi toggle, module/plugin items | components/cdc_os_ui/src/views/LockScreenView.cpp:362-404 | lockscreen |
| Lock-screen footer hint "Any key: unlock  [3]: menu" | components/cdc_ui/src/I18n.cpp:80 | lockscreen |
| PIN entry title "Enter PIN" | components/cdc_ui/src/I18n.cpp:79 | lockscreen |
| Duress PIN check runs before badge verify; selfDestruct does not return | components/cdc_os_ui/src/AppUi.cpp:353-364 | duress |
| Duress PIN must differ from badge PIN | components/cdc_core/include/cdc_core/PinManager.h:139-147 | duress |
| NO first-boot PIN-setup wizard: device ships with default PIN, lock screen shown directly | components/cdc_os_ui/src/AppUi.cpp:961-972 (push lock screen, no setup branch); PinManager.h:80 | boot |
