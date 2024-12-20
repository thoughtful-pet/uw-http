#include <uw.h>

#include "uw_http.h"

// probably should be session properties:
static bool debug = false;
//static int max_connections = 5;  //100;
//static int max_host_connections = 5;  //10;

static char* _http_headers[] = {
    // from Tor browser:
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0",
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/png,image/svg+xml,*/*;q=0.8",
    "Accept-Language: en-US,en;q=0.5",
    "Accept-Encoding: gzip, deflate, br, zstd",
    "Sec-GPC: 1",
    "Upgrade-Insecure-Requests: 1",
    "Connection: keep-alive",
    "Sec-Fetch-Dest: document",
    "Sec-Fetch-Mode: navigate",
    "Sec-Fetch-Site: none",
    "Sec-Fetch-User: ?1",
    "Priority: u=0, i"
};

/****************************************************************
 * HTTP request
 */

static void fini_http_request(UwValuePtr self)
/*
 * Basic UW interface method
 */
{
    HttpRequestData* req = (HttpRequestData*) self->extra_data;

    uw_destroy(&req->url);
    uw_destroy(&req->proxy);
    uw_destroy(&req->real_url);
    uw_destroy(&req->media_type);
    uw_destroy(&req->media_subtype);
    uw_destroy(&req->media_type_params);
    uw_destroy(&req->disposition_type);
    uw_destroy(&req->disposition_params);
    uw_destroy(&req->content);

    if (req->headers) {
        curl_slist_free_all(req->headers);
        req->headers = nullptr;
    }

    if (req->easy_handle) {
        curl_easy_cleanup(req->easy_handle);
        req->easy_handle = nullptr;
    }
}

static UwResult init_http_request(UwValuePtr self, va_list ap)
/*
 * Basic UW interface method
 * Initialize request structure and create CURL easy handle
 */
{
    HttpRequestData* req = (HttpRequestData*) self->extra_data;

    req->url     = UwString();
    req->proxy   = UwString();
    req->media_type    = UwString();
    req->media_subtype = UwString();
    req->media_type_params = UwMap();
    //req->content_encoding_is_utf8 = false;
    req->status  = 0;
    req->real_url = uw_clone(&req->url);

    req->easy_handle = curl_easy_init();
    if (!req->easy_handle) {
        fprintf(stderr, "Cannot make CURL handle\n");
        fini_http_request(self);
        return UwOOM();  // XXX
    }
    // set self as private data for easy_handle
    UwValuePtr self_ptr = _uw_default_allocator.alloc(sizeof(_UwValue));
    *self_ptr = uw_clone(self);
    curl_easy_setopt(req->easy_handle, CURLOPT_PRIVATE, self_ptr);

    // make headers
    for (size_t i = 0; i < sizeof(_http_headers)/sizeof(_http_headers[0]); i++) {
        struct curl_slist* temp = curl_slist_append(req->headers, _http_headers[i]);
        if (!temp) {
            fprintf(stderr, "Cannot make headers\n");
            fini_http_request(self);
            return UwOOM();
        }
        req->headers = temp;
    }
    curl_easy_setopt(req->easy_handle, CURLOPT_HTTPHEADER, req->headers);

    // other essentials
    curl_easy_setopt(req->easy_handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br, zstd");
    curl_easy_setopt(req->easy_handle, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");

    curl_easy_setopt(req->easy_handle, CURLOPT_TIMEOUT, 1200L);
    curl_easy_setopt(req->easy_handle, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(req->easy_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    curl_easy_setopt(req->easy_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(req->easy_handle, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(req->easy_handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(req->easy_handle, CURLOPT_AUTOREFERER, 1L);

    if (debug) {
        curl_easy_setopt(req->easy_handle, CURLOPT_VERBOSE, 1L);
    }

    // set write function
    UwInterface_Curl* iface = uw_get_interface(self, Curl);

    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEFUNCTION, iface->write_data);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEDATA, self_ptr);

    // python leftovers to do someday:
    //
    // if method == 'POST':
    //     if form_data is not None:
    //         post_data = urlencode(form_data)
    //     c.setopt(c.POSTFIELDS, post_data)

    return UwOK();
}

static size_t request_write_data(void* data, size_t always_1, size_t size, UwValuePtr self)
{
    HttpRequestData* req = (HttpRequestData*) self->extra_data;

    if (uw_is_null(&req->content)) {
        http_request_parse_headers(req);

        curl_off_t content_length;
        CURLcode res = curl_easy_getinfo(req->easy_handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
        if (res != CURLE_OK || content_length < 0) {
            content_length = 0;
        }
        req->content = uw_create_empty_string(content_length, 1);
        if (uw_error(&req->content)) {
            return 0;
        }
    }
    if (!size) {
        return 0;
    }
    if (!uw_string_append_buffer(&req->content, (uint8_t*) data, size)) {
        return 0;
    }
    return size;
}

static void request_complete(UwValuePtr self)
{
    HttpRequestData* req = (HttpRequestData*) self->extra_data;

    if (uw_is_null(&req->content)) {
        http_request_parse_headers(req);
    }
}

void http_request_set_url(UwValuePtr request, UwValuePtr url)
{
    HttpRequestData* req = (HttpRequestData*) request->extra_data;

    UW_CSTRING_LOCAL(url_cstr, url);
    curl_easy_setopt(req->easy_handle, CURLOPT_URL, url_cstr);
    uw_destroy(&req->url);
    req->url = uw_clone(url);
}

void http_request_set_proxy(UwValuePtr request, UwValuePtr proxy)
{
    if (!uw_is_string(proxy)) {
        return;
    }

    HttpRequestData* req = (HttpRequestData*) request->extra_data;

    UW_CSTRING_LOCAL(proxy_cstr, proxy);
    curl_easy_setopt(req->easy_handle, CURLOPT_PROXY, proxy_cstr);
    uw_destroy(&req->proxy);
    req->proxy = uw_clone(proxy);
}

void http_request_set_cookie(UwValuePtr request, UwValuePtr cookie)
{
    if (!uw_is_string(cookie)) {
        return;
    }

    HttpRequestData* req = (HttpRequestData*) request->extra_data;

    UW_CSTRING_LOCAL(cookie_cstr, cookie);
    curl_easy_setopt(req->easy_handle, CURLOPT_COOKIE, cookie_cstr);
}

void http_request_set_resume(UwValuePtr request, size_t pos)
{
    if (pos == 0) {
        return;
    }

    HttpRequestData* req = (HttpRequestData*) request->extra_data;

    curl_easy_setopt(req->easy_handle, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) pos);
}

void http_update_status(UwValuePtr request)
{
    HttpRequestData* req = (HttpRequestData*) request->extra_data;

    long status;
    CURLcode err = curl_easy_getinfo(req->easy_handle, CURLINFO_RESPONSE_CODE, &status);
    if (err) {
        fprintf(stderr, "Error: %s\n", curl_easy_strerror(err));
    } else {
        req->status = (unsigned int) status;
    }
}

/****************************************************************
 * Global initialization
 */

UwTypeId  UwTypeId_HttpRequest = 0;
int       UwInterfaceId_Curl   = -1;

static UwType http_request_type;

static UwInterface_Curl curl_interface = {
    .write_data = request_write_data,
    .complete   = request_complete
};

void init_http()
{
    // init CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // create HTTP request subtype
    UwTypeId_HttpRequest = uw_subtype(
        &http_request_type, "HTTPRequest",
        UwTypeId_Struct, sizeof(HttpRequestData)
    );
    if (UwTypeId_HttpRequest == UwTypeId_Null) {
        fprintf(stderr, "Failed creating HTTP request subtype\n");
        exit(1);
    }
    http_request_type._init = init_http_request;
    http_request_type._fini = fini_http_request;

    // create Curl interface
    UwInterfaceId_Curl = uw_register_interface();
    if (UwInterfaceId_Curl == -1) {
        fprintf(stderr, "Failed registering CURL interface\n");
        exit(1);
    }
    http_request_type.interfaces[UwInterfaceId_Curl] = &curl_interface;
}

void cleanup_http()
{
    curl_global_cleanup();
}

/****************************************************************
 * CURL sessions and runner
 */

void* create_http_session()
{
    CURLM* multi_handle = curl_multi_init();

#   ifdef CURLPIPE_MULTIPLEX
        // enables http/2
        curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#   endif

    return (void*) multi_handle;
}

void delete_http_session(void* session)
{
    CURLM* multi_handle = (CURLM*) session;

    CURLMcode err = curl_multi_cleanup(multi_handle);
    if (err) {
        fprintf(stderr, "ERROR %s: %s\n", __func__, curl_multi_strerror(err));
    }
}

bool add_http_request(void* session, UwValuePtr request)
{
    CURLM* multi_handle = (CURLM*) session;
    HttpRequestData* req = (HttpRequestData*) request->extra_data;

    CURLMcode err = curl_multi_add_handle(multi_handle, req->easy_handle);
    if (err) {
        fprintf(stderr, "ERROR: %s\n", curl_multi_strerror(err));
        return false;
    } else {
        return true;
    }
}

static void check_transfers(CURLM* multi_handle)
{
    for(;;) {
        // check transfers
        int msgs_left;
        CURLMsg *m = curl_multi_info_read(multi_handle, &msgs_left);
        if (!m) {
            break;
        }
        if(m->msg != CURLMSG_DONE) {
            continue;
        }

        UwValuePtr request = nullptr;
        CURLcode err = curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char**) &request);
        if (err) {
            fprintf(stderr, "FATAL: %s\n", curl_easy_strerror(err));
            exit(0);
        }
        curl_easy_setopt(m->easy_handle, CURLOPT_PRIVATE, nullptr);

        HttpRequestData* req = (HttpRequestData*) request->extra_data;

        if(m->data.result != CURLE_OK) {
            UW_CSTRING_LOCAL(url_cstr, &req->url);
            fprintf(stderr, "FAILED %s: %s\n", url_cstr, curl_easy_strerror(m->data.result));
        } else {
            // get real URL
            char* url = nullptr;
            curl_easy_getinfo(req->easy_handle, CURLINFO_EFFECTIVE_URL, &url);
            if (url) {
                uw_destroy(&req->real_url);
                req->real_url = uw_create_string_cstr(url);
            }
            // get response status
            http_update_status(request);
            {
                UW_CSTRING_LOCAL(url_cstr, &req->url);
                fprintf(stderr, "STATUS %u: %s\n", req->status, url_cstr);
            }

            UwInterface_Curl* iface = uw_get_interface(request, Curl);
            iface->complete(request);
        }
        curl_multi_remove_handle(multi_handle, req->easy_handle);
        uw_destroy(request);
        _uw_default_allocator.free(request, sizeof(_UwValue));
    }
}

bool http_perform(void* session, int* running_transfers)
{
    CURLM* multi_handle = (CURLM*) session;
    CURLMcode err;

    err = curl_multi_perform(multi_handle, running_transfers);
    if (err) {
        fprintf(stderr, "FATAL %s:%s:%d: %s\n", __FILE__, __func__, __LINE__, curl_multi_strerror(err));
        return false;
    }
    if (!*running_transfers) {
        // handles for completed requests do not appear here,
        // check them before exiting:
        check_transfers(multi_handle);
        return true;
    }

    // wait for something to happen
    err = curl_multi_wait(multi_handle, NULL, 0, 1000, NULL);
    if (err) {
        fprintf(stderr, "FATAL %s:%s:%d: %s\n", __FILE__, __func__, __LINE__, curl_multi_strerror(err));
        return false;
    }

    check_transfers(multi_handle);
    return true;
}
