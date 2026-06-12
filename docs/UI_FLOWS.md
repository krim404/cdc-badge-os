# UI Flows Reference

User interface navigation and interaction patterns for the CDC Badge.

> **Note:** This document shows representative UI flows and may not reflect all current screens. It is maintained for illustration purposes only.

## Navigation

| Key | Function |
|-----|----------|
| **Y** | Confirm / Select / Approve |
| **N** | Cancel / Back / Deny |
| **1-9, 0, *, #** | Input / Navigate |

## Lock Screen

The default view when device is locked.

```
┌─────────────────────────┐
│ 🔋 85%     14:32   🔒  │
├─────────────────────────┤
│                         │
│      Badge Name         │
│      Info Line 1        │
│      Info Line 2        │
│                         │
├─────────────────────────┤
│ [Y] Unlock   [N] Sleep  │
└─────────────────────────┘
```

**Actions:**
- **Y** → PIN Entry → Main Menu
- **N** (hold 5s) → Deep Sleep

## Main Menu

After unlocking, shows module entries + Tools + Settings.

```
Main Menu
─────────────────────────
> FIDO2/WebAuthn
  TOTP
  Passwords
  GPG
  Tools
  Settings
─────────────────────────
[Y] Select  [N] Lock
```

**Module entries** (from registered modules):
- FIDO2/WebAuthn
- TOTP
- Passwords
- GPG

**Fixed entries:**
- Tools
- Settings

## FIDO2/WebAuthn Module

### Credential List

```
FIDO2 Credentials
─────────────────────────
> github.com (user@...)
  google.com (user@...)
  (empty slots)
─────────────────────────
[Y] Details  [N] Back
```

### Authentication Prompt

When a website requests authentication:

```
┌─────────────────────────┐
│     FIDO2 Request       │
├─────────────────────────┤
│                         │
│   github.com            │
│   wants to sign in      │
│                         │
├─────────────────────────┤
│  [Y] Approve  [N] Deny  │
└─────────────────────────┘
```

## TOTP Module

### Account List

```
TOTP
─────────────────────────
> Add Account
  GitHub
  Google
  AWS
─────────────────────────
[Y] Select  [N] Back
```

### Code Display

```
TOTP Code
─────────────────────────
  GitHub

     847 293

  ████████░░░░  18s
─────────────────────────
[3] Edit  [N] Back
```

### Add Account Wizard

1. **Account Name** → T9 Input
2. **Secret (Base32)** → T9 Input
3. **Issuer** → T9 Input (optional)
4. **Digits** → Select (6/7/8)
5. **Algorithm** → Select (SHA1/SHA256/SHA512)
6. **Period** → Select (30s/60s)

## Password Module

### Entry List

```
Passwords
─────────────────────────
> Add Entry
  GitHub (user@example)
  AWS Console (admin)
─────────────────────────
[Y] Select  [N] Back
```

### Entry Details

```
Password Details
─────────────────────────
Name: GitHub
User: user@example.com
URL:  github.com
─────────────────────────
[Y] Show PW  [N] Back
```

## Tools Menu

```
Tools
─────────────────────────
> Modules
  WiFi
  Bluetooth
  Hardware Info
  Plugins
─────────────────────────
[Y] Select  [N] Back
```

### Modules View

Shows all registered modules with status:

```
Modules
─────────────────────────
  mod_fido2      [ON]
  mod_2fa        [ON]
  mod_password   [ON]
  mod_gpg        [ON]
─────────────────────────
[Y] Toggle  [N] Back
```

### WiFi Menu

```
WiFi
─────────────────────────
> Connect
  Setup Network
  Connection Details
  NTP Sync
─────────────────────────
[Y] Select  [N] Back
```

### WiFi Scan

```
WiFi Networks
─────────────────────────
> MyNetwork      -45dB 🔒
  GuestWiFi      -62dB
  Neighbor       -78dB 🔒
─────────────────────────
[Y] Connect  [N] Back
```

## Settings Menu

```
Settings
─────────────────────────
> Brightness
  Language
  Timezone
  Auto Sleep
  Badge Text
  Set Date
  Set Time
  Change PIN
─────────────────────────
[Y] Select  [N] Back
```

### Brightness Slider

```
Brightness
─────────────────────────

  ████████████░░░░  75%

─────────────────────────
[Y] Save  [N] Cancel
```

### Language Selection

```
Language
─────────────────────────
> English
  Deutsch
─────────────────────────
[Y] Select  [N] Back
```

### Change PIN

```
Change PIN
─────────────────────────

Enter current PIN:
  ****

─────────────────────────
[Y] Confirm  [N] Cancel
```

Then:
```
Enter new PIN:
  ****
```

Then:
```
Confirm new PIN:
  ****
```

## T9 Input

Text input using phone-style multi-tap:

```
Account Name
─────────────────────────

  GITHUB_

  [2ABC] [3DEF]

─────────────────────────
[Y] Save  [*] Symbols
```

| Key | Characters |
|-----|------------|
| 1 | . @ - _ 1 |
| 2 | A B C 2 |
| 3 | D E F 3 |
| 4 | G H I 4 |
| 5 | J K L 5 |
| 6 | M N O 6 |
| 7 | P Q R S 7 |
| 8 | T U V 8 |
| 9 | W X Y Z 9 |
| 0 | Space 0 |
| * | Symbols mode |
| # | Backspace |

## Toast Messages

Temporary overlay for success/error feedback:

```
┌─────────────────────────┐
│                         │
│     ✓ Saved             │
│                         │
└─────────────────────────┘
```

## Confirm Dialog

For destructive actions:

```
┌─────────────────────────┐
│      Delete Entry?      │
├─────────────────────────┤
│  This cannot be undone  │
├─────────────────────────┤
│  [Y] Yes    [N] No      │
└─────────────────────────┘
```

## Power States

| State | Trigger | Wake | Display |
|-------|---------|------|---------|
| Active | Normal use | - | On |
| Light Sleep | Idle on lock screen | Any key | Off (fast wake) |
| Deep Sleep | Hold N 5s on lock | Y key only | Off (slow wake) |
