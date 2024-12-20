#pragma once

#include <curl/curl.h>
#include <uw.h>

extern UwTypeId UwTypeId_HttpRequest;
/*
 * type id for http request, returned by uw_subtype
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
    _UwExtraData value_data;

    CURL* easy_handle;

    _UwValue url;
    _UwValue proxy;
    _UwValue real_url;

    // Parsed headers, call http_request_parse_headers for that.
    // Can be nullptr!
    _UwValue media_type;
    _UwValue media_subtype;
    _UwValue media_type_params;  // map
    _UwValue disposition_type;
    _UwValue disposition_params; // values can be strings of maps containing charset, language, and value

    // The content received by default handlers.
    // Always binary, regardless of content-type charset
    _UwValue content;

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
void http_request_set_resume(UwValuePtr request, size_t pos);

void http_update_status(UwValuePtr request);

// runner
bool http_perform(void* session, int* running_transfers);

// utils
UwResult urljoin_cstr(char* base_url, char* other_url);
UwResult urljoin(UwValuePtr base_url, UwValuePtr other_url);

void http_request_parse_content_type(HttpRequestData* req);
void http_request_parse_content_disposition(HttpRequestData* req);
void http_request_parse_headers(HttpRequestData* req);

UwResult http_request_get_filename(HttpRequestData* req);
