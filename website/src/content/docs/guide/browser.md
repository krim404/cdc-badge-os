---
title: Browser
description: A minimal text browser that fetches a web page and shows its main readable content.
sidebar:
  order: 12
---

The badge includes a minimal **text browser** that fetches a web page over Wi-Fi
and shows its main readable content as scrollable text. It strips away
navigation, ads, sidebars, scripts and styling, keeping the article body. Open
it from **Tools → Browser**.

It needs an active Wi-Fi connection (see [Wi-Fi & time](/guide/wifi-time/)). When
offline, the browser shows a message pointing to the Wi-Fi settings.

## Home screen

Opening Browser shows the **bookmarks list**. The first entry is **Open
webpage**; the rest are your saved bookmarks.

- **Y** opens the selected entry.
- **Open webpage** prompts for a URL (pre-filled with `https://`) and loads it.
- **3** on a bookmark opens a menu with **Delete bookmark**.
- **N** leaves the browser.

## Reading a page

While a page is shown, the footer displays the current URL (or, when a link is
selected, that link's target).

- **2 / 8** scroll up and down.
- **4 / 6** select the previous / next interactive element: an in-page link, a
  form checkbox, or a submit control. The selection is highlighted and scrolled
  into view.
- **Y** activates the selection: it follows a link, toggles a checkbox, or
  submits a form. With nothing selected, **Y** opens the full links list.
- **N** (short) goes back to the previous page; on the first page it returns to
  the bookmarks list.
- **N** (long press) leaves the browser.
- **3** opens the context menu:
  - **Links** — the numbered list of links on the page.
  - **View source** — show the page's raw HTML, unrendered.
  - **View full page** / **View readable** — toggle the content filter: full page
    keeps every block, readable shows only the extracted main content.
  - **Bookmark** — save the current page (hidden once it is bookmarked).

In-page links are shown as underlined text (the link's label, like a web
browser). **Bold** text is rendered heavier and ~~struck-through~~ text with a
line; italics are shown as plain text. Form checkboxes render as `[ ]` / `[x]`
and the submit control as `[>]`.

To open a different page, press **N** back to the bookmarks list and choose
**Open webpage**.

## Bookmarks

Bookmarks are saved on the device and persist across reboots. Add the current
page with **Bookmark** in the page context menu; remove one with **3 → Delete
bookmark** on the home screen.

## Source selection

The browser extracts the requested page's main content by text density. When the
page advertises an **AMP** variant (a lighter version of the same article), that
variant is used instead. If little readable text can be extracted, the page's
title and description are shown as a summary card.

## Captive portal login

Many public Wi-Fi hotspots require a login on a portal page (an "I agree" or
"Connect" button) before granting internet access.

- After the badge joins a Wi-Fi network it silently checks for a captive portal.
  When one is detected and the device is unlocked, a **"Captive portal found.
  Log in now?"** prompt appears: **Y** opens the browser on the portal page,
  **N** dismisses it.
- The **Captive portal login** entry also appears on the Browser home screen
  while a portal is detected (it is hidden on normal networks).
- The portal page renders with its accept form inline. Select the terms checkbox
  with **4 / 6**, toggle it with **Y**, then select the **[>]** submit control and
  press **Y** to send the form and gain access.
- Portals that use a plain link to connect work via the in-page links.

Supported portal forms are limited to a submit button plus optional checkboxes
(terms acceptance). Portals that need typed input (voucher, e-mail) or that build
their form with JavaScript are not supported.

## Local HTML files

In the file explorer (**Files**), opening a `.html` or `.htm` file renders it with
the same reader engine: tags are stripped and the readable text is shown
scrollable. In-page links and accept-style forms are interactive, selected with
**4 / 6** and activated with **Y**, just like a remote page.

## Limitations

The browser renders text only. It does not run JavaScript or apply CSS, and it
does not handle media. Accept-style forms (checkboxes plus a submit button) are
interactive; forms that need typed input (search boxes, e-mail, passwords) are
not. Pages that build their content with JavaScript show only their summary card
or a "no readable content" notice. Non-text responses show a content-type notice;
plain-text responses are shown as-is. Very large pages are scanned up to a fixed
limit and then truncated.
