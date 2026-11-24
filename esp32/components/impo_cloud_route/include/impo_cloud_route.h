// SPDX-License-Identifier: Apache-2.0
//
// Where a gadget reaches Impo's gateway from.
//
// Gadgets reach Impo at gadgets.impo.ai (Cloudflare). From mainland China that
// path loses many connections, so a gadget there opens its connections to the
// relay at CONFIG_IMPO_CHINA_RELAY_HOST:CONFIG_IMPO_CHINA_RELAY_PORT instead.
// The relay copies the bytes of any TLS connection that names gadgets.impo.ai
// (SNI) to gadgets.impo.ai:443 and never decrypts them: the TLS session, its
// certificate check and every header stay between the gadget and Cloudflare.
// So a caller keeps gadgets.impo.ai as the TLS name (esp-tls / esp_http_client
// common_name) and as the Host header; only the address it dials changes.
//
// The gadget asks the relay's GET /where once per Wi-Fi address
// ({"country":"CN"} or {"country":"other"}); when that fails it uses
// gadgets.impo.ai and asks again five minutes later. Three failed connections
// to the relay in a row send the gadget to gadgets.impo.ai for ten minutes.
// Any other gateway host (a developer's own) is always used as given.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMPO_CLOUD_HOST "gadgets.impo.ai"
#define IMPO_CLOUD_BASE_URL "https://" IMPO_CLOUD_HOST

// The host and port to dial for a configured gateway host (empty means
// gadgets.impo.ai). The TLS name and Host header stay `host`. May block for
// the /where lookup, at most about 5 s.
void impo_cloud_route_endpoint(const char *host, char *out_host, size_t cap, int *out_port);

// The URL to fetch for `url`: an https://gadgets.impo.ai URL goes to the relay
// when the gadget is in mainland China; any other URL is copied unchanged.
void impo_cloud_route_url(const char *url, char *out, size_t cap);

// The TLS name (common_name) and Host header for a URL or host that the two
// calls above returned: IMPO_CLOUD_HOST when it is the relay, which passes the
// TLS through to gadgets.impo.ai; NULL when it is the server itself.
const char *impo_cloud_route_tls_name(const char *url_or_host);

// The result of a connection to `host` (a host name or an https:// URL); only
// connections to the relay count. A TLS session or an HTTP answer (any status)
// means the relay carried it.
void impo_cloud_route_report(const char *host, bool ok);

// A new Wi-Fi address: decide again before the next connection. Safe to call
// from the event loop; it never waits.
void impo_cloud_route_reset(void);

#ifdef __cplusplus
}
#endif
