#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <uw.h>

#include "uw_http.h"

static char* get_response_header(CURL* easy_handle, char* name)
/*
 * Return the pointer to an internally allocated memory.
 * The header should be parsed immediately, otherwise subsequent calls to CURL API
 * will clobber it.
 */
{
    char* last_value = nullptr;
    int last_request = 0;
    int last_amount = 1;

    struct curl_header* hdr;

    // iterate over requests made to get the last header
    for (int i = 0;; i++) {
        CURLHcode res = curl_easy_header(easy_handle, name, 0, CURLH_HEADER, i, &hdr);
        if (res == CURLHE_OK) {
            last_value = hdr->value;
            last_amount = hdr->amount;
            last_request = i;
        } else {
            break;
        }
    }
    if (last_value == nullptr) {
        return nullptr;
    }
    // get last instance of header
    CURLHcode res = curl_easy_header(easy_handle, name, last_amount - 1, CURLH_HEADER, last_request, &hdr);
    if (res == CURLHE_OK) {
        return hdr->value;
    } else {
        return last_value;
    }
}

static inline bool is_ctl(unsigned char c)
/*
 * https://datatracker.ietf.org/doc/html/rfc2616#section-2.2
 *
 * CTL = <any US-ASCII control character
 *       (octets 0 - 31) and DEL (127)>
 */
{
    return (0 <= c && c <= 31) || c == 127;
}

static inline bool is_separator(unsigned char c)
/*
 * https://datatracker.ietf.org/doc/html/rfc2616#section-2.2
 *
 * separators = "(" | ")" | "<" | ">" | "@"
 *            | "," | ";" | ":" | "\" | <">
 *            | "/" | "[" | "]" | "?" | "="
 *            | "{" | "}" | SP  | HT
 */
{
    switch (c) {
        case '(':  case ')':  case '<':  case '>':  case '@':
        case ',':  case ';':  case ':':  case '\\': case '"':
        case '/':  case '[':  case ']':  case '?':  case '=':
        case '{':  case '}':  case ' ':  case '\t':
            return true;
        default:
            return false;
    }
}

static inline void skip_lwsp(char** current_char)
/*
 * WSP = SP / HTAB
 * LWSP = *(WSP / CRLF WSP)
 */
{
    // simplified, not strictly follows the grammar
    char* ptr = *current_char;
    for (;;) {
        char c = *ptr;
        if (c == 0) {
            break;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        ptr++;
    }
    *current_char = ptr;
}

static UwResult parse_token(char** current_char)
/*
 * https://datatracker.ietf.org/doc/html/rfc2616#section-2.2
 *
 * token = 1*<any CHAR except CTLs or separators>
 *
 * Return token as string or error.
 */
{
    char* token_start = *current_char;
    char* token_end = token_start;

    while (!(is_separator(*token_end) || is_ctl(*token_end))) {
        token_end++;
    }
    UwValue token = uw_create("");
    if (uw_ok(&token)) {
        size_t token_length = token_end - token_start;
        if (token_length) {
            if (!uw_string_append_substring_cstr(&token, token_start, 0, token_length)) {
                return UwOOM();
            }
        }
    }
    *current_char = token_end;
    return uw_move(&token);
}

static UwResult parse_quoted_string(char** current_char)
/*
 * https://datatracker.ietf.org/doc/html/rfc7230#section-3.2.6
 *
 * quoted-string = DQUOTE *( qdtext / quoted-pair ) DQUOTE
 * qdtext        = HTAB / SP / %x21 / %x23-5B / %x5D-7E / obs-text
 * obs-text      = %x80-FF
 * quoted-pair   = "\" ( HTAB / SP / VCHAR / obs-text )
 *
 * Return string, null value if not a quoted string, or OOM error.
 */
{
    char* qstr_start = *current_char;

    UWDECL_String(result);

    if (*qstr_start != '"') {
        return UwNull();
    }
    qstr_start++;

    char* qstr_end = qstr_start;
    size_t qstr_length = 0;
    for (;;) {
        unsigned char c = *qstr_end;
        if (is_ctl(c)) {
            if (c != ' ' && c != '\t') {
                break;
            }
        }
        if (c == '"') {
            break;
        }
        if (c != '\\') {
            qstr_end++;
            continue;
        }
        // append what we've got and skip quote char
        qstr_length = qstr_end - qstr_start;
        if (qstr_length) {
            if (!uw_string_append_substring_cstr(&result, qstr_start, 0, qstr_length)) {
                return UwOOM();
            }
        }
        qstr_end++;
        qstr_start = qstr_end;
    }
    if (*qstr_end != '"') {
        // strict parsing, ignore malformed string
        if (!uw_string_truncate(&result, 0)) {
            return UwOOM();
        }
    } else {
        qstr_length = qstr_end - qstr_start;
        if (qstr_length) {
            if (!uw_string_append_substring_cstr(&result, qstr_start, 0, qstr_length)) {
                return UwOOM();
            }
        }
        qstr_end++;  // skip closing quote
    }
    *current_char = qstr_end;
    return uw_move(&result);
}

static inline bool is_mime_charsetc(char c)
/*
 * mime-charsetc = ALPHA / DIGIT
 *                 / "!" / "#" / "$" / "%" / "&"
 *                 / "+" / "-" / "^" / "_" / "`"
 *                 / "{" / "}" / "~"
 *                 ; as <mime-charset> in Section 2.3 of [RFC2978]
 *                 ; except that the single quote is not included
 *                 ; SHOULD be registered in the IANA charset registry
 */
{
    if (isalnum(c)) {
        return true;
    }
    switch (c) {
        case '!':  case '#':  case '$':  case '%':  case '&':
        case '+':  case '-':  case '^':  case '_':  case '`':
        case '{':  case '}':  case '~':
            return true;
        default:
            return false;
    }
}

static inline char xdigit_to_num(char** current_char)
{
    char c = **current_char;

    if (!isxdigit(c)) {
        return 0;
    }
    (*current_char)++;
    if (isdigit(c)) {
        return c - '0';
    } else if (islower(c)) {
        return 10 + c - 'a';
    } else {
        return 10 + c - 'A';
    }
}

static inline unsigned char parse_value_char(char** current_char)
/*
 * value-chars = *( pct-encoded / attr-char )
 *
 * pct-encoded = "%" HEXDIG HEXDIG
 *               ; see [RFC3986], Section 2.1
 *
 * attr-char   = ALPHA / DIGIT
 *               / "!" / "#" / "$" / "&" / "+" / "-" / "."
 *               / "^" / "_" / "`" / "|" / "~"
 *               ; token except ( "*" / "'" / "%" )
 */
{
    unsigned char c = **current_char;

    if (isalnum(c)) {
        (*current_char)++;
        return c;
    }
    switch (c) {
        case '!':  case '#':  case '$':  case '&':  case '+':  case '-':  case '.':
        case '^':  case '_':  case '`':  case '|':  case '~':
            (*current_char)++;
            return c;
        case '%':
            break;
        default:
            return 0;
    }
    // pct-encoded
    (*current_char)++;
    char high_nibble = xdigit_to_num(current_char);
    if (high_nibble == 0) {
        return 0;
    }
    c = xdigit_to_num(current_char);
    if (c == 0) {
        return 0;
    }
    return (high_nibble << 4) | c;
}

static UwResult parse_ext_value(char** current_char)
/*
 * current_char must point to the first non-space character
 *
 * ext-value           = charset  "'" [ language ] "'" value-chars
 *
 * charset             = "UTF-8" / "ISO-8859-1" / mime-charset
 *
 * mime-charset        = 1*mime-charsetc
 *
 * language            = <Language-Tag, defined in [RFC5646], Section 2.1>
 *
 * Return string, null value if not a quoted string, or OOM error.
 */
{
    char* charset_ptr = *current_char;
    char* language_ptr = *current_char;

    for (;;) {
        if (!is_mime_charsetc(*charset_ptr)) {
            break;
        }
        charset_ptr++;
    }

    *current_char = language_ptr;
    if (*language_ptr != '\'') {
        // malformed ext-value
        return UwNull();
    };

    *language_ptr++ = 0;  // terminate charset part

    char* value_ptr = language_ptr;

    // get language tag by simply searching closing single quote
    for (;;) {
        char c = *value_ptr;
        if (c == '\'' || c == 0) {
            break;
        }
        value_ptr++;
    }
    *current_char = value_ptr;

    if (*value_ptr != '\'') {
        // malformed ext-value
        return UwNull();
    }
    *value_ptr++ = 0;  // terminate language part
    *current_char = value_ptr;

    UwValue value = uw_create_empty_string(strlen(value_ptr) + 1, 1);
    if (uw_error(&value)) {
        return uw_move(&value);
    }

    for (;;) {
        unsigned char c = parse_value_char(current_char);
        if (c == 0) {
            break;
        }
        if (!uw_string_append_char(&value, c)) {
            return UwOOM();
        }
    }

    UwValue charset = uw_create_string_cstr(charset_ptr);
    if (uw_error(&charset)) {
        return uw_move(&charset);
    }

    UwValue language = uw_create_string_cstr(language_ptr);
    if (uw_error(&language)) {
        return uw_move(&language);
    }

    return UwMap(
        UwCharPtr("charset"),  uw_move(&charset),
        UwCharPtr("language"), uw_move(&language),
        UwCharPtr("value"),    uw_move(&value)
    );
}

static bool parse_media_type(char** current_char, HttpRequestData* req)
/*
 * https://datatracker.ietf.org/doc/html/rfc7231#section-3.1.1.1
 *
 * media-type = type "/" subtype *( OWS ";" OWS parameter )
 * type       = token
 * subtype    = token
 *
 * parameter  = token "=" ( token / quoted-string )
 *
 * XXX: replaced OWS with LWSP
 */
{
    UwValue media_type = parse_token(current_char);
    if (uw_error(&media_type)) {
        return false;
    }
    if (**current_char == 0) {
        return false;
    }
    if (**current_char != '/') {
        return false;
    }
    (*current_char)++;

    UwValue media_subtype = parse_token(current_char);
    if (uw_error(&media_subtype)) {
        return false;
    }

    UwValue params = UwMap();
    for (;;) {
        skip_lwsp(current_char);
        if (**current_char == 0) {
            break;
        }
        if (**current_char != ';') {
            // malformed header, but we've got as most as we could, haven't we?
            break;
        }
        (*current_char)++;
        skip_lwsp(current_char);
        {
            UwValue param_name = parse_token(current_char);
            skip_lwsp(current_char);
            if (**current_char != '=') {
                break;
            }
            (*current_char)++;
            skip_lwsp(current_char);

            if (**current_char == 0) {
                break;
            }

            UwValue param_value = UwNull();
            if (**current_char == '"') {
                param_value = parse_quoted_string(current_char);
            } else {
                param_value = parse_token(current_char);
            }
            if (!uw_is_string(&param_value)) {
                break;
            }

            uw_string_lower(&param_name);
            if (!uw_map_update(&params, &param_name, &param_value)) {
                return false;
            }
        }
    }
    uw_destroy(&req->media_type);
    uw_destroy(&req->media_subtype);
    uw_destroy(&req->media_type_params);
    req->media_type        = uw_move(&media_type);
    req->media_subtype     = uw_move(&media_subtype);
    req->media_type_params = uw_move(&params);
    return true;
}

static bool parse_content_disposition(char** current_char, HttpRequestData* req)
/*
 * content-disposition = "Content-Disposition" ":"
 *                             disposition-type *( ";" disposition-parm )
 *
 * disposition-type    = "inline" | "attachment" | disp-ext-type
 *                       ; case-insensitive
 *
 * disp-ext-type       = token
 *
 * disposition-parm    = filename-parm | disp-ext-parm
 *
 * filename-parm       = "filename" "=" value
 *                     | "filename*" "=" ext-value
 *
 * disp-ext-parm       = token "=" value
 *                     | ext-token "=" ext-value
 *
 * ext-token           = <the characters in token, followed by "*">
 */
{
    UwValue disposition_type = parse_token(current_char);
    if (uw_error(&disposition_type)) {
        return false;
    }
    uw_string_lower(&disposition_type);

    UwValue params = UwMap();
    for (;;) {
        skip_lwsp(current_char);
        if (**current_char == 0) {
            break;
        }
        if (**current_char != ';') {
            // malformed header, but we've got as most as we could, haven't we?
            break;
        }
        (*current_char)++;
        skip_lwsp(current_char);
        {
            bool is_ext_value = false;

            UwValue param_name = parse_token(current_char);
            if (**current_char == '*') {
                is_ext_value = true;
                (*current_char)++;
            }
            skip_lwsp(current_char);
            if (**current_char != '=') {
                break;
            }
            (*current_char)++;
            skip_lwsp(current_char);

            if (**current_char == 0) {
                break;
            }

            UwValue param_value = UwNull();
            if (is_ext_value) {
                param_value = parse_ext_value(current_char);
            } else if (**current_char == '"') {
                param_value = parse_quoted_string(current_char);
            } else {
                param_value = parse_token(current_char);
            }
            if (!uw_is_string(&param_value)) {
                break;
            }

            uw_string_lower(&param_name);
            if (!uw_map_update(&params, &param_name, &param_value)) {
                return false;
            }
        }
    }
    uw_destroy(&req->disposition_type);
    uw_destroy(&req->disposition_params);
    req->disposition_type   = uw_move(&disposition_type);
    req->disposition_params = uw_move(&params);
    return true;
}

void http_request_parse_content_type(HttpRequestData* req)
/*
 * Parse content-type header
 */
{
    CURLcode res;
    char* content_type;
    res = curl_easy_getinfo(req->easy_handle, CURLINFO_CONTENT_TYPE, &content_type);
    if (res != CURLE_OK) {
        return;
    }
    if (!content_type) {
        return;
    }
    char* ct = content_type;
    if (!parse_media_type(&ct, req)) {
        fprintf(stderr, "WARNING: failed to parse content type %s\n", content_type);
    }
}

void http_request_parse_content_disposition(HttpRequestData* req)
/*
 * Parse content-disposition header
 */
{
    char* content_disposition = get_response_header(req->easy_handle, "Content-Disposition");
    if (!content_disposition) {
        return;
    }

    puts(content_disposition);
    char* p = content_disposition;
    if (!parse_content_disposition(&p, req)) {
        fprintf(stderr, "WARNING: failed to parse content dispostion %s\n", content_disposition);
    }
}

void http_request_parse_headers(HttpRequestData* req)
{
    http_request_parse_content_type(req);
    http_request_parse_content_disposition(req);
}

UwResult http_request_get_filename(HttpRequestData* req)
/*
 * Get file name from the following sources:
 *   - Content-Disposition
 *   - Location
 *   - URL
 *
 * Return map containing filename and charset.
 *
 * If no filename found and URL ends with slash, return "index.html"
 */
{
    if (uw_is_map(&req->disposition_params)) {
        if (uw_is_string(&req->disposition_type) && uw_equal(&req->disposition_type, "attachment")) {
            UwValue filename = uw_map_get(&req->disposition_params, "filename");
            if (uw_ok(&filename)) {
                if (uw_is_map(&filename)) {
                    UwValue fname = uw_map_get(&filename, "value");
                    UwValue charset = uw_map_get(&filename, "charset");
                    return UwMap(
                        UwCharPtr("filename"), uw_move(&fname),
                        UwCharPtr("charset"),  uw_move(&charset)
                    );
                } else {
                    return UwMap(
                        UwCharPtr("filename"), uw_move(&filename),
                        UwCharPtr("charset"),  UwString()
                    );
                }
            }
        }
    }

    UwValue parts = UwNull();

    char* last_location = get_response_header(req->easy_handle, "Location");
    if (last_location) {
        UwValue location = uw_create_string_cstr(last_location);
        if (uw_error(&location)) {
            return uw_move(&location);
        }
        parts = uw_string_split_chr(&location, '/');
    } else {
        parts = uw_string_split_chr(&req->url, '/');
    }
    if (uw_error(&parts)) {
        return uw_move(&parts);
    }

    UwValue filename = uw_list_item(&parts, -1);
    if (uw_strlen(&filename) == 0) {
        if (!uw_string_append(&filename, "index.html")) {
            return UwOOM();
        }
    }
    return UwMap(
        UwCharPtr("filename"), uw_move(&filename),
        UwCharPtr("charset"),  UwString()
    );
}

UwResult urljoin_cstr(char* base_url, char* other_url)
{
    CURLU* handle = curl_url();
    if (!handle) {
        return UwOOM();
    }

    CURLUcode rc;

    rc = curl_url_set(handle, CURLUPART_URL, base_url, 0);
    if(rc) {
        fprintf(stderr, "%s URL error: %s\n", __func__, curl_url_strerror(rc));
        curl_url_cleanup(handle);
        return UwOOM();  // really?
    }
    rc = curl_url_set(handle, CURLUPART_URL, other_url, 0);
    if(rc) {
        fprintf(stderr, "%s URL error: %s\n", __func__, curl_url_strerror(rc));
        curl_url_cleanup(handle);
        return UwOOM();  // really?
    }
    char* url;
    rc = curl_url_get(handle, CURLUPART_URL, &url, 0);
    if(rc) {
        fprintf(stderr, "FATAL: %s URL error: %s\n", __func__, curl_url_strerror(rc));
        exit(1);
    }
    UwValue result = uw_create_string_cstr(url);
    curl_free(url);
    curl_url_cleanup(handle);
    return uw_move(&result);
}

UwResult urljoin(UwValuePtr base_url, UwValuePtr other_url)
{
    UW_CSTRING_LOCAL(cstr_base_url, base_url);
    UW_CSTRING_LOCAL(cstr_other_url, other_url);
    return urljoin_cstr(cstr_base_url, cstr_other_url);
}
