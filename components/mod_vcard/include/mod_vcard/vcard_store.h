#pragma once

#include <cstddef>
#include <cstdint>

#define VCARD_MAX_LEN   768
#define VCARD_MAX_CARDS 100

/**
 * \brief Structured representation of an own vCard for editor/wizard use.
 *
 * Field sizes are bounded so any single field can be edited via T9InputView
 * (which has a 128 character limit).
 */
typedef struct {
    char given_name[48];
    char family_name[48];
    char formatted_name[96];
    char organization[64];
    char title[64];
    char email[96];
    char tel_home[32];
    char tel_cell[32];
    char tel_work[32];
    char url[128];
    char impp_telegram[64];
    char impp_signal[64];
    char impp_matrix[96];
    char impp_threema[32];
    char social_profile[128];
    char note[128];
} vcard_data_t;

bool vcard_store_set_own(const char* vcard, size_t len, char* err, size_t err_len);
size_t vcard_store_get_own(char* out, size_t max_len);
size_t vcard_filter_empty_fields(char* vcard, size_t len);
bool vcard_store_has_own(void);
bool vcard_store_clear_own(void);
bool vcard_store_get_display_own(char* out, size_t max_len);
void vcard_store_init(void);
uint16_t vcard_store_count(void);
bool vcard_store_add(const char* vcard, size_t len, char* err, size_t err_len);

/**
 * \brief Reports whether an exact-text vCard is already stored.
 * \param vcard Candidate vCard text.
 * \param len Candidate length.
 * \return `true` if a stored card matches \p vcard byte for byte.
 */
bool vcard_store_contains(const char* vcard, size_t len);

bool vcard_store_delete(uint16_t slot);
size_t vcard_store_get(uint16_t slot, char* out, size_t max_len);
bool vcard_store_get_display(uint16_t slot, char* out, size_t max_len);
uint16_t vcard_store_get_sorted(uint16_t* out_slots, uint16_t max_slots);

/**
 * \brief Parses vCard 4.0 raw text into a structured vcard_data_t.
 * \param raw Null-terminated vCard text (may contain CRLF or LF line endings).
 * \param out Output struct (zeroed before parsing).
 * \return `true` if input looked like a vCard (BEGIN:VCARD seen).
 */
bool vcard_parse_to_struct(const char* raw, vcard_data_t* out);

/**
 * \brief Generates vCard 4.0 text from a structured vcard_data_t.
 *        Empty fields are omitted. FN falls back to "given family" when empty.
 * \param data Input struct.
 * \param out_buf Output buffer.
 * \param buf_len Output buffer size (should be >= VCARD_MAX_LEN + 1).
 * \return Length of generated vCard text (excluding terminator), or 0 on failure.
 */
size_t vcard_generate_from_struct(const vcard_data_t* data, char* out_buf, size_t buf_len);
