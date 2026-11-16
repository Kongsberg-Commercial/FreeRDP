/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 Windows
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
#include <freerdp/client.h>
#include <freerdp/client/fallback.h>
#include "xf_reconnect.h"
#include "xf_utils.h"
#include "xf_window.h"

#define XF_RECONNECT_BANNER_HEIGHT 80

static XFontStruct* xf_reconnect_load_font(Display* display)
{
	static const char* fonts[] = { "-*-helvetica-bold-r-normal--24-*-*-*-*-*-iso8859-1",
		                           "-*-dejavu sans-bold-r-normal--24-*-*-*-*-*-*-*", "12x24",
		                           "10x20", "fixed" };

	for (size_t x = 0; x < ARRAYSIZE(fonts); x++)
	{
		XFontStruct* font = XLoadQueryFont(display, fonts[x]);
		if (font)
			return font;
	}
	return nullptr;
}

/* Black band with white text across the middle of the last session image, so the user can
 * see that the session is reconnecting without losing the last state of the screen. */
void xf_reconnect_redraw(xfContext* xfc)
{
	WINPR_ASSERT(xfc);

	if (!xfc->reconnectBanner || !xfc->display || !xfc->window || !xfc->window->handle)
		return;

	xf_lock_x11(xfc);

	const Window handle = xfc->window->handle;
	XWindowAttributes attr = WINPR_C_ARRAY_INIT;
	if (LogDynAndXGetWindowAttributes(xfc->log, xfc->display, handle, &attr) == 0)
	{
		xf_unlock_x11(xfc);
		return;
	}

	GC gc = LogDynAndXCreateGC(xfc->log, xfc->display, handle, 0, nullptr);
	if (!gc)
	{
		xf_unlock_x11(xfc);
		return;
	}

	const int screen = DefaultScreen(xfc->display);
	const int bannerHeight = MIN(XF_RECONNECT_BANNER_HEIGHT, attr.height);
	const int top = (attr.height - bannerHeight) / 2;

	LogDynAndXSetForeground(xfc->log, xfc->display, gc, BlackPixel(xfc->display, screen));
	LogDynAndXFillRectangle(xfc->log, xfc->display, handle, gc, 0, top, (unsigned)attr.width,
	                        (unsigned)bannerHeight);

	XFontStruct* font = xf_reconnect_load_font(xfc->display);
	if (font)
		XSetFont(xfc->display, gc, font->fid);

	const int len = (int)strnlen(xfc->reconnectMessage, sizeof(xfc->reconnectMessage));
	const int textWidth = font ? XTextWidth(font, xfc->reconnectMessage, len) : 0;
	const int ascent = font ? font->ascent : 0;
	const int descent = font ? font->descent : 0;
	const int x = MAX(8, (attr.width - textWidth) / 2);
	const int y = top + ((bannerHeight + ascent - descent) / 2);

	LogDynAndXSetForeground(xfc->log, xfc->display, gc, WhitePixel(xfc->display, screen));
	XDrawString(xfc->display, handle, gc, x, y, xfc->reconnectMessage, len);

	if (font)
		XFreeFont(xfc->display, font);
	LogDynAndXFreeGC(xfc->log, xfc->display, gc);
	LogDynAndXFlush(xfc->log, xfc->display);

	xf_unlock_x11(xfc);
}

SSIZE_T xf_retry_dialog(freerdp* instance, const char* what, size_t current, void* userarg)
{
	WINPR_ASSERT(instance);
	const SSIZE_T delay = client_common_retry_dialog(instance, what, current, userarg);
	xfContext* xfc = (xfContext*)instance->context;
	WINPR_ASSERT(xfc);
	/* No session window yet while the first connection is still being retried */
	if ((delay < 0) || !xfc->display || !xfc->window || strcmp(what, "connection") != 0)
		return delay;

	const BOOL switching = freerdp_client_fallback_monitor_is_switching(xfc->fallbackMonitor);

	char* text = freerdp_client_fallback_reconnect_text(instance->context, switching, current);
	if (text)
	{
		(void)snprintf(xfc->reconnectMessage, sizeof(xfc->reconnectMessage), "%s", text);
		free(text);
	}

	xf_lock_x11(xfc);
	char* title = freerdp_client_fallback_reconnect_title(instance->context, switching, current);
	xf_SetDesktopWindowTitle(xfc, title);
	free(title);
	xfc->reconnectBanner = TRUE;
	xf_unlock_x11(xfc);

	xf_reconnect_redraw(xfc);
	return delay;
}

void xf_reconnect_close(xfContext* xfc)
{
	WINPR_ASSERT(xfc);
	if (!xfc->display)
		return;

	xf_lock_x11(xfc);
	const BOOL hadBanner = xfc->reconnectBanner;
	xfc->reconnectBanner = FALSE;

	/* The core updates the title to the server the session ended up on */
	const char* title =
	    freerdp_settings_get_string(xfc->common.context.settings, FreeRDP_WindowTitle);
	if (hadBanner)
		xf_SetDesktopWindowTitle(xfc, title);

	if (hadBanner && xfc->window && xfc->image && !xfc->remote_app)
	{
		const rdpSettings* settings = xfc->common.context.settings;
		xf_draw_screen(
		    xfc, 0, 0,
		    WINPR_ASSERTING_INT_CAST(int32_t,
		                             freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth)),
		    WINPR_ASSERTING_INT_CAST(int32_t,
		                             freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)));
	}
	LogDynAndXFlush(xfc->log, xfc->display);
	xf_unlock_x11(xfc);
}
