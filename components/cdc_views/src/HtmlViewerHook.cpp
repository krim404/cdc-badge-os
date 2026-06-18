#include "cdc_views/HtmlViewerHook.h"

namespace cdc::ui {
namespace {
HtmlViewerFn  s_htmlViewer = nullptr;
UrlOpenerFn   s_urlOpener = nullptr;
ImageOpenerFn s_imageOpener = nullptr;
}

void setHtmlViewer(HtmlViewerFn fn) { s_htmlViewer = fn; }

HtmlViewerFn htmlViewer() { return s_htmlViewer; }

void setUrlOpener(UrlOpenerFn fn) { s_urlOpener = fn; }

UrlOpenerFn urlOpener() { return s_urlOpener; }

void setImageOpener(ImageOpenerFn fn) { s_imageOpener = fn; }

ImageOpenerFn imageOpener() { return s_imageOpener; }

}  // namespace cdc::ui
