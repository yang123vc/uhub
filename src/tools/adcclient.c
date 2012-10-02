/*
 * uhub - A tiny ADC p2p connection hub
 * Copyright (C) 2007-2012, Jan Vidar Krey
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

#include "tools/adcclient.h"

#define ADC_HANDSHAKE "HSUP ADBASE ADTIGR ADPING\n"
#define ADC_CID_SIZE 39
#define BIG_BUFSIZE 32768
#define TIGERSIZE 24
#define MAX_RECV_BUFFER 65536

// #define ADCC_DEBUG
// #define ADC_CLIENT_DEBUG_PROTO
enum ADC_client_state
{
	ps_none, /* Not connected */
	ps_conn, /* Connecting... */
	ps_conn_ssl, /* SSL handshake */
	ps_protocol, /* Have sent HSUP */
	ps_identify, /* Have sent BINF */
	ps_verify, /* Have sent HPAS */
	ps_normal, /* Are fully logged in */
};

enum ADC_client_flags
{
	cflag_none = 0,
	cflag_ssl = 1,
	cflag_choke = 2,
	cflag_pipe = 4,
};

struct ADC_client
{
	sid_t sid;
	enum ADC_client_state state;
	struct adc_message* info;
	struct ioq_recv* recv_queue;
	struct ioq_send* send_queue;
	adc_client_cb callback;
	size_t s_offset;
	size_t r_offset;
	size_t timeout;
	struct net_connection* con;
	struct net_timer* timer;
	struct sockaddr_in addr;
	char* hub_address;
	char* nick;
	char* desc;
	int flags;
	void* ptr;
#ifdef SSL_SUPPORT
#ifdef SSL_USE_OPENSSL
	const SSL_METHOD* ssl_method;
	SSL_CTX* ssl_ctx;
#endif /* SSL_USE_OPENSSL */
#endif /*  SSL_SUPPORT */
};


static ssize_t ADC_client_recv(struct ADC_client* client);
static void ADC_client_send_info(struct ADC_client* client);
static void ADC_client_on_connected(struct ADC_client* client);
#ifdef SSL_SUPPORT
static void ADC_client_on_connected_ssl(struct ADC_client* client);
#endif
static void ADC_client_on_disconnected(struct ADC_client* client);
static void ADC_client_on_login(struct ADC_client* client);
static int ADC_client_parse_address(struct ADC_client* client, const char* arg);
static int ADC_client_on_recv_line(struct ADC_client* client, const char* line, size_t length);
static int ADC_client_send_queue(struct ADC_client* client);

static void ADC_client_debug(struct ADC_client* client, const char* format, ...)
{
	char logmsg[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	fprintf(stdout, "* [%p] %s\n", client, logmsg);
}

#ifdef ADCC_DEBUG
#define ADC_TRACE fprintf(stderr, "TRACE: %s\n", __PRETTY_FUNCTION__)
#else
#define ADC_TRACE do { } while(0)
#endif

#ifdef ADCC_DEBUG
static const char* ADC_client_state_string[] =
{
	"ps_none",
	"ps_conn",
	"ps_conn_ssl",
	"ps_protocol",
	"ps_identify",
	"ps_verify",
	"ps_normal",
	0
};
#endif

static void ADC_client_set_state(struct ADC_client* client, enum ADC_client_state state)
{
	ADC_TRACE;
	if (client->state != state)
	{
#ifdef ADCC_DEBUG
		ADC_client_debug(client, "Set state %s (was %s)", ADC_client_state_string[(int) state], ADC_client_state_string[(int) client->state]);
#endif
		client->state = state;
	}
}


static void adc_cid_pid(struct ADC_client* client)
{
	ADC_TRACE;
	char seed[64];
	char pid[64];
	char cid[64];
	uint64_t tiger_res1[3];
	uint64_t tiger_res2[3];

	/* create cid+pid pair */
	memset(seed, 0, 64);
	snprintf(seed, 64, VERSION "%p", client);

	tiger((uint64_t*) seed, strlen(seed), tiger_res1);
	base32_encode((unsigned char*) tiger_res1, TIGERSIZE, pid);
	tiger((uint64_t*) tiger_res1, TIGERSIZE, tiger_res2);
	base32_encode((unsigned char*) tiger_res2, TIGERSIZE, cid);
	cid[ADC_CID_SIZE] = 0;
	pid[ADC_CID_SIZE] = 0;

	adc_msg_add_named_argument(client->info, ADC_INF_FLAG_PRIVATE_ID, pid);
	adc_msg_add_named_argument(client->info, ADC_INF_FLAG_CLIENT_ID, cid);
}

static void event_callback(struct net_connection* con, int events, void *arg)
{
	ADC_TRACE;
	struct ADC_client* client = (struct ADC_client*) net_con_get_ptr(con);

	switch (client->state)
	{
		case ps_conn:
			if (events == NET_EVENT_TIMEOUT)
			{
				client->callback(client, ADC_CLIENT_DISCONNECTED, 0);
				return;
			}

			if (events & NET_EVENT_WRITE)
				ADC_client_connect(client, 0);
			break;

#ifdef SSL_SUPPORT
		case ps_conn_ssl:
			if (events == NET_EVENT_TIMEOUT)
			{
				client->callback(client, ADC_CLIENT_DISCONNECTED, 0);
				return;
			}

			ADC_client_on_connected_ssl(client);
			break;
#endif

		default:
			if (events & NET_EVENT_READ)
			{
				if (ADC_client_recv(client) == -1)
				{
					ADC_client_on_disconnected(client);
				}
			}

			if (events & NET_EVENT_WRITE)
			{
				ADC_client_send_queue(client);
			}
	}
}

#define UNESCAPE_ARG(TMP, TARGET) \
		if (TMP) \
			TARGET = adc_msg_unescape(TMP); \
		else \
			TARGET = NULL; \
		hub_free(TMP);

#define UNESCAPE_ARG_X(TMP, TARGET, SIZE) \
		if (TMP) \
			adc_msg_unescape_to_target(TMP, TARGET, SIZE); \
		else \
			TARGET[0] = '\0'; \
		hub_free(TMP);

#define EXTRACT_NAMED_ARG(MSG, NAME, TARGET) \
		do { \
			char* tmp = adc_msg_get_named_argument(MSG, NAME); \
			UNESCAPE_ARG(tmp, TARGET); \
		} while (0)

#define EXTRACT_NAMED_ARG_X(MSG, NAME, TARGET, SIZE) \
		do { \
			char* tmp = adc_msg_get_named_argument(MSG, NAME); \
			UNESCAPE_ARG_X(tmp, TARGET, SIZE); \
		} while(0)

#define EXTRACT_POS_ARG(MSG, POS, TARGET) \
		do { \
			char* tmp = adc_msg_get_argument(MSG, POS); \
			UNESCAPE_ARG(tmp, TARGET); \
		} while (0)


static int ADC_client_on_recv_line(struct ADC_client* client, const char* line, size_t length)
{
	struct ADC_chat_message chat;
	struct ADC_client_callback_data data;

	ADC_TRACE;
#ifdef ADC_CLIENT_DEBUG_PROTO
	ADC_client_debug(client, "- LINE: '%s'", line);
#endif

	/* Parse message */
	struct adc_message* msg = adc_msg_parse(line, length);
	if (!msg)
	{
		ADC_client_debug(client, "WARNING: Message cannot be decoded: \"%s\"", line);
		return -1;
	}

	if (length < 4)
	{
		ADC_client_debug(client, "Unexpected response from hub: '%s'", line);
		return -1;
	}

	switch (msg->cmd)
	{
		case ADC_CMD_ISUP:
			break;

		case ADC_CMD_ISID:
			if (client->state == ps_protocol)
			{
				client->sid = string_to_sid(&line[5]);
				client->callback(client, ADC_CLIENT_LOGGING_IN, 0);
				ADC_client_set_state(client, ps_identify);
				ADC_client_send_info(client);
			}
			break;

		case ADC_CMD_BMSG:
		case ADC_CMD_EMSG:
		case ADC_CMD_DMSG:
		case ADC_CMD_IMSG:
		{
			chat.from_sid       = msg->source;
			chat.to_sid         = msg->target;
			data.chat = &chat;
			EXTRACT_POS_ARG(msg, 0, chat.message);
			chat.flags = 0;

			if (adc_msg_has_named_argument(msg, ADC_MSG_FLAG_ACTION))
				chat.flags |= chat_flags_action;

			if (adc_msg_has_named_argument(msg, ADC_MSG_FLAG_PRIVATE))
				chat.flags |= chat_flags_private;

			client->callback(client, ADC_CLIENT_MESSAGE, &data);
			hub_free(chat.message);
			break;
		}

		case ADC_CMD_IINF:
		{
			struct ADC_hub_info hubinfo;
			EXTRACT_NAMED_ARG(msg, "NI", hubinfo.name);
			EXTRACT_NAMED_ARG(msg, "DE", hubinfo.description);
			EXTRACT_NAMED_ARG(msg, "VE", hubinfo.version);

			struct ADC_client_callback_data data;
			data.hubinfo = &hubinfo;
			client->callback(client, ADC_CLIENT_HUB_INFO, &data);
			hub_free(hubinfo.name);
			hub_free(hubinfo.description);
			hub_free(hubinfo.version);
			break;
		}

		case ADC_CMD_BSCH:
		case ADC_CMD_FSCH:
		{
			client->callback(client, ADC_CLIENT_SEARCH_REQ, 0);
			break;
		}

		case ADC_CMD_BINF:
		{
			if (msg->source == client->sid)
			{
				if (client->state == ps_verify || client->state == ps_identify)
				{
					ADC_client_on_login(client);
				}
			}
			else
			{
				if (adc_msg_has_named_argument(msg, "ID"))
				{
					struct ADC_user user;
					user.sid = msg->source;
					EXTRACT_NAMED_ARG_X(msg, "NI", user.name, sizeof(user.name));
					EXTRACT_NAMED_ARG_X(msg, "DE", user.description, sizeof(user.description));
					EXTRACT_NAMED_ARG_X(msg, "VE", user.version, sizeof(user.version));
					EXTRACT_NAMED_ARG_X(msg, "ID", user.cid, sizeof(user.cid));
					EXTRACT_NAMED_ARG_X(msg, "I4", user.address, sizeof(user.address));

					struct ADC_client_callback_data data;
					data.user = &user;
					client->callback(client, ADC_CLIENT_USER_JOIN, &data);
				}
			}
		}
		break;

		case ADC_CMD_IQUI:
		{
			struct ADC_client_quit_reason reason;
			memset(&reason, 0, sizeof(reason));
			reason.sid = string_to_sid(&line[5]);

			if (adc_msg_has_named_argument(msg, ADC_QUI_FLAG_DISCONNECT))
				reason.flags |= 1;

			data.quit = &reason;
			client->callback(client, ADC_CLIENT_USER_QUIT, &data);
			break;
		}

		case ADC_CMD_ISTA:
			/*
			if (strncmp(line, "ISTA 000", 8))
			{
				ADC_client_debug(client, "status: '%s'\n", (start + 9));
			}
			*/
			break;
			
		default:
			break;
	}

	adc_msg_free(msg);
	return 0;
}

static ssize_t ADC_client_recv(struct ADC_client* client)
{
	static char buf[BIG_BUFSIZE];
	struct ioq_recv* q = client->recv_queue;
	size_t buf_size = ioq_recv_get(q, buf, BIG_BUFSIZE);
	ssize_t size;

	ADC_TRACE;

	if (client->flags & cflag_choke)
		buf_size = 0;
	size = net_con_recv(client->con, buf + buf_size, BIG_BUFSIZE - buf_size);

	if (size > 0)
		buf_size += size;

	if (size < 0)
		return -1;
	else if (size == 0)
		return 0;
	else
	{
		char* lastPos = 0;
		char* start = buf;
		char* pos = 0;
		size_t remaining = buf_size;

		while ((pos = memchr(start, '\n', remaining)))
		{
			lastPos = pos+1;
			pos[0] = '\0';

#ifdef DEBUG_SENDQ
			LOG_DUMP("PROC: \"%s\" (%d)\n", start, (int) (pos - start));
#endif

			if (client->flags & cflag_choke)
				client->flags &= ~cflag_choke;
			else
			{
				if (((pos - start) > 0) && MAX_RECV_BUFFER > (pos - start))
				{
					if (ADC_client_on_recv_line(client, start, pos - start) == -1)
						return -1;
				}
			}

			pos[0] = '\n'; /* FIXME: not needed */
			pos ++;
			remaining -= (pos - start);
			start = pos;
		}

		if (lastPos || remaining)
		{
			if (remaining < (size_t) MAX_RECV_BUFFER)
			{
				ioq_recv_set(q, lastPos ? lastPos : buf, remaining);
			}
			else
			{
				ioq_recv_set(q, 0, 0);
				client->flags |= cflag_choke;
				LOG_WARN("Received message past MAX_RECV_BUFFER (%d), dropping message.", MAX_RECV_BUFFER);
			}
		}
		else
		{
			ioq_recv_set(q, 0, 0);
		}
	}
	return 0;
}

static int ADC_client_send_queue(struct ADC_client* client)
{
	int ret = 0;
	while (ioq_send_get_bytes(client->send_queue))
	{
		ret = ioq_send_send(client->send_queue, client->con);
		if (ret <= 0)
			break;
	}

	if (ret < 0)
		return quit_socket_error;

	if (ioq_send_get_bytes(client->send_queue))
	{
		net_con_update(client->con, NET_EVENT_READ | NET_EVENT_WRITE);
	}
	else
	{
		net_con_update(client->con, NET_EVENT_READ);
	}
	return 0;
}


void ADC_client_send(struct ADC_client* client, struct adc_message* msg)
{
	ADC_TRACE;

	uhub_assert(client->con != NULL);
	uhub_assert(msg->cache && *msg->cache);

	if (ioq_send_is_empty(client->send_queue) && !(client->flags & cflag_pipe))
	{
		/* Perform oportunistic write */
		ioq_send_add(client->send_queue, msg);
		ADC_client_send_queue(client);
	}
	else
	{
		ioq_send_add(client->send_queue, msg);
		if (!(client->flags & cflag_pipe))
			net_con_update(client->con, NET_EVENT_READ | NET_EVENT_WRITE);
	}
}

void ADC_client_send_info(struct ADC_client* client)
{
	ADC_TRACE;
	client->info = adc_msg_construct_source(ADC_CMD_BINF, client->sid, 64);

	adc_msg_add_named_argument_string(client->info, ADC_INF_FLAG_NICK, client->nick);

	if (client->desc)
	{
		adc_msg_add_named_argument_string(client->info, ADC_INF_FLAG_DESCRIPTION, client->desc);
	}

	adc_msg_add_named_argument_string(client->info, ADC_INF_FLAG_USER_AGENT, PRODUCT "/" VERSION);

	adc_cid_pid(client);
	ADC_client_send(client, client->info);
}


struct ADC_client* ADC_client_create(const char* nickname, const char* description, void* ptr)
{
	ADC_TRACE;
	struct ADC_client* client = (struct ADC_client*) hub_malloc_zero(sizeof(struct ADC_client));

	int sd = net_socket_create(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sd == -1) return NULL;

	client->con = net_con_create();
#if 0
	/* FIXME */
	client->timer = 0; /* FIXME: hub_malloc(sizeof(struct net_timer)); */
#endif
	net_con_initialize(client->con, sd, event_callback, client, 0);
#if 0
	/* FIXME */
	net_timer_initialize(client->timer, timer_callback, client);
#endif
	ADC_client_set_state(client, ps_none);

	client->nick = hub_strdup(nickname);
	client->desc = hub_strdup(description);

	client->send_queue = ioq_send_create();
	client->recv_queue = ioq_recv_create();

	client->ptr = ptr;
	return client;
}

void ADC_client_destroy(struct ADC_client* client)
{
	ADC_TRACE;
	ADC_client_disconnect(client);
#if 0
	/* FIXME */
	net_timer_shutdown(client->timer);
#endif
	ioq_send_destroy(client->send_queue);
	ioq_recv_destroy(client->recv_queue);
	hub_free(client->timer);
	adc_msg_free(client->info);
	hub_free(client->nick);
	hub_free(client->desc);
	hub_free(client->hub_address);
}

int ADC_client_connect(struct ADC_client* client, const char* address)
{
	ADC_TRACE;
	if (!client->hub_address)
	{
		if (!ADC_client_parse_address(client, address))
			return 0;
	
		client->callback(client, ADC_CLIENT_CONNECTING, 0);
	}

	int ret = net_connect(net_con_get_sd(client->con), (struct sockaddr*) &client->addr, sizeof(struct sockaddr_in));
	if (ret == 0 || (ret == -1 && net_error() == EISCONN))
	{
		ADC_client_on_connected(client);
	}
	else if (ret == -1 && (net_error() == EALREADY || net_error() == EINPROGRESS || net_error() == EWOULDBLOCK || net_error() == EINTR))
	{
		net_con_update(client->con, NET_EVENT_READ | NET_EVENT_WRITE);
		ADC_client_set_state(client, ps_conn);
	}
	else
	{
		ADC_client_on_disconnected(client);
		return 0;
	}
	return 1;
}

static void ADC_client_on_connected(struct ADC_client* client)
{
	ADC_TRACE;
#ifdef SSL_SUPPORT
	if (client->flags & cflag_ssl)
	{
		net_con_update(client->con, NET_EVENT_READ | NET_EVENT_WRITE);
		client->callback(client, ADC_CLIENT_SSL_HANDSHAKE, 0);
		ADC_client_set_state(client, ps_conn_ssl);
	}
	else
#endif
	{
		net_con_update(client->con, NET_EVENT_READ);
		client->callback(client, ADC_CLIENT_CONNECTED, 0);
		ADC_client_send(client, adc_msg_create(ADC_HANDSHAKE));
		ADC_client_set_state(client, ps_protocol);
	}
}

#ifdef SSL_SUPPORT
static void ADC_client_on_connected_ssl(struct ADC_client* client)
{
	ADC_TRACE;
	net_con_update(client->con, NET_EVENT_READ);
	client->callback(client, ADC_CLIENT_SSL_OK, 0);
	client->callback(client, ADC_CLIENT_CONNECTED, 0);
	ADC_client_send(client, adc_msg_create(ADC_HANDSHAKE));
	ADC_client_set_state(client, ps_protocol);
}
#endif

static void ADC_client_on_disconnected(struct ADC_client* client)
{
	ADC_TRACE;
	net_con_close(client->con);
	client->con = 0;
	ADC_client_set_state(client, ps_none);
}

static void ADC_client_on_login(struct ADC_client* client)
{
	ADC_TRACE;
	ADC_client_set_state(client, ps_normal);
	client->callback(client, ADC_CLIENT_LOGGED_IN, 0);
}

void ADC_client_disconnect(struct ADC_client* client)
{
	ADC_TRACE;
	if (client->con && net_con_get_sd(client->con) != -1)
	{
		net_con_close(client->con);
		client->con = 0;
	}
}

static int ADC_client_parse_address(struct ADC_client* client, const char* arg)
{
	ADC_TRACE;
	char* split;
	int ssl = 0;
	struct hostent* dns;
	struct in_addr* addr;

	if (!arg)
		return 0;

	client->hub_address = hub_strdup(arg);

	/* Minimum length of a valid address */
	if (strlen(arg) < 9)
		return 0;

	/* Check for ADC or ADCS */
	if (!strncmp(arg, "adc://", 6))
	{
		client->flags &= ~cflag_ssl;
	}
	else if (!strncmp(arg, "adcs://", 7))
	{
		client->flags |= cflag_ssl;
		ssl = 1;
	}
	else
		return 0;

	/* Split hostname and port (if possible) */
	split = strrchr(client->hub_address + 6 + ssl, ':');
	if (split == 0 || strlen(split) < 2 || strlen(split) > 6)
		return 0;

	/* Ensure port number is valid */
	int port = strtol(split+1, NULL, 10);
	if (port <= 0 || port > 65535)
		return 0;

	split[0] = 0;

	/* Resolve IP address (FIXME: blocking call) */
	dns = gethostbyname(client->hub_address + 6 + ssl);
	if (dns)
	{
		addr = (struct in_addr*) dns->h_addr_list[0];
	}

	// Initialize the sockaddr struct.
	memset(&client->addr, 0, sizeof(client->addr));
	client->addr.sin_family = AF_INET;
	client->addr.sin_port   = htons(port);
	memcpy(&client->addr.sin_addr, addr, sizeof(struct in_addr));
	return 1;
}

void ADC_client_set_callback(struct ADC_client* client, adc_client_cb cb)
{
	ADC_TRACE;
	client->callback = cb;
}

sid_t ADC_client_get_sid(const struct ADC_client* client)
{
	return client->sid;
}

const char* ADC_client_get_nick(const struct ADC_client* client)
{
	return client->nick;
}

const char* ADC_client_get_description(const struct ADC_client* client)
{
	return client->desc;
}

void* ADC_client_get_ptr(const struct ADC_client* client)
{
	return client->ptr;
}