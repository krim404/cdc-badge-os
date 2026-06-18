# Feature Specification: USB Mass Storage (vFAT file transfer)

**Feature Branch**: `019-usb-mass-storage`

**Created**: 2026-06-18

**Status**: Draft

**Input**: User description: "ein neues modul soll erstellt werden: USB-Mass storage device, um dateien leicht auf das badge vfat übertragen zu können. es soll standardmäßig deaktiviert sein, also wie die anderen usb services auch."

## Clarifications

### Session 2026-06-18

- Q: Storage scope — expose the entire vFAT volume (system content reachable by the host) or isolate a user-only volume? → A: Expose the entire vFAT volume. The system folder (named `system`, hidden only in the badge's own user view) is therefore reachable by the connected host and no separate user-only partition is introduced.
- Q: How should the `system` folder be protected from host modification? → A: Advisory only — mark `system` and its contents read-only + hidden + system (FAT attributes) so standard host file managers hide it and refuse to change or delete it. This is not a hard, device-enforced boundary (a host with raw block access can override it); lost system content is recoverable by re-flashing the default image.
- Q: While the host has the volume mounted, does the badge lock itself out of its own storage? → A: No. The badge only reads this volume in normal operation, so there is no badge-write vs host-write conflict to guard against. The badge keeps read access (Files browser and viewers stay available) and simply does not write to the volume while the host is connected. A transient inconsistent read while the host is mid-write is acceptable (display glitch only, no corruption).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Copy files onto the badge over USB (Priority: P1)

A user wants to put files (for example an image or a Markdown document) onto the
badge without the serial upload tooling. They enable the USB Mass Storage service
on the badge, plug the badge into a computer, and the badge shows up as an ordinary
removable drive. They drag the files onto that drive, safely eject it, and the files
then appear in the badge's "Files" menu and are usable by the badge (e.g. opened in
the image or Markdown viewer).

**Why this priority**: This is the core value of the feature. Drag-and-drop from a
file manager is the easiest possible transfer path and removes the need for the
serial companion tools for the common case. Without it the feature delivers nothing.

**Independent Test**: Enable the service, connect the badge to a computer, copy one
file onto the drive, eject, disconnect, and confirm the file is listed in the badge's
Files browser and opens correctly.

**Acceptance Scenarios**:

1. **Given** the USB Mass Storage service is enabled and the badge is connected to a
   computer, **When** the user opens the computer's file manager, **Then** the badge
   appears as a removable drive showing the existing user files.
2. **Given** the badge drive is open on the computer, **When** the user copies a file
   onto it and safely ejects the drive, **Then** the file is present in the badge's
   "Files" menu after the transfer completes.
3. **Given** a supported file (image or Markdown) was copied onto the badge, **When**
   the user opens it from the Files menu, **Then** it renders correctly, identical to
   a file uploaded by the existing serial tools.

---

### User Story 2 - Copy files off the badge to a computer (Priority: P2)

A user wants to retrieve a file that lives on the badge (for example to back it up or
inspect it on a computer). With the service enabled and the badge connected, they open
the badge drive and copy files from it to the computer.

**Why this priority**: Bidirectional transfer rounds out the workflow and is a natural
expectation of a mass-storage drive, but the inbound direction (Story 1) is what the
request centres on, so this is secondary.

**Independent Test**: With a known file present on the badge, connect it with the
service enabled and confirm the file can be copied to the computer and is byte-for-byte
identical to the original.

**Acceptance Scenarios**:

1. **Given** files exist on the badge and the service is enabled, **When** the user
   opens the badge drive on the computer, **Then** those files are visible and readable.
2. **Given** a file is read from the badge drive, **When** it is compared to the
   on-badge copy, **Then** the contents are identical (no truncation or corruption).

---

### User Story 3 - Storage stays private unless explicitly enabled (Priority: P1)

A security-conscious user expects the badge not to expose its storage to any computer
it is plugged into. The service is off by default; the badge presents no drive until
the user deliberately turns it on, exactly like the other optional USB services. Turning
it off again removes the drive.

**Why this priority**: This is a hardware security key. Exposing storage by default
would be a security regression, so the default-off behaviour is as critical as the
transfer itself.

**Independent Test**: On a freshly provisioned badge, connect it to a computer and
confirm no storage drive appears. Enable the service, confirm the drive appears, disable
it, and confirm the drive disappears.

**Acceptance Scenarios**:

1. **Given** a badge with default settings, **When** it is connected to a computer,
   **Then** no mass-storage drive is presented to the host.
2. **Given** the service is enabled, **When** the user disables it, **Then** the drive
   is removed from the host and the setting persists across a reboot.
3. **Given** the service was enabled and the badge is rebooted, **When** the badge
   reconnects, **Then** it comes back with the service in the user's last chosen state.

---

### Edge Cases

- **Concurrent access**: While the host has the drive mounted, the badge must not write to
  the same storage (the host is the sole writer), so simultaneous writes that could corrupt
  the filesystem cannot occur. The badge may still read; a read taken while the host is
  mid-write may show a transiently inconsistent view (a display glitch), but never corrupts
  data.
- **Unsafe removal**: The user yanks the cable (or the host crashes) mid-transfer. A
  partially written file may be incomplete, but the filesystem and all previously stored
  files must remain consistent and the badge must recover on the next boot.
- **Volume full**: The user copies more data than the storage can hold. The copy fails
  on the host side with a normal "disk full" error; no existing badge data is lost or
  corrupted.
- **System content on the volume**: The exposed volume also physically holds the badge's
  system folder (named `system`: plugin files and language overlays). It is marked
  read-only + hidden + system so standard host file managers hide it and refuse to change
  it, but this is advisory: a host can clear the attributes or write raw blocks. The badge
  must tolerate a host that does so, and the default partition image can be re-flashed to
  restore the system content. (See Assumptions.)
- **Toggling off while mounted**: The user disables the service or unplugs while the host
  still has the drive open. The drive disappears from the host; the badge regains
  exclusive access and the filesystem stays consistent.
- **Incompatible file names**: The host enforces standard FAT file-name rules; names the
  filesystem cannot represent are rejected by the host, not silently corrupted.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide a USB Mass Storage service that exposes the badge's
  vFAT user storage to a connected computer as a standard removable drive, requiring no
  host-side drivers or companion application.
- **FR-002**: The service MUST be disabled by default and active only after the user
  explicitly enables it, consistent with the other optional USB services.
- **FR-003**: Users MUST be able to enable and disable the service from the badge's
  module settings, and the chosen state MUST persist across reboots.
- **FR-004**: When the service is enabled and the badge is connected, the host MUST be
  able to read the existing files on the exposed storage.
- **FR-005**: When the service is enabled and the badge is connected, the host MUST be
  able to create, write, and delete files on the exposed storage.
- **FR-006**: Files written by the host MUST appear in the badge's "Files" browser after
  the transfer completes, and MUST be usable by badge features that read that storage
  (e.g. the image and Markdown viewers).
- **FR-007**: The system MUST prevent filesystem corruption from concurrent access: while
  the host has the storage mounted over USB, the badge MUST NOT write to that storage (the
  host is the sole writer). The badge MAY retain read access (browsing, viewers); it MUST
  NOT lock itself out of its own storage.
- **FR-008**: Disabling the service or disconnecting USB MUST remove the drive from the
  host and restore the badge's own access to the storage.
- **FR-009**: The badge MUST remain operable (display, keypad, other features) while the
  service is enabled and the storage is being accessed by the host.
- **FR-010**: The drive presented to the host MUST report the storage's actual total and
  free capacity.
- **FR-011**: The service MUST coexist with the always-available USB serial console;
  enabling mass storage MUST NOT disable the serial console.
- **FR-012**: The badge SHOULD indicate when the service is active and when a host is
  connected, consistent with how the other USB services surface their status.
- **FR-013**: The system content folder (`/system` on the volume) MUST be marked so that
  standard host file managers hide it and refuse to modify or delete it (read-only, hidden,
  and system attributes). This is an advisory protection; it need not withstand a host that
  deliberately clears the attributes or writes raw blocks.

### Key Entities *(include if feature involves data)*

- **USB Mass Storage service**: The optional, user-toggleable capability that, when on,
  publishes the storage to the host. Has an enabled/disabled state persisted on the badge.
- **vFAT user storage volume**: The existing on-badge file area that backs the "Files"
  menu. It is what the host sees as the removable drive. Its root also contains the `system`
  folder (`/system`: plugin files, language overlays), which is attribute-protected and
  hidden from the badge's own user view.
- **User file**: An individual file on the volume (e.g. an image or Markdown document)
  that is transferred between host and badge and may be opened by badge features.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With the service enabled, a connected badge appears as a removable drive on
  a standard Windows, macOS, or Linux computer within 5 seconds, without installing any
  driver.
- **SC-002**: A user can copy a file from the computer onto the badge and see it in the
  Files menu end-to-end in under 1 minute.
- **SC-003**: With the service disabled (the default), connecting the badge presents no
  storage drive to the host in 100% of connections.
- **SC-004**: In 100% of normal transfers (within capacity, with a safe eject), no
  existing badge data is lost and the filesystem remains consistent.
- **SC-005**: Files transferred in either direction are byte-identical when read back, with
  zero truncation or corruption.
- **SC-006**: The USB serial console remains usable in 100% of sessions where the storage
  drive is simultaneously mounted by the host.
- **SC-007**: The enabled/disabled state chosen by the user is preserved across 100% of
  reboots.

## Assumptions

- **Exposed storage**: The service exposes the badge's existing vFAT volume in full (the
  same volume that backs the "Files" menu), not a new dedicated partition. On that volume
  the system content lives in a root folder named `system` (appearing to the host as
  `/system`); `/vfat` is only the badge's internal mount point and is not a folder on the
  volume. The `system` folder is marked read-only + hidden + system so standard host file
  managers hide it and refuse to modify it, but this is advisory, not a hard boundary, and
  re-flashing the default storage image restores system content. An isolated user-only
  volume (a partition-layout change) was considered and rejected in favour of exposing the
  whole volume.
- **Direction**: Transfer is read-write, since the stated goal is to put files onto the
  badge; reading files off the badge falls out of the same capability.
- **Activation model**: Enable/disable follows the established optional-USB-service pattern:
  default-off in the module defaults, toggled from the badge's module settings, and
  persisted on the badge, identical to the existing USB HID and OTP HID services.
- **Concurrency model**: The badge does not lock itself out while the host has the volume
  mounted. Because the badge only reads this volume in normal operation, the host is the
  sole writer and there is no write conflict; the badge keeps read access (Files browser,
  viewers) and simply does not write to the volume while a host is connected. A read taken
  while the host is mid-write may be transiently inconsistent (display glitch, not corruption).
- **Transport**: Transfer happens over USB only, through the computer's normal file manager;
  no companion app, web tool, or BLE path is part of this feature.
- **Capacity**: The exposed capacity is bounded by the existing vFAT user partition; this
  feature does not enlarge it.
- **File naming**: Standard FAT short/long file-name rules apply and are enforced by the host.

## Dependencies

- The existing vFAT user storage and "Files" browser (mod_vfat; spec 018 "vfat-user-area").
- The existing USB stack that already provides the serial console and composes optional USB
  functions (the pattern used by the USB HID services).
- The module registration and persisted enable/disable framework used by all optional
  modules.
