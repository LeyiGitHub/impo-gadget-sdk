// SPDX-License-Identifier: Apache-2.0
//
// Where a gadget reaches Impo's gateway from.
//
// Gadgets reach Impo at gadgets.impo.ai (Cloudflare). From mainland China that
// path loses many connections, so a gadget there goes through the relay at
// CONFIG_IMPO_CHINA_RELAY_HOST:CONFIG_IMPO_CHINA_RELAY_PORT, which forwards the
// same TLS requests and WebSockets to gadgets.impo.ai. The gadget asks the
// relay's GET /where once per Wi-Fi address ({"country":"CN"} or
// {"country":"other"}); when that fails it uses gadgets.impo.ai and asks again
// five minutes later. Three failed connections to the relay in a row send the
// gadget to gadgets.impo.ai for ten minutes. Any other gateway host (a
// developer's own) is always used as given.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMPO_CLOUD_HOST "gadgets.impo.ai"
#define IMPO_CLOUD_BASE_URL "https://" IMPO_CLOUD_HOST

// The host and port to open TLS to for a configured gateway host (empty means
// gadgets.impo.ai). May block for the /where lookup, at most about 5 s.
void impo_cloud_route_endpoint(const char *host, char *out_host, size_t cap, int *out_port);

// The URL to fetch for `url`: an https://gadgets.impo.ai URL goes to the relay
// when the gadget is in mainland China; any other URL is copied unchanged.
void impo_cloud_route_url(const char *url, char *out, size_t cap);

// The HTTP Host header for an endpoint: "host" on port 443, else "host:port".
void impo_cloud_route_host_header(const char *host, int port, char *out, size_t cap);

// The result of a connection to `host` (a host name or an https:// URL); only
// connections to the relay count. A request the relay answered 502 or 504
// (it could not reach gadgets.impo.ai) is a failed one.
void impo_cloud_route_report(const char *host, bool ok);

// Whether an HTTP status (0 = no answer) shows that the relay did its job.
bool impo_cloud_route_http_ok(int status);

// A new Wi-Fi address: decide again before the next connection. Safe to call
// from the event loop; it never waits.
void impo_cloud_route_reset(void);

#ifdef __cplusplus
}
#endif
