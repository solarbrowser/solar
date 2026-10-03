#pragma once

#include <nghttp2/nghttp2.h>

// nghttp2 1.63 renamed the calls that return a size or take a data provider, so as to name the
// signed type they use on every platform, and deprecated the old names. Older releases, such as
// the one a distribution may still ship, have only the old names, and they mean the same thing.
#if NGHTTP2_VERSION_NUM < 0x013f00
using nghttp2_ssize = ssize_t;
using nghttp2_data_provider2 = nghttp2_data_provider;
#define nghttp2_session_mem_recv2 nghttp2_session_mem_recv
#define nghttp2_session_mem_send2 nghttp2_session_mem_send
#define nghttp2_submit_request2 nghttp2_submit_request
#define nghttp2_submit_response2 nghttp2_submit_response
#endif
