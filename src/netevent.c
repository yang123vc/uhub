/*
 * uhub - A tiny ADC p2p connection hub
 * Copyright (C) 2007-2009, Jan Vidar Krey
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "uhub.h"
#include "hubio.h"

/* FIXME: This should not be needed! */
extern struct hub_info* g_hub;

#ifdef DEBUG_SENDQ
void debug_sendq_send(struct user* user, int sent, int total)
{
	printf("SEND: sd=%d, %d/%d bytes\n", user->net.sd, sent, total);
	if (sent == -1)
	{
		int err = net_error();
		printf("    errno: %d - %s\n", err, net_error_string(err));
	}
}

void debug_sendq_recv(struct user* user, int received, int max, const char* buffer)
{
	printf("RECV: %d/%d bytes\n", ret, (int) len);
	if (ret == -1)
	{
		int err = net_error();
		printf("    errno: %d - %s\n", err, net_error_string(err));
	}
	else if (ret > 0)
	{
		char* data = hub_malloc_zero(ret + 1);
		memcpy(data, buf, ret);
		printf("RECV: \"%s\"\n", data);
		hub_free(data);
	}
}
#endif

int net_user_send(void* ptr, const void* buf, size_t len)
{
	struct user* user = (struct user*) ptr;
	int ret = net_send(user->net.sd, buf, len, UHUB_SEND_SIGNAL);
#ifdef DEBUG_SENDQ
	debug_sendq_send(user, ret, len);
#endif
	if (ret > 0)
	{
		user_reset_last_write(user);
	}
	else if (ret == -1 && net_error() == EWOULDBLOCK)
	{
		return -2;
	}
	else
	{
		// user->close_flag = quit_socket_error;
		return 0;
	}
	return ret;
}

#ifdef SSL_SUPPORT
int net_user_send_ssl(void* ptr, const void* buf, size_t len)
{
	struct user* user = (struct user*) ptr;
	int ret = SSL_write(user->net.ssl, buf, (int) len);
#ifdef DEBUG_SENDQ
	debug_sendq_send(user, ret, len);
#endif
	if (ret > 0)
	{
		user_reset_last_write(user);
	}
	else if (ret == -1 && net_error() == EWOULDBLOCK)
	{
		return -2;
	}
	else
	{
		// user->close_flag = quit_socket_error;
		return 0;
	}
	return ret;
}
#endif

int net_user_recv(void* ptr, void* buf, size_t len)
{
	struct user* user = (struct user*) ptr;
	int ret = net_recv(user->net.sd, buf, len, 0);
	if (ret > 0)
	{
		user_reset_last_read(user);
	}
#ifdef DEBUG_SENDQ
	debug_sendq_recv(user, ret, len, buf);
#endif
	return ret;
}


#ifdef SSL_SUPPORT
int net_user_recv_ssl(void* ptr, void* buf, size_t len)
{
	struct user* user = (struct user*) ptr;
	int ret = SSL_read(user->net.ssl, buf, len);
	if (ret > 0)
	{
		user_reset_last_read(user);
	}
#ifdef DEBUG_SENDQ
	debug_sendq_recv(user, ret, len, buf);
#endif
	return ret;
}
#endif

int handle_net_read(struct user* user)
{
	static char buf[MAX_RECV_BUF];
	struct hub_recvq* q = user->net.recv_queue;
	size_t buf_size = hub_recvq_get(q, buf, MAX_RECV_BUF);
	ssize_t size = net_user_recv(user, &buf[buf_size], MAX_RECV_BUF - buf_size);

	if (size > 0)
		buf_size += size;

	if (size == -1)
	{
		if (net_error() == EWOULDBLOCK || net_error() == EINTR)
			return 0;

		return quit_socket_error;
	}
	else if (size == 0)
	{
		return quit_disconnected;
	}
	else
	{
		char* lastPos = 0;
		char* start = buf;
		char* pos = 0;
		size_t remaining = buf_size;

		while ((pos = memchr(start, '\n', remaining)))
		{
			lastPos = pos;
			pos[0] = '\0';

#ifdef DEBUG_SENDQ
			printf("PROC: \"%s\" (%d)\n", start, (int) (pos - start));
#endif

			if (user_flag_get(user, flag_maxbuf))
			{
				user_flag_unset(user, flag_maxbuf);
			}
			else
			{
				if (g_hub->config->max_recv_buffer < (pos - start))
				{
					if (hub_handle_message(g_hub, user, start, (pos - start)) == -1)
					{
							return quit_protocol_error;
							break;
					}
				}
			}

			pos[0] = '\n'; /* FIXME: not needed */
			pos ++;
			remaining -= (pos - start);
			start = pos;
		}

		if (lastPos)
		{
			if (remaining < g_hub->config->max_recv_buffer)
			{
				hub_recvq_set(q, lastPos, remaining);
			}
			else
			{
				hub_recvq_set(q, 0, 0);
				user_flag_set(user, flag_maxbuf);
			}
		}
		else
		{
			hub_recvq_set(q, 0, 0);
		}
	}
	return 0;
}

int handle_net_write(struct user* user)
{
	for (;;)
	{
		int ret = hub_sendq_send(user->net.send_queue, net_user_send, user);
		if (ret <= 0)
			break;
	}

	if (hub_sendq_get_bytes(user->net.send_queue))
	{
		user_net_io_want_write(user);
	}
	return 0;
}

static void net_event(int fd, short ev, void *arg)
{
	struct user* user = (struct user*) arg;
	int flag_close = 0;

#ifdef DEBUG_SENDQ
	hub_log(log_trace, "net_on_read() : fd=%d, ev=%d, arg=%p", fd, (int) ev, arg);
#endif

	if (ev == EV_TIMEOUT)
	{
		if (user_is_connecting(user))
		{
			flag_close = quit_timeout;
		}
		else
		{
			// FIXME: hub is not neccesarily set!
			// hub_send_ping(hub, user);
		}
	}
	else if (ev == EV_READ)
	{
		flag_close = handle_net_read(user);
	}
	else if (ev == EV_WRITE)
	{
		flag_close = handle_net_write(user);
	}
	
	if (flag_close)
	{
		hub_disconnect_user(g_hub, user, flag_close);
		return;
	}
	
	if (user_is_logged_in(user))
	{
		if (user->net.ev_read)
		{
			struct timeval timeout = { TIMEOUT_IDLE, 0 };
			event_add(user->net.ev_read, &timeout);
		}
	}
	else if (user_is_connecting(user))
	{
		if (user->net.ev_read)
		{
			struct timeval timeout = { TIMEOUT_HANDSHAKE, 0 };
			event_add(user->net.ev_read, &timeout);
		}
	}
}


static void prepare_user_net(struct hub_info* hub, struct user* user)
{
		int fd = user->net.sd;
		struct timeval timeout = { TIMEOUT_CONNECTED, 0 };

#ifdef SET_SENDBUG
		size_t sendbuf = 0;
		size_t recvbuf = 0;

		if (net_get_recvbuf_size(fd, &recvbuf) != -1)
		{
			if (recvbuf > MAX_RECV_BUF || !recvbuf) recvbuf = MAX_RECV_BUF;
			net_set_recvbuf_size(fd, recvbuf);
		}

		if (net_get_sendbuf_size(fd, &sendbuf) != -1)
		{
			if (sendbuf > MAX_SEND_BUF || !sendbuf) sendbuf = MAX_SEND_BUF;
			net_set_sendbuf_size(fd, sendbuf);
		}
#endif

		net_set_nonblocking(fd, 1);
		net_set_nosigpipe(fd, 1);

		event_set(user->net.ev_read,  fd, EV_READ | EV_PERSIST, net_event, user);
		event_set(user->net.ev_write, fd, EV_WRITE,             net_event, user);
		event_base_set(hub->evbase, user->net.ev_read);
		event_base_set(hub->evbase, user->net.ev_write);
		event_add(user->net.ev_read,  &timeout);

}

void net_on_accept(int server_fd, short ev, void *arg)
{
	struct hub_info* hub = (struct hub_info*) arg;
	struct user* user = 0;
	struct ip_addr_encap ipaddr;
	const char* addr;
	
	for (;;)
	{
		int fd = net_accept(server_fd, &ipaddr);
		if (fd == -1)
		{
			if (net_error() == EWOULDBLOCK)
			{
				break;
			}
			else
			{
				hub_log(log_error, "Accept error: %d %s", net_error(), strerror(net_error()));
				break;
			}
		}
		
		addr = ip_convert_to_string(&ipaddr); 

		/* FIXME: Should have a plugin log this */
		hub_log(log_trace, "Got connection from %s", addr);
		
		/* FIXME: A plugin should perform this check: is IP banned? */
		if (acl_is_ip_banned(hub->acl, addr))
		{
			hub_log(log_info, "Denied      [%s] (IP banned)", addr);
			net_close(fd);
			continue;
		}
		
		user = user_create(hub, fd);
		if (!user)
		{
			hub_log(log_error, "Unable to create user after socket accepted. Out of memory?");
			net_close(fd);
			break;
		}
		
		/* Store IP address in user object */
		memcpy(&user->net.ipaddr, &ipaddr, sizeof(ipaddr));

		prepare_user_net(hub, user);
	}
}

