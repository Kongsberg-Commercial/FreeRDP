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

#include <freerdp/config.h>

#include <string.h>

#include <winpr/assert.h>
#include <winpr/crt.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/thread.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <freerdp/client/fallback.h>
#include <freerdp/settings.h>
#include <freerdp/log.h>

#define TAG CLIENT_TAG("common.fallback")

#define FALLBACK_PROBE_INTERVAL_MS 5000
#define FALLBACK_PROBE_TIMEOUT_MS 1000
/* Consecutive successful probes required, avoids flapping on an unstable primary */
#define FALLBACK_PROBE_REQUIRED_OK 3

struct rdp_client_fallback_monitor
{
	rdpContext* context;
	BOOL switchBack;
	BOOL switching;
	char* primaryHost;
	UINT16 primaryPort;
	HANDLE thread;
	HANDLE stop;
	HANDLE available;
};

static BOOL fallback_probe_tcp(const char* host, UINT16 port, UINT32 timeoutMs)
{
	char service[8] = WINPR_C_ARRAY_INIT;
	struct addrinfo hints = WINPR_C_ARRAY_INIT;
	struct addrinfo* result = nullptr;
	BOOL ok = FALSE;

	(void)_snprintf(service, sizeof(service), "%u", (unsigned)port);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, service, &hints, &result) != 0)
		return FALSE;

	for (struct addrinfo* ai = result; ai && !ok; ai = ai->ai_next)
	{
#if defined(_WIN32)
		SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == INVALID_SOCKET)
			continue;

		u_long nonBlocking = 1;
		(void)ioctlsocket(s, FIONBIO, &nonBlocking);

		if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
			ok = TRUE;
		else if (WSAGetLastError() == WSAEWOULDBLOCK)
		{
			fd_set wfds;
			fd_set efds;
			FD_ZERO(&wfds);
			FD_ZERO(&efds);
			FD_SET(s, &wfds);
			FD_SET(s, &efds);
			struct timeval tv = { (long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000) };
			if ((select(0, nullptr, &wfds, &efds, &tv) > 0) && FD_ISSET(s, &wfds))
				ok = TRUE;
		}
		closesocket(s);
#else
		const int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s < 0)
			continue;

		const int flags = fcntl(s, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(s, F_SETFL, flags | O_NONBLOCK);

		if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0)
			ok = TRUE;
		else if (errno == EINPROGRESS)
		{
			struct pollfd pfd = { .fd = s, .events = POLLOUT, .revents = 0 };
			if (poll(&pfd, 1, (int)timeoutMs) > 0)
			{
				int err = 0;
				socklen_t len = sizeof(err);
				if ((getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0) && (err == 0))
					ok = TRUE;
			}
		}
		close(s);
#endif
	}

	freeaddrinfo(result);
	return ok;
}

static DWORD WINAPI fallback_probe_thread(LPVOID arg)
{
	rdpClientFallbackMonitor* monitor = (rdpClientFallbackMonitor*)arg;
	WINPR_ASSERT(monitor);

	UINT32 okCount = 0;
	while (WaitForSingleObject(monitor->stop, FALLBACK_PROBE_INTERVAL_MS) == WAIT_TIMEOUT)
	{
		if (fallback_probe_tcp(monitor->primaryHost, monitor->primaryPort,
		                       FALLBACK_PROBE_TIMEOUT_MS))
			okCount++;
		else
			okCount = 0;

		if (okCount >= FALLBACK_PROBE_REQUIRED_OK)
		{
			WLog_INFO(TAG, "Primary server %s:%u is reachable again", monitor->primaryHost,
			          (unsigned)monitor->primaryPort);
			(void)SetEvent(monitor->available);
			break;
		}
	}
	return 0;
}

rdpClientFallbackMonitor* freerdp_client_fallback_monitor_new(rdpContext* context, BOOL switchBack)
{
	WINPR_ASSERT(context);

	rdpClientFallbackMonitor* monitor = calloc(1, sizeof(rdpClientFallbackMonitor));
	if (!monitor)
		return nullptr;

	monitor->context = context;
	monitor->switchBack = switchBack;

	const char* host = freerdp_settings_get_string(context->settings, FreeRDP_ServerHostname);
	const UINT32 port = freerdp_settings_get_uint32(context->settings, FreeRDP_ServerPort);
	if (host)
	{
		monitor->primaryHost = _strdup(host);
		if (!monitor->primaryHost)
			goto fail;
	}
	monitor->primaryPort = ((port > 0) && (port <= UINT16_MAX)) ? (UINT16)port : 3389;

	monitor->stop = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	monitor->available = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!monitor->stop || !monitor->available)
		goto fail;

	return monitor;

fail:
	freerdp_client_fallback_monitor_free(monitor);
	return nullptr;
}

void freerdp_client_fallback_monitor_free(rdpClientFallbackMonitor* monitor)
{
	if (!monitor)
		return;

	freerdp_client_fallback_monitor_stop(monitor);
	if (monitor->stop)
		(void)CloseHandle(monitor->stop);
	if (monitor->available)
		(void)CloseHandle(monitor->available);
	free(monitor->primaryHost);
	free(monitor);
}

void freerdp_client_fallback_monitor_stop(rdpClientFallbackMonitor* monitor)
{
	if (!monitor)
		return;

	if (monitor->thread)
	{
		(void)SetEvent(monitor->stop);
		(void)WaitForSingleObject(monitor->thread, INFINITE);
		(void)CloseHandle(monitor->thread);
		monitor->thread = nullptr;
	}
	if (monitor->available)
		(void)ResetEvent(monitor->available);
}

void freerdp_client_fallback_monitor_update(rdpClientFallbackMonitor* monitor)
{
	if (!monitor)
		return;

	freerdp_client_fallback_monitor_stop(monitor);

	if (!monitor->switchBack || !monitor->primaryHost)
		return;

	const rdpSettings* settings = monitor->context->settings;
	if (!freerdp_settings_get_bool(settings, FreeRDP_AutoReconnectionEnabled))
		return;

	const char* fallback = freerdp_settings_get_string(settings, FreeRDP_FallbackServerHostname);
	const char* current = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
	const UINT32 currentPort = freerdp_settings_get_uint32(settings, FreeRDP_ServerPort);
	if (!fallback || !*fallback || !current)
		return;
	if ((strcmp(current, monitor->primaryHost) == 0) && (currentPort == monitor->primaryPort))
		return;

	(void)ResetEvent(monitor->stop);
	monitor->thread = CreateThread(nullptr, 0, fallback_probe_thread, monitor, 0, nullptr);
	if (monitor->thread)
		WLog_INFO(TAG, "On fallback server, monitoring primary %s:%u", monitor->primaryHost,
		          (unsigned)monitor->primaryPort);
	else
		WLog_ERR(TAG, "Failed to start monitoring primary %s:%u", monitor->primaryHost,
		         (unsigned)monitor->primaryPort);
}

HANDLE freerdp_client_fallback_monitor_event(const rdpClientFallbackMonitor* monitor)
{
	if (!monitor || !monitor->thread)
		return nullptr;
	return monitor->available;
}

BOOL freerdp_client_fallback_monitor_primary_available(const rdpClientFallbackMonitor* monitor)
{
	if (!monitor || !monitor->thread)
		return FALSE;
	return WaitForSingleObject(monitor->available, 0) == WAIT_OBJECT_0;
}

BOOL freerdp_client_fallback_monitor_switch(rdpClientFallbackMonitor* monitor, freerdp* instance,
                                            window_events_fkt_t window_events)
{
	WINPR_ASSERT(monitor);
	WINPR_ASSERT(instance);

	freerdp_client_fallback_monitor_stop(monitor);
	WLog_INFO(TAG, "Switching back to primary server");

	monitor->switching = TRUE;
	const BOOL switched = client_auto_reconnect_ex(instance, window_events);
	monitor->switching = FALSE;

	if (!switched)
	{
		WLog_ERR(TAG, "Failed to switch back to primary server");
		return FALSE;
	}

	freerdp_client_fallback_monitor_update(monitor);
	return TRUE;
}

BOOL freerdp_client_fallback_monitor_is_switching(const rdpClientFallbackMonitor* monitor)
{
	if (!monitor)
		return FALSE;
	return monitor->switching;
}

char* freerdp_client_fallback_reconnect_text(rdpContext* context, BOOL switchingToPrimary,
                                             size_t attempt)
{
	WINPR_ASSERT(context);

	const rdpSettings* settings = context->settings;
	const char* host = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
	const UINT32 max = freerdp_settings_get_uint32(settings, FreeRDP_AutoReconnectMaxRetries);

	char* text = nullptr;
	size_t len = 0;
	(void)winpr_asprintf(&text, &len, "%s - connecting to %s (attempt %" PRIuz " of %" PRIu32 ")",
	                     switchingToPrimary ? "Primary available again" : "Connection lost",
	                     host ? host : "", attempt + 1, max);
	return text;
}

char* freerdp_client_fallback_reconnect_title(rdpContext* context, BOOL switchingToPrimary,
                                              size_t attempt)
{
	WINPR_ASSERT(context);

	const rdpSettings* settings = context->settings;
	const char* host = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
	const UINT32 max = freerdp_settings_get_uint32(settings, FreeRDP_AutoReconnectMaxRetries);
	const char* title = freerdp_settings_get_string(settings, FreeRDP_WindowTitle);

	char* base = _strdup((title && *title) ? title : "FreeRDP");
	if (!base)
		return nullptr;

	/* Drop the " - [Connected to ...]" suffix the core appends after connecting */
	char* suffix = strstr(base, " - [");
	if (suffix)
		*suffix = '\0';

	char* text = nullptr;
	size_t len = 0;
	(void)winpr_asprintf(&text, &len, "%s - [%s - connecting to %s, attempt %" PRIuz "/%" PRIu32 "]",
	                     base, switchingToPrimary ? "SWITCHING TO PRIMARY" : "DISCONNECTED",
	                     host ? host : "", attempt + 1, max);
	free(base);
	return text;
}
