/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Client fallback server helpers
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FREERDP_CLIENT_FALLBACK_H
#define FREERDP_CLIENT_FALLBACK_H

#include <winpr/wtypes.h>

#include <freerdp/api.h>
#include <freerdp/freerdp.h>
#include <freerdp/client.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Name of the client option enabling automatic switch back to the primary server */
#define FREERDP_CLIENT_FALLBACK_SWITCHBACK_ARG "fallback-switchback"

	/**
	 * Watches the primary server while a session runs on the /fallback-server and reports when
	 * the primary is reachable again (/fallback-switchback).
	 */
	typedef struct rdp_client_fallback_monitor rdpClientFallbackMonitor;

	FREERDP_API void freerdp_client_fallback_monitor_free(rdpClientFallbackMonitor* monitor);

	/**
	 * Must be created before freerdp_connect, while ServerHostname/ServerPort still hold the
	 * primary server given with /v.
	 */
	WINPR_ATTR_MALLOC(freerdp_client_fallback_monitor_free, 1)
	WINPR_ATTR_NODISCARD
	FREERDP_API rdpClientFallbackMonitor*
	freerdp_client_fallback_monitor_new(rdpContext* context, BOOL switchBack);

	/** Starts probing the primary if the session is on the fallback, stops it otherwise */
	FREERDP_API void freerdp_client_fallback_monitor_update(rdpClientFallbackMonitor* monitor);

	FREERDP_API void freerdp_client_fallback_monitor_stop(rdpClientFallbackMonitor* monitor);

	/** Event signalled when the primary is reachable again, nullptr while not probing */
	WINPR_ATTR_NODISCARD
	FREERDP_API HANDLE
	freerdp_client_fallback_monitor_event(const rdpClientFallbackMonitor* monitor);

	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL
	freerdp_client_fallback_monitor_primary_available(const rdpClientFallbackMonitor* monitor);

	/**
	 * Disconnects from the fallback and reconnects, starting with the primary.
	 * Restarts probing afterwards if the session ended up on the fallback again.
	 */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_client_fallback_monitor_switch(rdpClientFallbackMonitor* monitor,
	                                                        freerdp* instance,
	                                                        window_events_fkt_t window_events);

	/** TRUE while freerdp_client_fallback_monitor_switch is reconnecting */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL
	freerdp_client_fallback_monitor_is_switching(const rdpClientFallbackMonitor* monitor);

	/**
	 * Text for a reconnect banner, e.g.
	 * "Connection lost - connecting to host (attempt 2 of 10)". Free with free().
	 */
	WINPR_ATTR_MALLOC(free, 1)
	WINPR_ATTR_NODISCARD
	FREERDP_API char* freerdp_client_fallback_reconnect_text(rdpContext* context,
	                                                         BOOL switchingToPrimary,
	                                                         size_t attempt);

	/**
	 * Window title while reconnecting, e.g.
	 * "Base - [DISCONNECTED - connecting to host, attempt 2/10]". Free with free().
	 */
	WINPR_ATTR_MALLOC(free, 1)
	WINPR_ATTR_NODISCARD
	FREERDP_API char* freerdp_client_fallback_reconnect_title(rdpContext* context,
	                                                          BOOL switchingToPrimary,
	                                                          size_t attempt);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_CLIENT_FALLBACK_H */
