/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * network send wrapper.
 *
 * Copyright (c) 2006-2009 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef CONFIG_HAVE_POLL
#	include <poll.h>
#endif

#include <glib.h>

#include "pgm/transport.h"
#include "pgm/rate_control.h"
#include "pgm/net.h"

#define NET_DEBUG

#ifndef NET_DEBUG
#	define g_trace(m,...)		while (0)
#else
#	define g_trace(m,...)		g_debug(__VA_ARGS__)
#endif


/* locked and rate regulated sendto
 *
 * on success, returns number of bytes sent.  on error, -1 is returned, and
 * errno set appropriately.
 */

gssize
_pgm_sendto (
	pgm_transport_t*	transport,
	gboolean		use_rate_limit,
	gboolean		use_router_alert,
	const void*		buf,
	gsize			len,
	int			flags,
	const struct sockaddr*	to,
	gsize			tolen
	)
{
	g_assert( transport );
	g_assert( buf );
	g_assert( len > 0 );
	g_assert( to );
	g_assert( tolen > 0 );

	GStaticMutex* mutex = use_router_alert ? &transport->send_with_router_alert_mutex : &transport->send_mutex;
	int sock = use_router_alert ? transport->send_with_router_alert_sock : transport->send_sock;

	if (use_rate_limit)
	{
		int check = _pgm_rate_check (transport->rate_control, len, flags);
		if (check < 0 && errno == EAGAIN)
		{
			return (gssize)check;
		}
	}

	g_static_mutex_lock (mutex);

	ssize_t sent = sendto (sock, buf, len, flags, to, (socklen_t)tolen);
	if (	sent < 0 &&
		errno != ENETUNREACH &&		/* Network is unreachable */
		errno != EHOSTUNREACH &&	/* No route to host */
		!( errno == EAGAIN && flags & MSG_DONTWAIT )	/* would block on non-blocking send */
	   )
	{
#ifdef CONFIG_HAVE_POLL
/* poll for cleared socket */
		struct pollfd p = {
			.fd		= transport->send_sock,
			.events		= POLLOUT,
			.revents	= 0
		};
		int ready = poll (&p, 1, 500 /* ms */);
#else
		fd_set writefds;
		FD_ZERO(&writefds);
		FD_SET(transport->send_sock, &writefds);
		struct timeval tv = {
			.tv_sec  = 0,
			.tv_usec = 500 /* ms */ * 1000
		};
		int ready = select (1, NULL, &writefds, NULL, &tv);
#endif /* CONFIG_HAVE_POLL */
		if (ready > 0)
		{
			sent = sendto (sock, buf, len, flags, to, (socklen_t)tolen);
			if ( sent < 0 )
			{
				g_warning ("sendto %s failed: %i/%s",
						inet_ntoa( ((const struct sockaddr_in*)to)->sin_addr ),
						errno,
						strerror (errno));
			}
		}
		else if (ready == 0)
		{
			g_warning ("sendto %s socket timeout.",
					 inet_ntoa( ((const struct sockaddr_in*)to)->sin_addr ));
		}
		else
		{
			g_warning ("blocked sendto %s socket failed: %i %s",
					inet_ntoa( ((const struct sockaddr_in*)to)->sin_addr ),
					errno,
					strerror (errno));
		}
	}

	g_static_mutex_unlock (mutex);

	return sent;
}

/* socket helper, for setting pipe ends non-blocking
 *
 * on success, returns 0.  on error, returns -1, and sets errno appropriately.
 */

int
_pgm_set_nonblocking (
	int		filedes[2]
	)
{
	int retval = 0;

/* set write end non-blocking */
	int fd_flags = fcntl (filedes[1], F_GETFL);
	if (fd_flags < 0) {
		retval = fd_flags;
		goto out;
	}
	retval = fcntl (filedes[1], F_SETFL, fd_flags | O_NONBLOCK);
	if (retval < 0) {
		retval = fd_flags;
		goto out;
	}
/* set read end non-blocking */
	fcntl (filedes[0], F_GETFL);
	if (fd_flags < 0) {
		retval = fd_flags;
		goto out;
	}
	retval = fcntl (filedes[0], F_SETFL, fd_flags | O_NONBLOCK);
	if (retval < 0) {
		retval = fd_flags;
		goto out;
	}

out:
	return retval;
}		

/* eof */
