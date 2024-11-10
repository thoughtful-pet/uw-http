#pragma once

#include <curl/curl.h>
#include <uw_c.h>

extern UwTypeId UwTypeId_HttpRequest;
/*
 * type id for http request, returned by uw_subclass
 */

extern int UwInterfaceId_Curl;
/*
 * CURL interface id for HttpRequest
 *
 * important: stick to naming conventions for uw_get_interface macro to work
 */

typedef size_t (*HttpRequestWriter)  (void* data, size_t always_1, size_t size, UwValuePtr self);
typedef void   (*HttpRequestComplete)(UwValuePtr self);

typedef struct {
    HttpRequestWriter     write_data;
    HttpRequestComplete   complete;

} UwInterface_Curl;


typedef struct {
    CURL* easy_handle;

    UwValuePtr url;
    UwValuePtr proxy;
    UwValuePtr real_url;

    // Parsed headers, call http_request_parse_headers for that.
    // Can be nullptr!
    UwValuePtr media_type;
    UwValuePtr media_subtype;
    UwValuePtr media_type_params;  // map
    UwValuePtr disposition_type;
    UwValuePtr disposition_params; // values can be strings of maps containing charset, language, and value

    // The content received by default handlers.
    // Always binary, regardless of content-type charset
    UwValuePtr content;

    struct curl_slist* headers;

    unsigned int status;

} HttpRequestData;


// global initialization
void init_http();
void cleanup_http();

// sessions
void* create_http_session();
bool add_http_request(void* session, UwValuePtr request);
void delete_http_session(void* session);

// request
void http_request_set_url(UwValuePtr request, UwValuePtr url);
void http_request_set_proxy(UwValuePtr request, UwValuePtr proxy);
void http_request_set_cookie(UwValuePtr request, UwValuePtr cookie);

void http_update_status(UwValuePtr request);

// runner
bool http_perform(void* session, int* running_transfers);

// utils
UwValuePtr urljoin_cstr(char* base_url, char* other_url);
UwValuePtr urljoin(UwValuePtr base_url, UwValuePtr other_url);

void http_request_parse_content_type(HttpRequestData* req);
void http_request_parse_content_disposition(HttpRequestData* req);
void http_request_parse_headers(HttpRequestData* req);

UwValuePtr http_request_get_filename(HttpRequestData* req);
