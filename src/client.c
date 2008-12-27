/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "client.h"
#include "command.h"
#include "conf.h"
#include "log.h"
#include "listen.h"
#include "permission.h"
#include "utils.h"
#include "ioops.h"
#include "main_notify.h"
#include "dlist.h"
#include "idle.h"

#include "../config.h"

#include <glib.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

#define GREETING				"OK MPD " PROTOCOL_VERSION "\n"

#define CLIENT_LIST_MODE_BEGIN			"command_list_begin"
#define CLIENT_LIST_OK_MODE_BEGIN			"command_list_ok_begin"
#define CLIENT_LIST_MODE_END				"command_list_end"
#define CLIENT_TIMEOUT_DEFAULT			(60)
#define CLIENT_MAX_CONNECTIONS_DEFAULT		(10)
#define CLIENT_MAX_COMMAND_LIST_DEFAULT		(2048*1024)
#define CLIENT_MAX_OUTPUT_BUFFER_SIZE_DEFAULT	(8192*1024)

/* set this to zero to indicate we have no possible clients */
static unsigned int client_max_connections;	/*CLIENT_MAX_CONNECTIONS_DEFAULT; */
static int client_timeout = CLIENT_TIMEOUT_DEFAULT;
static size_t client_max_command_list_size =
    CLIENT_MAX_COMMAND_LIST_DEFAULT;
static size_t client_max_output_buffer_size =
    CLIENT_MAX_OUTPUT_BUFFER_SIZE_DEFAULT;

struct deferred_buffer {
	size_t size;
	char data[sizeof(long)];
};

struct client {
	struct list_head siblings;

	char buffer[4096];
	size_t bufferLength;
	size_t bufferPos;

	int fd;	/* file descriptor; -1 if expired */
	unsigned permission;

	/** the uid of the client process, or -1 if unknown */
	int uid;

	time_t lastTime;
	GSList *cmd_list;	/* for when in list mode */
	int cmd_list_OK;	/* print OK after each command execution */
	size_t cmd_list_size;	/* mem cmd_list consumes */
	GQueue *deferred_send;	/* for output if client is slow */
	size_t deferred_bytes;	/* mem deferred_send consumes */
	unsigned int num;	/* client number */

	char send_buf[4096];
	size_t send_buf_used;	/* bytes used this instance */

	/** is this client waiting for an "idle" response? */
	bool idle_waiting;

	/** idle flags pending on this client, to be sent as soon as
	    the client enters "idle" */
	unsigned idle_flags;

	/** idle flags that the client wants to receive */
	unsigned idle_subscriptions;
};

static LIST_HEAD(clients);
static unsigned num_clients;

static void client_write_deferred(struct client *client);

static void client_write_output(struct client *client);

bool client_is_expired(const struct client *client)
{
	return client->fd < 0;
}

int client_get_uid(const struct client *client)
{
	return client->uid;
}

unsigned client_get_permission(const struct client *client)
{
	return client->permission;
}

void client_set_permission(struct client *client, unsigned permission)
{
	client->permission = permission;
}

static inline void client_set_expired(struct client *client)
{
	if (client->fd >= 0) {
		xclose(client->fd);
		client->fd = -1;
	}
}

static void client_init(struct client *client, int fd)
{
	static unsigned int next_client_num;

	assert(fd >= 0);

	client->cmd_list_size = 0;
	client->cmd_list_OK = -1;
	client->bufferLength = 0;
	client->bufferPos = 0;
	client->fd = fd;
	set_nonblocking(fd);
	client->lastTime = time(NULL);
	client->cmd_list = NULL;
	client->deferred_send = g_queue_new();
	client->deferred_bytes = 0;
	client->num = next_client_num++;
	client->send_buf_used = 0;

	client->permission = getDefaultPermissions();

	xwrite(fd, GREETING, strlen(GREETING));
}

static void free_cmd_list(GSList *list)
{
	for (GSList *tmp = list; tmp != NULL; tmp = g_slist_next(tmp))
		g_free(tmp->data);

	g_slist_free(list);
}

static void new_cmd_list_ptr(struct client *client, char *s)
{
	client->cmd_list = g_slist_prepend(client->cmd_list, g_strdup(s));
}

static void
deferred_buffer_free(gpointer data, G_GNUC_UNUSED gpointer user_data)
{
	struct deferred_buffer *buffer = data;
	g_free(buffer);
}

static void client_close(struct client *client)
{
	assert(num_clients > 0);
	assert(!list_empty(&clients));
	list_del(&client->siblings);
	--num_clients;

	client_set_expired(client);

	if (client->cmd_list) {
		free_cmd_list(client->cmd_list);
		client->cmd_list = NULL;
	}

	g_queue_foreach(client->deferred_send, deferred_buffer_free, NULL);
	g_queue_free(client->deferred_send);

	SECURE("client %i: closed\n", client->num);
	free(client);
}

static const char *
sockaddr_to_tmp_string(const struct sockaddr *addr)
{
	const char *hostname;

	switch (addr->sa_family) {
#ifdef HAVE_TCP
	case AF_INET:
		hostname = (const char *)inet_ntoa(((const struct sockaddr_in *)
						    addr)->sin_addr);
		if (!hostname)
			hostname = "error getting ipv4 address";
		break;
#ifdef HAVE_IPV6
	case AF_INET6:
		{
			static char host[INET6_ADDRSTRLEN + 1];
			memset(host, 0, INET6_ADDRSTRLEN + 1);
			if (inet_ntop(AF_INET6, (const void *)
				      &(((const struct sockaddr_in6 *)addr)->
					sin6_addr), host,
				      INET6_ADDRSTRLEN)) {
				hostname = (const char *)host;
			} else {
				hostname = "error getting ipv6 address";
			}
		}
		break;
#endif
#endif /* HAVE_TCP */
#ifdef HAVE_UN
	case AF_UNIX:
		hostname = "local connection";
		break;
#endif /* HAVE_UN */
	default:
		hostname = "unknown";
	}

	return hostname;
}

void client_new(int fd, const struct sockaddr *addr, int uid)
{
	struct client *client;

	if (num_clients >= client_max_connections) {
		ERROR("Max Connections Reached!\n");
		xclose(fd);
		return;
	}

	client = xcalloc(1, sizeof(*client));
	list_add(&client->siblings, &clients);
	++num_clients;
	client_init(client, fd);
	client->uid = uid;
	SECURE("client %i: opened from %s\n", client->num,
	       sockaddr_to_tmp_string(addr));
}

static int client_process_line(struct client *client, char *line)
{
	int ret = 1;

	if (strcmp(line, "noidle") == 0) {
		if (client->idle_waiting) {
			/* send empty idle response and leave idle mode */
			client->idle_waiting = false;
			command_success(client);
			client_write_output(client);
		}

		/* do nothing if the client wasn't idling: the client
		   has already received the full idle response from
		   client_idle_notify(), which he can now evaluate */

		return 0;
	} else if (client->idle_waiting) {
		/* during idle mode, clients must not send anything
		   except "noidle" */
		ERROR("client %i: command \"%s\" during idle\n",
		      client->num, line);
		return COMMAND_RETURN_CLOSE;
	}

	if (client->cmd_list_OK >= 0) {
		if (strcmp(line, CLIENT_LIST_MODE_END) == 0) {
			DEBUG("client %i: process command "
			      "list\n", client->num);

			/* for scalability reasons, we have prepended
			   each new command; now we have to reverse it
			   to restore the correct order */
			client->cmd_list = g_slist_reverse(client->cmd_list);

			ret = command_process_list(client,
						   client->cmd_list_OK,
						   client->cmd_list);
			DEBUG("client %i: process command "
			      "list returned %i\n", client->num, ret);

			if (ret == COMMAND_RETURN_CLOSE ||
			    client_is_expired(client))
				return COMMAND_RETURN_CLOSE;

			if (ret == 0)
				command_success(client);

			client_write_output(client);
			free_cmd_list(client->cmd_list);
			client->cmd_list = NULL;
			client->cmd_list_OK = -1;
		} else {
			size_t len = strlen(line) + 1;
			client->cmd_list_size += len;
			if (client->cmd_list_size >
			    client_max_command_list_size) {
				ERROR("client %i: command "
				      "list size (%lu) is "
				      "larger than the max "
				      "(%lu)\n",
				      client->num,
				      (unsigned long)client->cmd_list_size,
				      (unsigned long)
				      client_max_command_list_size);
				return COMMAND_RETURN_CLOSE;
			} else
				new_cmd_list_ptr(client, line);
		}
	} else {
		if (strcmp(line, CLIENT_LIST_MODE_BEGIN) == 0) {
			client->cmd_list_OK = 0;
			ret = 1;
		} else if (strcmp(line, CLIENT_LIST_OK_MODE_BEGIN) == 0) {
			client->cmd_list_OK = 1;
			ret = 1;
		} else {
			DEBUG("client %i: process command \"%s\"\n",
			      client->num, line);
			ret = command_process(client, line);
			DEBUG("client %i: command returned %i\n",
			      client->num, ret);

			if (ret == COMMAND_RETURN_CLOSE ||
			    client_is_expired(client))
				return COMMAND_RETURN_CLOSE;

			if (ret == 0)
				command_success(client);

			client_write_output(client);
		}
	}

	return ret;
}

static int client_input_received(struct client *client, size_t bytesRead)
{
	char *start = client->buffer + client->bufferPos, *end;
	char *newline, *next;
	int ret;

	assert(client->bufferPos <= client->bufferLength);
	assert(client->bufferLength + bytesRead <= sizeof(client->buffer));

	client->bufferLength += bytesRead;
	end = client->buffer + client->bufferLength;

	/* process all lines */
	while ((newline = memchr(start, '\n', end - start)) != NULL) {
		next = newline + 1;

		if (newline > start && newline[-1] == '\r')
			--newline;
		*newline = 0;

		ret = client_process_line(client, start);
		if (ret == COMMAND_RETURN_KILL ||
		    ret == COMMAND_RETURN_CLOSE)
			return ret;
		if (client_is_expired(client))
			return COMMAND_RETURN_CLOSE;

		start = next;
	}

	/* mark consumed lines */
	client->bufferPos = start - client->buffer;

	/* if we're have reached the buffer's end, close the gab at
	   the beginning */
	if (client->bufferLength == sizeof(client->buffer)) {
		if (client->bufferPos == 0) {
			ERROR("client %i: buffer overflow\n",
			      client->num);
			return COMMAND_RETURN_CLOSE;
		}
		assert(client->bufferLength >= client->bufferPos
		       && "bufferLength >= bufferPos");
		client->bufferLength -= client->bufferPos;
		memmove(client->buffer,
			client->buffer + client->bufferPos,
			client->bufferLength);
		client->bufferPos = 0;
	}

	return 0;
}

static int client_read(struct client *client)
{
	ssize_t bytesRead;

	assert(client->bufferPos <= client->bufferLength);
	assert(client->bufferLength < sizeof(client->buffer));

	bytesRead = read(client->fd,
			 client->buffer + client->bufferLength,
			 sizeof(client->buffer) - client->bufferLength);

	if (bytesRead > 0)
		return client_input_received(client, bytesRead);
	else if (bytesRead < 0 && errno == EINTR)
		/* try again later, after select() */
		return 0;
	else
		/* peer disconnected or I/O error */
		return COMMAND_RETURN_CLOSE;
}

static void client_manager_register_read_fd(fd_set * fds, int *fdmax)
{
	struct client *client;

	FD_ZERO(fds);
	addListenSocketsToFdSet(fds, fdmax);

	list_for_each_entry(client, &clients, siblings) {
		if (!client_is_expired(client) &&
		    g_queue_is_empty(client->deferred_send)) {
			FD_SET(client->fd, fds);
			if (*fdmax < client->fd)
				*fdmax = client->fd;
		}
	}
}

static void client_manager_register_write_fd(fd_set * fds, int *fdmax)
{
	struct client *client;

	FD_ZERO(fds);

	list_for_each_entry(client, &clients, siblings) {
		if (client->fd >= 0 && !client_is_expired(client)
		    && !g_queue_is_empty(client->deferred_send)) {
			FD_SET(client->fd, fds);
			if (*fdmax < client->fd)
				*fdmax = client->fd;
		}
	}
}

int client_manager_io(void)
{
	fd_set rfds;
	fd_set wfds;
	fd_set efds;
	struct client *client, *n;
	int ret;
	int fdmax = 0;

	FD_ZERO( &efds );
	client_manager_register_read_fd(&rfds, &fdmax);
	client_manager_register_write_fd(&wfds, &fdmax);

	registered_IO_add_fds(&fdmax, &rfds, &wfds, &efds);

	main_notify_lock();
	ret = select(fdmax + 1, &rfds, &wfds, &efds, NULL);
	main_notify_unlock();

	if (ret < 0) {
		if (errno == EINTR)
			return 0;

		FATAL("select() failed: %s\n", strerror(errno));
	}

	registered_IO_consume_fds(&ret, &rfds, &wfds, &efds);

	getConnections(&rfds);

	list_for_each_entry_safe(client, n, &clients, siblings) {
		if (FD_ISSET(client->fd, &rfds)) {
			ret = client_read(client);
			if (ret == COMMAND_RETURN_KILL)
				return COMMAND_RETURN_KILL;
			if (ret == COMMAND_RETURN_CLOSE) {
				client_close(client);
				continue;
			}

			assert(!client_is_expired(client));

			client->lastTime = time(NULL);
		}
		if (!client_is_expired(client) &&
		    FD_ISSET(client->fd, &wfds)) {
			client_write_deferred(client);
			client->lastTime = time(NULL);
		}
	}

	return 0;
}

void client_manager_init(void)
{
	char *test;
	ConfigParam *param;

	param = getConfigParam(CONF_CONN_TIMEOUT);

	if (param) {
		client_timeout = strtol(param->value, &test, 10);
		if (*test != '\0' || client_timeout <= 0) {
			FATAL("connection timeout \"%s\" is not a positive "
			      "integer, line %i\n", CONF_CONN_TIMEOUT,
			      param->line);
		}
	}

	param = getConfigParam(CONF_MAX_CONN);

	if (param) {
		client_max_connections = strtol(param->value, &test, 10);
		if (*test != '\0' || client_max_connections <= 0) {
			FATAL("max connections \"%s\" is not a positive integer"
			      ", line %i\n", param->value, param->line);
		}
	} else
		client_max_connections = CLIENT_MAX_CONNECTIONS_DEFAULT;

	param = getConfigParam(CONF_MAX_COMMAND_LIST_SIZE);

	if (param) {
		long tmp = strtol(param->value, &test, 10);
		if (*test != '\0' || tmp <= 0) {
			FATAL("max command list size \"%s\" is not a positive "
			      "integer, line %i\n", param->value, param->line);
		}
		client_max_command_list_size = tmp * 1024;
	}

	param = getConfigParam(CONF_MAX_OUTPUT_BUFFER_SIZE);

	if (param) {
		long tmp = strtol(param->value, &test, 10);
		if (*test != '\0' || tmp <= 0) {
			FATAL("max output buffer size \"%s\" is not a positive "
			      "integer, line %i\n", param->value, param->line);
		}
		client_max_output_buffer_size = tmp * 1024;
	}
}

static void client_close_all(void)
{
	struct client *client, *n;

	list_for_each_entry_safe(client, n, &clients, siblings)
		client_close(client);
	num_clients = 0;
}

void client_manager_deinit(void)
{
	client_close_all();

	client_max_connections = 0;
}

void client_manager_expire(void)
{
	struct client *client, *n;

	list_for_each_entry_safe(client, n, &clients, siblings) {
		if (client_is_expired(client)) {
			DEBUG("client %i: expired\n", client->num);
			client_close(client);
		} else if (!client->idle_waiting && /* idle clients
						       never expire */
			   time(NULL) - client->lastTime >
			   client_timeout) {
			DEBUG("client %i: timeout\n", client->num);
			client_close(client);
		}
	}
}

static void client_write_deferred(struct client *client)
{
	ssize_t ret = 0;

	while (!g_queue_is_empty(client->deferred_send)) {
		struct deferred_buffer *buf =
			g_queue_peek_head(client->deferred_send);

		assert(buf->size > 0);
		assert(buf->size <= client->deferred_bytes);

		ret = write(client->fd, buf->data, buf->size);
		if (ret < 0)
			break;
		else if ((size_t)ret < buf->size) {
			assert(client->deferred_bytes >= (size_t)ret);
			client->deferred_bytes -= ret;
			buf->size -= ret;
			memmove(buf->data, buf->data + ret, buf->size);
			break;
		} else {
			size_t decr = sizeof(*buf) -
				sizeof(buf->data) + buf->size;

			assert(client->deferred_bytes >= decr);
			client->deferred_bytes -= decr;
			free(buf);
			g_queue_pop_head(client->deferred_send);
		}
		client->lastTime = time(NULL);
	}

	if (g_queue_is_empty(client->deferred_send)) {
		DEBUG("client %i: buffer empty %lu\n", client->num,
		      (unsigned long)client->deferred_bytes);
		assert(client->deferred_bytes == 0);
	} else if (ret < 0 && errno != EAGAIN && errno != EINTR) {
		/* cause client to close */
		DEBUG("client %i: problems flushing buffer\n",
		      client->num);
		client_set_expired(client);
	}
}

static void client_defer_output(struct client *client,
				const void *data, size_t length)
{
	size_t alloc;
	struct deferred_buffer *buf;

	assert(length > 0);

	alloc = sizeof(*buf) - sizeof(buf->data) + length;
	client->deferred_bytes += alloc;
	if (client->deferred_bytes > client_max_output_buffer_size) {
		ERROR("client %i: output buffer size (%lu) is "
		      "larger than the max (%lu)\n",
		      client->num,
		      (unsigned long)client->deferred_bytes,
		      (unsigned long)client_max_output_buffer_size);
		/* cause client to close */
		client_set_expired(client);
		return;
	}

	buf = g_malloc(alloc);
	buf->size = length;
	memcpy(buf->data, data, length);

	g_queue_push_tail(client->deferred_send, buf);
}

static void client_write_direct(struct client *client,
				const char *data, size_t length)
{
	ssize_t ret;

	assert(length > 0);
	assert(g_queue_is_empty(client->deferred_send));

	if ((ret = write(client->fd, data, length)) < 0) {
		if (errno == EAGAIN || errno == EINTR) {
			client_defer_output(client, data, length);
		} else {
			DEBUG("client %i: problems writing\n", client->num);
			client_set_expired(client);
			return;
		}
	} else if ((size_t)ret < client->send_buf_used) {
		client_defer_output(client, data + ret, length - ret);
	}

	if (!g_queue_is_empty(client->deferred_send))
		DEBUG("client %i: buffer created\n", client->num);
}

static void client_write_output(struct client *client)
{
	if (client_is_expired(client) || !client->send_buf_used)
		return;

	if (!g_queue_is_empty(client->deferred_send)) {
		client_defer_output(client, client->send_buf,
				    client->send_buf_used);

		/* try to flush the deferred buffers now; the current
		   server command may take too long to finish, and
		   meanwhile try to feed output to the client,
		   otherwise it will time out.  One reason why
		   deferring is slow might be that currently each
		   client_write() allocates a new deferred buffer.
		   This should be optimized after MPD 0.14. */
		client_write_deferred(client);
	} else
		client_write_direct(client, client->send_buf,
				    client->send_buf_used);

	client->send_buf_used = 0;
}

void client_write(struct client *client, const char *buffer, size_t buflen)
{
	/* if the client is going to be closed, do nothing */
	if (client_is_expired(client))
		return;

	while (buflen > 0 && !client_is_expired(client)) {
		size_t copylen;

		assert(client->send_buf_used < sizeof(client->send_buf));

		copylen = sizeof(client->send_buf) - client->send_buf_used;
		if (copylen > buflen)
			copylen = buflen;

		memcpy(client->send_buf + client->send_buf_used, buffer,
		       copylen);
		buflen -= copylen;
		client->send_buf_used += copylen;
		buffer += copylen;
		if (client->send_buf_used >= sizeof(client->send_buf))
			client_write_output(client);
	}
}

void client_puts(struct client *client, const char *s)
{
	client_write(client, s, strlen(s));
}

void client_vprintf(struct client *client, const char *fmt, va_list args)
{
	va_list tmp;
	int length;
	char *buffer;

	va_copy(tmp, args);
	length = vsnprintf(NULL, 0, fmt, tmp);
	va_end(tmp);

	if (length <= 0)
		/* wtf.. */
		return;

	buffer = xmalloc(length + 1);
	vsnprintf(buffer, length + 1, fmt, args);
	client_write(client, buffer, length);
	free(buffer);
}

G_GNUC_PRINTF(2, 3) void client_printf(struct client *client, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	client_vprintf(client, fmt, args);
	va_end(args);
}

/**
 * Send "idle" response to this client.
 */
static void
client_idle_notify(struct client *client)
{
	unsigned flags, i;
	const char *const* idle_names;

	assert(client->idle_waiting);
	assert(client->idle_flags != 0);

	flags = client->idle_flags;
	client->idle_flags = 0;
	client->idle_waiting = false;

	idle_names = idle_get_names();
	for (i = 0; idle_names[i]; ++i) {
		if (flags & (1 << i) & client->idle_subscriptions)
			client_printf(client, "changed: %s\n",
				      idle_names[i]);
	}

	client_puts(client, "OK\n");
	client->lastTime = time(NULL);
}

void client_manager_idle_add(unsigned flags)
{
	struct client *client;

	assert(flags != 0);

	list_for_each_entry(client, &clients, siblings) {
		if (client_is_expired(client))
			continue;

		client->idle_flags |= flags;
		if (client->idle_waiting
		    && (client->idle_flags & client->idle_subscriptions)) {
			client_idle_notify(client);
			client_write_output(client);
		}
	}
}

bool client_idle_wait(struct client *client, unsigned flags)
{
	assert(!client->idle_waiting);

	client->idle_waiting = true;
	client->idle_subscriptions = flags;

	if (client->idle_flags & client->idle_subscriptions) {
		client_idle_notify(client);
		return true;
	} else
		return false;
}