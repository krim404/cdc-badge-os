#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::ui {

/**
 * \brief Font ids used by the styled-line model.
 *
 * Values mirror \ref cdc::ui::FontId so the view can map directly, but this
 * header stays dependency-free (no GFX includes) so the parser is host-testable.
 */
namespace mdfont {
enum : uint8_t {
    Builtin = 0,
    Bold9pt = 1,
    Bold12pt = 2,
    Bold18pt = 3,
    Bold24pt = 4,
};
} // namespace mdfont

/// \brief Block role of a styled line, used by the view to apply layout/markers.
enum class MdLineKind : uint8_t {
    Paragraph,
    Heading,
    Bullet,
    Ordered,
    Code,
    Quote,
    Rule,
    Blank,
};

/**
 * \brief One rendered logical line produced by the Markdown parser.
 *
 * `text` is a non-owning span into the parsed source, so the source MUST
 * outlive the model. Inline emphasis/links inside `text` are applied by the
 * view at draw time; block-level markers are already stripped here.
 */
struct StyledLine {
    const char* text = nullptr;  ///< Span into source (not null-terminated).
    uint16_t len = 0;            ///< Span length in bytes.
    MdLineKind kind = MdLineKind::Paragraph;
    uint8_t font = mdfont::Builtin;  ///< One of \ref mdfont.
    uint8_t indent = 0;              ///< Nesting depth (view multiplies by columns).
    bool inverted = false;           ///< Inverse background (code).
};

/// \brief Sink the parser emits styled lines to (decouples model from storage).
struct StyledLineSink {
    virtual void emit(const StyledLine& line) = 0;
    virtual ~StyledLineSink() = default;
};

} // namespace cdc::ui
