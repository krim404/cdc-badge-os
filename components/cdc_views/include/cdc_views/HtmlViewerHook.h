#pragma once

#include <cstddef>

/**
 * \file HtmlViewerHook.h
 * \brief Optional HTML-rendering hook, decoupling HTML producers from consumers.
 *
 * A provider (the browser module) registers a function that renders an HTML
 * document; consumers (e.g. the file explorer) call \ref htmlViewer to open a
 * local HTML file without depending on the provider module. When no provider is
 * registered the getter returns nullptr and the consumer falls back.
 */

namespace cdc::ui {

/// \brief Render an HTML document. \p html / \p title are UTF-8.
using HtmlViewerFn = void (*)(const char* title, const char* html, size_t len);

/// \brief Register the HTML viewer (called by the provider module at init).
void setHtmlViewer(HtmlViewerFn fn);

/// \brief Current HTML viewer, or nullptr when none is registered.
HtmlViewerFn htmlViewer();

/// \brief Open a URL in the browser (enters the browser and loads it). \p url is UTF-8.
using UrlOpenerFn = void (*)(const char* url);

/// \brief Register the URL opener (called by the browser module at init).
void setUrlOpener(UrlOpenerFn fn);

/// \brief Current URL opener, or nullptr when none is registered.
UrlOpenerFn urlOpener();

/// \brief Open a local image file by path (relative to the file explorer's
///        current directory). \p path is UTF-8.
using ImageOpenerFn = void (*)(const char* path);

/// \brief Register the local-image opener (called by the file explorer at init).
void setImageOpener(ImageOpenerFn fn);

/// \brief Current image opener, or nullptr when none is registered.
ImageOpenerFn imageOpener();

}  // namespace cdc::ui
