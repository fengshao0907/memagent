/* 
Copyright (c) 2008 QUE Hongyu
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
*/

/* Changelog:
 * 2008-08-20, coding started
 * 2008-09-04, v0.1 finished
 * 2008-09-07, v0.2 finished, code cleanup, drive_get_server function
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <event.h>

#define VERSION "0.2"

#define OUTOFCONN "SERVER_ERROR OUT OF CONNECTION"

#define BUFFERLEN 2048
#define MAX_TOKENS 8
#define COMMAND_TOKEN 0
#define KEY_TOKEN 1
#define BYTES_TOKEN 4
#define KEY_MAX_LENGTH 250
#define BUFFER_PIECE_SIZE 32

#define UNUSED(x) ( (void)(x) )

/* structure definitions */
typedef struct conn conn;
typedef struct matrix matrix;
typedef struct list list;
typedef struct buffer buffer;
typedef struct server server;

typedef enum {
	CLIENT_COMMAND,
	CLIENT_NREAD, /* MORE CLIENT DATA */
	CLIENT_TRANSCATION
} client_state_t;

typedef enum {
	SERVER_INIT,
	SERVER_CONNECTING,
	SERVER_CONNECTED,
	SERVER_ERROR
} server_state_t;

struct buffer {
	char *ptr;

	size_t used;
	size_t size;
	size_t len; /* ptr length */

	struct buffer *next;
};

/* list to buffers */
struct list {
	buffer *first;
	buffer *last;
};

/* connection to memcached server */
struct server {
	int sfd;
	server_state_t state;
	struct event ev;
	
	matrix *owner;

	/* first response line
	 * NOT_FOUND\r\n
	 * STORED\r\n
	 */
	char line[BUFFERLEN];
	int pos;

	/* get/gets key ....
	 * VALUE <key> <flags> <bytes> [<cas unique>]\r\n 
	 */
	int valuebytes;
	int has_response_header:1;
	int remove_trail:1;

	/* input buffer */
	list *request;
	/* output buffer */
	list *response;

};

struct conn {
	/* client part */
	int cfd;
	client_state_t state;
	struct event ev;

	/* command buffer */
	char line[BUFFERLEN+1];
	int pos;

	int storebytes; /* bytes stored by CAS/SET/ADD/... command */

	struct flag {
		int is_get_cmd:1;
		int is_gets_cmd:1;
		int is_set_cmd:1;
		int no_reply:1;
		int is_finished:1;
	} flag;

	int keycount; /* GET/GETS multi keys */
	int keyidx;
	char **keys;

	/* input buffer */
	list *request;
	/* output buffer */
	list *response;

	struct server *srv;
};

/* memcached server structure */
struct matrix {
	char *ip;
	int port;
	struct sockaddr_in dstaddr;

	int size;
	int used;
	struct server **pool;
};

typedef struct token_s {
	char *value;
	size_t length;
} token_t;

/* static variables */
static int port = 11211, maxconns = 4096, curconns = 0, sockfd = -1, verbose_mode = 0;
static struct event ev_master;

static int freetotal, freecurr;
static struct conn **freeconns;

static struct matrix *matrixs = NULL; /* memcached server list */
static int matrixcnt = 0;

static void drive_client(const int, const short, void *);
static void drive_server(const int, const short, void *);
static void drive_get_server(const int, const short, void *);
static void finish_transcation(conn *);
static void do_transcation(conn *);

static void show_help(void)
{
	char *b = "memcached agent v" VERSION " Build-Date: " __DATE__ " " __TIME__ "\n"
		  "Usage:\n  -h this message\n" 
		   "  -u uid\n" 
		   "  -g gid\n"
		   "  -p port, default is 11211\n"
		   "  -s ip:port, set memcached backend server ip and port\n"
		   "  -l ip, local bind ip address, default is 0.0.0.0\n"
		   "  -n number, set max connections, default is 4096\n"
		   "  -D don't go to background\n"
		   "  -v verbose\n"
		   "\n";
	fprintf(stderr, b, strlen(b));
}

/* the famous DJB hash function for strings from stat_cache.c*/
static int hashme(char *str)
{
	unsigned int hash = 5381;
	const char *s;

	if (str == NULL) return 0;

	for (s = str; *s; s++) { 
		hash = ((hash << 5) + hash) + *s;
	}
	hash &= 0x7FFFFFFF; /* strip the highest bit */
	return hash;
}


static buffer* buffer_init_size(int size)
{
	buffer *b;

	if (size <= 0) return NULL;
	b = (struct buffer *) calloc(sizeof(struct buffer), 1);
	if (b == NULL) return NULL;

	size +=  BUFFER_PIECE_SIZE - (size %  BUFFER_PIECE_SIZE);

	b->ptr = (char *) calloc(1, size);
	if (b->ptr == NULL) {
		free(b);
		return NULL;
	}

	b->len = size;
	return b;
}

static void buffer_free(buffer *b)
{
	if (!b) return;

	free(b->ptr);
	free(b);
}

static list *list_init(void)
{
	list *l;

	l = (struct list *) calloc(sizeof(struct list), 1);
	return l;
}

static void list_free(list *l, int keep_list)
{
	buffer *b, *n;

	if (l == NULL) return;

	b = l->first;
	while(b) {
		n = b->next;
		buffer_free(b);
		b = n;
	}

	if (keep_list)
		l->first = l->last = NULL;
	else
		free(l);
}

static void remove_finished_buffers(list *l)
{
	buffer *n, *b;

	if (l == NULL) return;
	b = l->first;
	while(b) {
		if (b->used < b->size) /* incompleted buffer */
			break;
		n = b->next;
		buffer_free(b);
		b = n;
	}

	if (b == NULL) {
		l->first = l->last = NULL;
	} else {
		l->first = b;
	}
}

static void move_list(list *src, list *dst)
{
	if (src == NULL || dst == NULL) return;

	if (dst->first == NULL)
		dst->first = src->first;
	else
		dst->last->next = src->first;

	dst->last = src->last;

	src->last = src->first = NULL;
}

static void append_buffer_to_list(list *l, buffer *b)
{
	if (l == NULL || b == NULL)
		return;

	if (l->first == NULL) {
		l->first = l->last = b;
	} else {
		l->last->next = b;
		l->last = b;
	}
}

/* return -1 if not found
 * return pos if found
 */
static int memstr(char *s, char *find, int srclen, int findlen)
{
	char *bp, *sp;
	int len = 0, success = 0;
	
	if (findlen == 0 || srclen < findlen) return -1;
	for (len = 0; len <= (srclen-findlen); len ++) {
		if (s[len] == find[0]) {
			bp = s + len;
			sp = find;
			do {
				if (!*sp) {
					success = 1;
					break;
				}
			} while (*bp++ == *sp++);
			if (success) break;
		}
	}

	if (success) return len;
	else return -1;
}

static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens)
{
	char *s, *e;
	size_t ntokens = 0;

	assert(command != NULL && tokens != NULL && max_tokens > 1);

	for (s = e = command; ntokens < max_tokens - 1; ++e) {
		if (*e == ' ') {
			if (s != e) {
				tokens[ntokens].value = s;
				tokens[ntokens].length = e - s;
				ntokens++;
				*e = '\0';
			}
			s = e + 1;
		}
		else if (*e == '\0') {
			if (s != e) {
				tokens[ntokens].value = s;
				tokens[ntokens].length = e - s;
				ntokens++;
			}

			break; /* string end */
		}
	}

	/*
	 * If we scanned the whole string, the terminal value pointer is null,
	 * otherwise it is the first unprocessed character.
	 */
	tokens[ntokens].value =  *e == '\0' ? NULL : e;
	tokens[ntokens].length = 0;
	ntokens++;

	return ntokens;
}

static void free_server(struct server *s)
{
	if (s == NULL) return;

	if (s->sfd > 0) {
		event_del(&(s->ev));
		close(s->sfd);
	}

	list_free(s->request, 0);
	list_free(s->response, 0);
	free(s);
}

#define STEP 5
#define MAXIDLE 50

/* put server connection into keep alive pool */
static void put_server_into_pool(conn *c)
{
	struct matrix *m;
	struct server **p, *s;

	if (c == NULL || c->srv == NULL) return;

	s = c->srv;
	if (s->owner == NULL || s->state != SERVER_CONNECTED || s->sfd <= 0) {
		free_server(s);
		c->srv = NULL;
		return;
	}

	list_free(s->request, 1);
	list_free(s->response, 1);
	s->pos = s->has_response_header = s->remove_trail = 0;

	m = s->owner;
	if (m->size == 0) {
		m->pool = (struct server **) calloc(sizeof(struct server *), STEP);
		if (m->pool == NULL) {
			fprintf(stderr, "out of memory for pool allocation\n");
			m = NULL;
		} else {
			m->used = 0;
		}
	} else if (m->used == m->size) {
		if (m->size < MAXIDLE) {
			p = (struct server **)realloc(m->pool, sizeof(struct server *)*(m->size + STEP));
			if (p == NULL) {
				fprintf(stderr, "out of memory for pool reallocation\n");
				m = NULL;
			} else {
				m->pool = p;
				m->size += STEP;
			}
		} else {
			m = NULL;
		}
	}

	if (m != NULL) {
		m->pool[m->used ++] = s;
		event_del(&(s->ev));
		memset(&(s->ev), 0, sizeof(struct event));
	} else {
		free_server(s);
	}

	c->srv = NULL;
}

#undef STEP
#undef MAXIDLE

static void conn_close(conn *c)
{
	int i;

	assert(c != NULL);
	
	/* check client connection */
	if (c->cfd > 0) {
		if (verbose_mode)
			fprintf(stderr, "close client connection fd %d\n", c->cfd);
		event_del(&(c->ev));
		close(c->cfd);
		curconns --;
		c->cfd = 0;
	}

	put_server_into_pool(c);

	if (c->keys) {
		for (i = 0; i < c->keycount; i ++)
			free(c->keys[i]);
		free(c->keys);
		c->keys = NULL;
	}

	/* recycle client connections */
	if (freecurr < freetotal) {
		freeconns[freecurr++] = c;
		list_free(c->request, 1);
		list_free(c->response, 1);
		c->keycount = c->keyidx = c->pos = c->storebytes = 0;
	} else {
		list_free(c->request, 0);
		list_free(c->response, 0);
		free(c);
	}
}

/* ------------- from lighttpd's network_writev.c ------------ */

#ifndef UIO_MAXIOV
# if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__)
/* FreeBSD 4.7 defines it in sys/uio.h only if _KERNEL is specified */
#  define UIO_MAXIOV 1024
# elif defined(__sgi)
/* IRIX 6.5 has sysconf(_SC_IOV_MAX) which might return 512 or bigger */
#  define UIO_MAXIOV 512
# elif defined(__sun)
/* Solaris (and SunOS?) defines IOV_MAX instead */
#  ifndef IOV_MAX
#   define UIO_MAXIOV 16
#  else
#   define UIO_MAXIOV IOV_MAX
#  endif
# elif defined(IOV_MAX)
#  define UIO_MAXIOV IOV_MAX
# else
#  error UIO_MAXIOV nor IOV_MAX are defined
# endif
#endif

/* return 0 if success */
static int writev_list(int fd, list *l)
{
	size_t num_chunks, i, num_bytes = 0, toSend, r, r2;
	struct iovec chunks[UIO_MAXIOV];
	buffer *b;

	if (l == NULL || fd <= 0) return 0;

	for (num_chunks = 0, b = l->first; b && num_chunks < UIO_MAXIOV; num_chunks ++, b = b->next) ;

	for (i = 0, b = l->first; i < num_chunks; b = b->next, i ++) {
		if (b->size == 0) {
			num_chunks --;
			i --;
		} else {
			chunks[i].iov_base = b->ptr + b->used;
			toSend = b->size - b->used;

			/* protect the return value of writev() */
			if (toSend > SSIZE_MAX ||
			    (num_bytes + toSend) > SSIZE_MAX) {
				chunks[i].iov_len = SSIZE_MAX - num_bytes;

				num_chunks = i + 1;
				break;
			} else {
				chunks[i].iov_len = toSend;
			}

			num_bytes += toSend;
		}
	}

	if ((r = writev(fd, chunks, num_chunks)) < 0) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			fprintf(stderr, "writev to fd %d interrupted\n", fd); /* QHY */
			return 0; /* try again */
			break;
		case EPIPE:
		case ECONNRESET:
			return -2; /* connection closed */
			break;
		default:
			return -1; /* error */
			break;
		}
	}

	r2 = r;

	for (i = 0, b = l->first; i < num_chunks; b = b->next, i ++) {
		if (r >= (ssize_t)chunks[i].iov_len) {
			r -= chunks[i].iov_len;
			b->used += chunks[i].iov_len;
		} else {
			/* partially written */
			b->used += r;
			break;
		}
	}

	remove_finished_buffers(l);
	return r2;
}

/* --------- end here ----------- */

static void out_string(conn *c, const char *str)
{
	/* append str to c->wbuf */
	int len = 0;
	buffer *b;

	assert(c != NULL);
	
	if (str == NULL || str[0] == '\0') return;

	len = strlen(str);

	b = buffer_init_size(len + 3);
	if (b == NULL) return;

	memcpy(b->ptr, str, len);
	memcpy(b->ptr + len, "\r\n", 2);
	b->size = len + 2;
	b->ptr[b->size] = '\0';
	
	append_buffer_to_list(c->response, b);

	if (writev_list(c->cfd, c->response) >= 0) {
		if (c->response->first) {
			event_del(&(c->ev));
			event_set(&(c->ev), c->cfd, EV_WRITE|EV_PERSIST, drive_client, (void *) c);
			event_add(&(c->ev), 0);
		}
	} else {
		/* client reset/close connection*/
		conn_close(c);
	}
}

/* finish proxy transcation */
static void finish_transcation(conn *c)
{
	int i;

	if (c == NULL) return;

	if (c->keys) {
		for (i = 0; i < c->keycount; i ++)
			free(c->keys[i]);
		free(c->keys);
		c->keys = NULL;
		c->keycount = 0;
	}

	c->state = CLIENT_COMMAND;
	list_free(c->request, 1);

	if (c->flag.is_get_cmd && (c->flag.is_finished == 0)) {
		c->flag.is_finished = 1;
		out_string(c, "END");
	}
}

/* start/repeat memcached proxy transcations */

static void do_transcation(conn *c)
{
	int hash;
	struct matrix *m;
	struct server *s;
	char *key;
	buffer *b;

	if (c == NULL) return;

	/* recycle previous server connection */
	put_server_into_pool(c);
	
	if (c->flag.is_get_cmd) {
		if (c->keyidx < c->keycount) {
			key = c->keys[c->keyidx++];
		} else {
			/* end of get transcation */
			finish_transcation(c);
			return;
		}
	} else {
		key = c->keys[0];
	}

	/* just round selection */
	hash = hashme(key);
	m = matrixs + (hash%matrixcnt);

	if (m->pool && (m->used > 0)) {
		c->srv = m->pool[--m->used];
	} else {
		c->srv = (struct server *) calloc(sizeof(struct server), 1);
		assert(c->srv);
		c->srv->request = list_init();
		c->srv->response = list_init();
	}
	s = c->srv;
	s->owner = m;

	if (verbose_mode) 
		fprintf(stderr, "%s -> %s:%d\n", key, m->ip, m->port);

	if (c->srv->sfd <= 0) {
		c->srv->sfd = socket(AF_INET, SOCK_STREAM, 0); 
		if (c->srv->sfd < 0) {
			fprintf(stderr, "CAN'T CREATE TCP SOCKET TO MEMCACHED\n");
			conn_close(c);
			return;
		}
		fcntl(c->srv->sfd, F_SETFL, fcntl(c->srv->sfd, F_GETFL)|O_NONBLOCK);
	}

	/* reset flags */
	s->has_response_header = 0;
	s->remove_trail = 0;
	s->valuebytes = 0;

	if (c->flag.is_get_cmd) {
		b = buffer_init_size(strlen(key) + 20);
		if (b == NULL) {
			fprintf(stderr, "SERVER OUT OF MEMORY\n");
			s->state = SERVER_ERROR;
			conn_close(c);
			return;
		}
		b->size = snprintf(b->ptr, b->len - 1, "%s %s\r\n", c->flag.is_gets_cmd?"gets":"get", key);
		append_buffer_to_list(s->request, b);
	} else {
		move_list(c->request, s->request);
	}

	c->state = CLIENT_TRANSCATION;
	/* server event handler */
	memset(&(s->ev), 0, sizeof(struct event));

	if (c->flag.is_get_cmd)
		event_set(&(s->ev), s->sfd, EV_PERSIST|EV_WRITE, drive_get_server, (void *)c);
	else
		event_set(&(s->ev), s->sfd, EV_PERSIST|EV_WRITE, drive_server, (void *)c);

	event_add(&(s->ev), 0);
}

/* drive machine for 'get/gets' commands */
static void drive_get_server(const int fd, const short which, void *arg)
{
	struct server *s;
	conn *c;
	buffer *b;
	int socket_error, r, toread, pos;
	socklen_t servlen, socket_error_len;

	if (arg == NULL) return;
	c = (conn *)arg;

	s = c->srv;
	if (s == NULL) return;

	/* double check */
	assert(c->flag.is_get_cmd);

	if (which & EV_WRITE) {
		switch (s->state) {
			case SERVER_INIT:
				servlen = sizeof(s->owner->dstaddr);
				if (-1 == connect(s->sfd, (struct sockaddr *) &(s->owner->dstaddr), servlen)) {
					if (errno != EINPROGRESS && errno != EALREADY) {
						if (verbose_mode)
							fprintf(stderr, "!!%d: CAN'T CONNECT TO %s:%d, %s\n",
								s->sfd, s->owner->ip, s->owner->port, strerror(errno));
						conn_close(c);
						return;
					}
				}
				s->state = SERVER_CONNECTING;
				break;

			case SERVER_CONNECTING:
				socket_error_len = sizeof(socket_error);
				/* try to finish the connect() */
				if (0 != getsockopt(s->sfd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len)) {
					if (verbose_mode)
						fprintf(stderr, "!!%d: CAN'T CONNECT TO %s:%d, %s\n",
							s->sfd, s->owner->ip, s->owner->port, strerror(errno));
					conn_close(c);
					return;
				}
				
				if (socket_error != 0) {
					if (verbose_mode)
						fprintf(stderr, "!!%d: CAN'T CONNECT TO %s:%d, %s\n",
								s->sfd, s->owner->ip, s->owner->port, strerror(socket_error));
					conn_close(c);
					return;
				}
				
				if (verbose_mode)
					fprintf(stderr, "fd %d <-> %s:%d\n", 
							s->sfd, s->owner->ip, s->owner->port);
				s->state = SERVER_CONNECTED;
				break;

			case SERVER_CONNECTED:
				/* write request to memcached server */
				r = writev_list(s->sfd, s->request);
				if (r < 0) {
					/* write failed */
					s->state = SERVER_ERROR;
					conn_close(c);
					return;
				} else {
					if (s->request->first == NULL) {
						/* finish writing request to memcached server */
						event_del(&(s->ev));
						event_set(&(s->ev), s->sfd, EV_PERSIST|EV_READ, drive_get_server, arg);
						event_add(&(s->ev), 0);
					}
				}
				break;

			case SERVER_ERROR:
				conn_close(c);
				break;
		}
		return;
	} 
	
	if (!(which & EV_READ)) return;
   
	/* get the byte counts of read */
	if (ioctl(s->sfd, FIONREAD, &toread)) {
		if (verbose_mode)
			fprintf(stderr, "!!%d: IOCTL 'FIONREAD' ACTION FAILED, %s\n", 
				s->sfd, strerror(errno));
		s->state = SERVER_ERROR;
		conn_close(c);
		return;
	}

	if (toread == 0) {
		/* memcached server close/reset connection */
		s->state = SERVER_ERROR;
		conn_close(c);
		return;
	}

	if (s->has_response_header == 0) {
		/* NO RESPONSE HEADER */
		if (toread > (BUFFERLEN - s->pos))
			toread = BUFFERLEN - s->pos;
	} else {
		/* HAS RESPONSE HEADER */
		if (toread > (BUFFERLEN - s->pos))
			toread = BUFFERLEN - s->pos;
		if (toread > s->valuebytes)
			toread = s->valuebytes;
	}

	r = read(s->sfd, s->line + s->pos , toread);
	if (r <= 0) {
		if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
			if (verbose_mode)
				fprintf(stderr, "FAIL TO READ FROM SERVER %s:%d, %s\n", 
					s->owner->ip, s->owner->port, strerror(errno));
			s->state = SERVER_ERROR;
			conn_close(c);
		}
		return;
	}

	s->pos += r;
	s->line[s->pos] = '\0';

	if (s->has_response_header == 0) {
		pos = memstr(s->line, "\n", s->pos, 1);

		if (pos == -1) return; /* not found */

		/* found \n */
		s->has_response_header = 1;
		s->remove_trail = 0;
		pos ++;

		s->valuebytes = -1;

		/* VALUE <key> <flags> <bytes> [<cas unique>]\r\n
		 * END\r\n*/
		if (strncasecmp(s->line, "VALUE ", 6) == 0) {
			char *p = NULL;
			p = strchr(s->line + 6, ' ');
			if (p) {
				p = strchr(p + 1, ' ');
				if (p) {
					s->valuebytes = atol(p+1);
					if (s->valuebytes < 0) {
						s->state = SERVER_ERROR;
						conn_close(c);
					}
				}
			}
		}

		if (s->valuebytes < 0) {
			/* END\r\n or SERVER_ERROR\r\n
			 * just skip this transcation
			 */
			do_transcation(c);
			return;
		}
		s->valuebytes += 7; /* trailing \r\nEND\r\n */

		b = buffer_init_size(pos + 1);
		if (b == NULL) {
			fprintf(stderr, "SERVER OUT OF MEMORY\n");
			s->state = SERVER_ERROR;
			conn_close(c);
			return;
		}
		memcpy(b->ptr, s->line, pos);
		b->size = pos;
		append_buffer_to_list(s->response, b);

		if (s->pos > pos) {
			memmove(s->line, s->line + pos, s->pos - pos);
			s->pos -= pos;
		} else {
			s->pos = 0;
		}

		if (s->pos > 0) 
			s->valuebytes -= s->pos;
	} else {
		/* HAS RESPONSE HEADER */
		s->valuebytes -= r;
	}

	if (s->remove_trail) {
		s->pos = 0;
	} else if (s->pos > 0) {
		b = buffer_init_size(s->pos+1);
		if (b == NULL) {
			fprintf(stderr, "SERVER OUT OF MEMORY\n");
			s->state = SERVER_ERROR;
			conn_close(c);
			return;
		}
		memcpy(b->ptr, s->line, s->pos);
		b->size = s->pos;

		if (s->valuebytes <= 5) {
			b->size -= (5 - s->valuebytes); /* remove trailing END\r\n */
			s->remove_trail = 1;
		}
		s->pos = 0;

		append_buffer_to_list(s->response, b);
	}

	if (s->valuebytes == 0) {
		/* GET commands finished, go on next memcached server */
		move_list(s->response, c->response);
		if (writev_list(c->cfd, c->response) >= 0) {
			if (c->response->first) {
				event_del(&(c->ev));
				event_set(&(c->ev), c->cfd, EV_WRITE|EV_PERSIST, drive_client, (void *) c);
				event_add(&(c->ev), 0);
			}
			do_transcation(c); /* NEXT MEMCACHED SERVER */
		} else {
			/* client reset/close connection*/
			conn_close(c);
		}
	}
}

/* drive machine for commands other than 'get/gets' */
static void drive_server(const int fd, const short which, void *arg)
{
	struct server *s;
	conn *c;
	buffer *b;
	int socket_error, r, toread, pos;
	socklen_t servlen, socket_error_len;

	if (arg == NULL) return;
	c = (conn *)arg;

	s = c->srv;
	if (s == NULL) return;

	/* double check */
	assert(!c->flag.is_get_cmd);

	if (which & EV_WRITE) {
		switch (s->state) {
			case SERVER_INIT:
				servlen = sizeof(s->owner->dstaddr);
				if (-1 == connect(s->sfd, (struct sockaddr *) &(s->owner->dstaddr), servlen)) {
					if (errno != EINPROGRESS && errno != EALREADY) {
						if (verbose_mode)
							fprintf(stderr, "!!%d: CAN'T CONNECT TO %s:%d, %s\n",
								s->sfd, s->owner->ip, s->owner->port, strerror(errno));
						conn_close(c);
						return;
					}
				}
				s->state = SERVER_CONNECTING;
				break;

			case SERVER_CONNECTING:
				socket_error_len = sizeof(socket_error);
				/* try to finish the connect() */
				if (0 != getsockopt(s->sfd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len)) {
					if (verbose_mode)
						fprintf(stderr, "!!%d: CAN'T CONNECT TO %s:%d, %s\n",
							s->sfd, s->owner->ip, s->owner->port, strerror(errno));
					conn_close(c);
					return;
				}
				
				if (socket_error != 0) {
					if (verbose_mode)
						fprintf(stderr, "!!%d: CAN'T CONNECT TO %s:%d, %s\n",
								s->sfd, s->owner->ip, s->owner->port, strerror(socket_error));
					conn_close(c);
					return;
				}
				
				if (verbose_mode)
					fprintf(stderr, "fd %d <-> %s:%d\n", 
							s->sfd, s->owner->ip, s->owner->port);
				s->state = SERVER_CONNECTED;
				break;

			case SERVER_CONNECTED:
				/* write request to memcached server */
				r = writev_list(s->sfd, s->request);
				if (r < 0) {
					/* write failed */
					s->state = SERVER_ERROR;
					conn_close(c);
					return;
				} else {
					if (s->request->first == NULL ) {
						/* finish writing request to memcached server */
						if (c->flag.no_reply) {
							finish_transcation(c);
						} else {
							event_del(&(s->ev));
							event_set(&(s->ev), s->sfd, EV_PERSIST|EV_READ, drive_server, arg);
							event_add(&(s->ev), 0);
						}
					}
				}
				break;

			case SERVER_ERROR:
				conn_close(c);
				break;
		}
		return;
	} 
	
	if (!(which & EV_READ)) return;
   
	/* get the byte counts of read */
	if (ioctl(s->sfd, FIONREAD, &toread)) {
		if (verbose_mode)
			fprintf(stderr, "!!%d: IOCTL 'FIONREAD' ACTION FAILED, %s\n", 
				s->sfd, strerror(errno));
		conn_close(c);
		return;
	}

	if (toread == 0) {
		/* memcached server close/reset connection */
		s->state = SERVER_ERROR;
		conn_close(c);
		return;
	}

	if (toread > (BUFFERLEN - s->pos))
		toread = BUFFERLEN - s->pos;

	r = read(s->sfd, s->line + s->pos , toread);
	if (r <= 0) {
		if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
			if (verbose_mode)
				fprintf(stderr, "FAIL TO READ FROM SERVER %s:%d, %s\n", 
					s->owner->ip, s->owner->port, strerror(errno));
			s->state = SERVER_ERROR;
			conn_close(c);
		}
		return;
	}

	s->pos += r;
	s->line[s->pos] = '\0';

	pos = memstr(s->line, "\n", s->pos, 1);

	if (pos == -1) return; /* not found */

	/* found \n */
	pos ++;

	b = buffer_init_size(pos + 1);
	if (b == NULL) {
		fprintf(stderr, "SERVER OUT OF MEMORY\n");
		s->state = SERVER_ERROR;
		conn_close(c);
		return;
	}
	memcpy(b->ptr, s->line, pos);
	b->size = pos;

	append_buffer_to_list(s->response, b);
	move_list(s->response, c->response);
	if (writev_list(c->cfd, c->response) >= 0) {
		if (c->response->first) {
			event_del(&(c->ev));
			event_set(&(c->ev), c->cfd, EV_WRITE|EV_PERSIST, drive_client, (void *) c);
			event_add(&(c->ev), 0);
		}
		finish_transcation(c);
	} else {
		/* client reset/close connection*/
		conn_close(c);
	}
}

/* return 1 if command found
 * return 0 if not found
 */
static void process_command(conn *c)
{
	char *p;
	int len, skip = 0, i, j;
	buffer *b;
	token_t tokens[MAX_TOKENS];
	size_t ntokens;

	if (c->state != CLIENT_COMMAND) return;

	p = strchr(c->line, '\n');
	if (p == NULL) return;

	len = p - c->line;
	*p = '\0'; /* remove \n */
	if (*(p-1) == '\r') {
		*(p-1) = '\0'; /* remove \r */
		len --;
	}

	/* backup command line buffer first */
	b = buffer_init_size(len + 3);
	memcpy(b->ptr, c->line, len);
	b->ptr[len] = '\r';
	b->ptr[len+1] = '\n';
	b->ptr[len+2] = '\0';
	b->size = len + 2;

	memset(&(c->flag), 0, sizeof(c->flag));
	c->storebytes = c->keyidx = 0;

	ntokens = tokenize_command(c->line, tokens, MAX_TOKENS);
	if (ntokens >= 3 && (
			(strcmp(tokens[COMMAND_TOKEN].value, "get") == 0) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "gets") == 0)
			)) {
		/*
		 * get/gets <key>*\r\n
		 *
		 * VALUE <key> <flags> <bytes> [<cas unique>]\r\n
		 * <data block>\r\n
		 * "END\r\n"
		 */
		c->flag.is_get_cmd = 1;

		if (strcmp(tokens[COMMAND_TOKEN].value, "gets") == 0)
			c->flag.is_gets_cmd = 1; /* GETS */

		c->keycount = ntokens - KEY_TOKEN - 1;
		c->keys = (char **) calloc(sizeof(char *), c->keycount);
		assert(c->keys);
		for (i = KEY_TOKEN, j = 0; (i < ntokens) && (j < c->keycount); i ++, j ++)
			c->keys[j] = strdup(tokens[i].value);
	} else if ((ntokens == 4 || ntokens == 5) && (
				(strcmp(tokens[COMMAND_TOKEN].value, "decr") == 0) ||
				(strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0)
				)) {
		/*
		 * incr <key> <value> [noreply]\r\n
		 * decr <key> <value> [noreply]\r\n
		 *
		 * "NOT_FOUND\r\n" to indicate the item with this value was not found 
		 * <value>\r\n , where <value> is the new value of the item's data,
		 */
	} else if (ntokens >= 3 && ntokens <= 5 && (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {
		/*
		 * delete <key> [<time>] [noreply]\r\n
		 *
		 * "DELETED\r\n" to indicate success 
		 * "NOT_FOUND\r\n" to indicate that the item with this key was not
		 */
	} else if ((ntokens == 7 || ntokens == 8) && 
			(strcmp(tokens[COMMAND_TOKEN].value, "cas") == 0)) {
		/*
		 * cas <key> <flags> <exptime> <bytes> <cas unqiue> [noreply]\r\n
		 * <data block>\r\n
		 *
		 * "STORED\r\n", to indicate success.  
		 * "NOT_STORED\r\n" to indicate the data was not stored, but not
		 * "EXISTS\r\n" to indicate that the item you are trying to store with
		 * "NOT_FOUND\r\n" to indicate that the item you are trying to store
		 */
		c->flag.is_set_cmd = 1;
		c->storebytes = atol(tokens[BYTES_TOKEN].value);
		c->storebytes += 2; /* \r\n */
	} else if ((ntokens == 6 || ntokens == 7) && (
			(strcmp(tokens[COMMAND_TOKEN].value, "add") == 0) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "set") == 0) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "replace") == 0) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "prepend") == 0) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "append") == 0)
			)) {
		/*
		 * <cmd> <key> <flags> <exptime> <bytes> [noreply]\r\n
		 * <data block>\r\n
		 *
		 * "STORED\r\n", to indicate success.  
		 * "NOT_STORED\r\n" to indicate the data was not stored, but not
		 * "EXISTS\r\n" to indicate that the item you are trying to store with
		 * "NOT_FOUND\r\n" to indicate that the item you are trying to store
		 */
		c->flag.is_set_cmd = 1;
		c->storebytes = atol(tokens[BYTES_TOKEN].value);
		c->storebytes += 2; /* \r\n */
	} else if (ntokens >= 2 && (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0)) {
		/* END\r\n
		 */
		char tmp[128];
		out_string(c, "memcached agent v" VERSION);
		for (i = 0; i < matrixcnt; i ++) {
			snprintf(tmp, 127, "matrix %d -> %s:%d, pool size %d", 
					i+1, matrixs[i].ip, matrixs[i].port, matrixs[i].used);
			out_string(c, tmp);
		}
		out_string(c, "END");
		skip = 1;
	} else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0)) {
		conn_close(c);
		return;
	} else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0)) {
		out_string(c, "VERSION memcached agent v" VERSION);
		skip = 1;
	} else {
		out_string(c, "UNSUPPORTED COMMAND");
		skip = 1;
	}

	/* finish process commands */
	if (skip == 0) {
		/* append buffer to list */
		append_buffer_to_list(c->request, b);

		if (c->flag.is_get_cmd == 0) {
			if (tokens[ntokens-2].value && strcmp(tokens[ntokens-2].value, "noreply") == 0)
				c->flag.no_reply = 1;
			c->keycount = 1;
			c->keys = (char **) calloc(sizeof(char *), 1);
			if (c->keys == NULL) {
				fprintf(stderr, "SERVER OUT OF MEMORY\n");
				conn_close(c);
				return;
			}
			c->keys[0] = strdup(tokens[KEY_TOKEN].value);
		}
	} else {
		buffer_free(b);
	}

	i = p - c->line + 1;
	if (i < c->pos) {
		memmove(c->line, p+1, c->pos - i);
		c->pos -= i;
	} else {
		c->pos = 0;
	}

	if (c->storebytes > 0) {
		if (c->pos > 0) {
			/* append more buffer to list */
			b = buffer_init_size(c->pos + 1);
			if (b == NULL) {
				fprintf(stderr, "SERVER OUT OF MEMORY\n");
				conn_close(c);
				return;
			}
			memcpy(b->ptr, c->line, c->pos);
			b->size = c->pos;
			c->storebytes -= b->size;
			append_buffer_to_list(c->request, b);
			c->pos = 0;
		}
		if (c->storebytes > 0)
			c->state = CLIENT_NREAD;
		else 
			do_transcation(c);
	} else {
		if (skip == 0)
			do_transcation(c);
	}
}

/* drive machine of client connection */
static void drive_client(const int fd, const short which, void *arg)
{
	conn *c;
	int r, toread;
	buffer *b;

	c = (conn *)arg;
	assert(c != NULL);

	if (which & EV_READ) {
		/* get the byte counts of read */
		if (ioctl(c->cfd, FIONREAD, &toread)) {
			if (verbose_mode)
				fprintf(stderr, "!!%d: IOCTL 'FIONREAD' ACTION FAILED, %s\n", 
					c->cfd, strerror(errno));
			conn_close(c);
			return;
		}

		if (toread == 0) {
			conn_close(c);
			return;
		}

		switch(c->state) {
			case CLIENT_TRANSCATION:
			case CLIENT_COMMAND:
				r = BUFFERLEN - c->pos;
				if (r > toread) r = toread;

				toread = read(c->cfd, c->line + c->pos, r);
				if ((toread <= 0) && (errno != EINTR && errno != EAGAIN))  {
					conn_close(c);
					return;
				}
				c->pos += toread;
				c->line[c->pos] = '\0';
				process_command(c);
				break;
			case CLIENT_NREAD:
				/* we are going to read */
				assert(c->flag.is_set_cmd);

				if (toread > c->storebytes) toread = c->storebytes;

				b = buffer_init_size(toread + 1);
				if (b == NULL) {
					fprintf(stderr, "SERVER OUT OF MEMORY\n");
					conn_close(c);
					return;
				}

				r = read(c->cfd, b->ptr, toread);
				if ((r <= 0) && (errno != EINTR && errno != EAGAIN))  {
					buffer_free(b);
					conn_close(c);
					return;
				}
				b->size = r;
				b->ptr[r] = '\0';

				/* append buffer to list */
				append_buffer_to_list(c->request, b);
				c->storebytes -= r;
				if (c->storebytes <= 0)
					do_transcation(c);
				break;
		}
	} else if (which & EV_WRITE) {
		/* write to client */
		r = writev_list(c->cfd, c->response);
		if (r < 0) {
			conn_close(c);
			return;
		}

		if (c->response->first == NULL) {
			/* finish writing buffer to client
			 * switch back to reading from client
			 */
			event_del(&(c->ev));
			event_set(&(c->ev), c->cfd, EV_READ|EV_PERSIST, drive_client, (void *) c);
			event_add(&(c->ev), 0);
		}
	}
}

static void server_accept(const int fd, const short which, void *arg)
{
	conn *c = NULL;
	int newfd, flags = 1;
	struct sockaddr_in s_in;
	socklen_t len = sizeof(s_in);
	struct linger ling = {0, 0};

	UNUSED(arg);
	UNUSED(which);

	memset(&s_in, 0, len);
	newfd = accept(fd, (struct sockaddr *) &s_in, &len);
	if (newfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return ;
	}

	if (curconns >= maxconns) {
		/* out of connections */
		write(newfd, OUTOFCONN, sizeof(OUTOFCONN));
		close(newfd);
		return;
	}

	if (freecurr == 0) {
		c = (struct conn *) calloc(sizeof(struct conn), 1);
		if (c == NULL) {
			fprintf(stderr, "out of memory for new connection\n");
			close(newfd);
			return;
		}
		c->request = list_init();
		c->response = list_init();
	} else {
		c = freeconns[--freecurr];
	}

	curconns ++;

	c->cfd = newfd;

	fcntl(c->cfd, F_SETFL, fcntl(c->cfd, F_GETFL)|O_NONBLOCK);
	setsockopt(c->cfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
	setsockopt(c->cfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));

	/* setup client event handler */
	memset(&(c->ev), 0, sizeof(struct event));
	event_set(&(c->ev), c->cfd, EV_READ|EV_PERSIST, drive_client, (void *) c);
	event_add(&(c->ev), 0);
	
	return;
}

static void server_exit(int sig)
{
	if (verbose_mode)
		fprintf(stderr, "\nexiting\n");

	UNUSED(sig);

	if (sockfd > 0) close(sockfd);

	free(freeconns);
	exit(0);
}

int main(int argc, char **argv)
{
	char *p = NULL, *bindhost = NULL;
	int uid, gid, todaemon = 1, flags = 1, c;
	struct sockaddr_in server;
	struct linger ling = {0, 0};
	struct matrix *m; 
	
	while(-1 != (c = getopt(argc, argv, "p:u:g:s:Dhvn:l:"))) {
		switch (c) {
		case 'u':
			uid = atoi(optarg);
			if (uid > 0) {
				setuid(uid);
				seteuid(uid);
			}
			break;
		case 'g':
			gid = atoi(optarg);
			if (gid > 0) {
				setgid(gid);
				setegid(gid);
			}
			break;
		case 'v':
			verbose_mode = 1;
			todaemon = 0;
			break;
		case 'D':
			todaemon = 0;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'n':
			maxconns = atoi(optarg);
			if (maxconns <= 0) maxconns = 4096;
			break;
		case 'l':
			bindhost = optarg;
			break;
		case 's': /* server string */
			if (matrixcnt == 0) {
				matrixs = (struct matrix *) calloc(sizeof(struct matrix), 1);
				if (matrixs == NULL) {
					fprintf(stderr, "out of memory for %s\n", optarg);
					exit(1);
				}
				m = matrixs;
				matrixcnt = 1;
			} else {
				matrixs = (struct matrix *)realloc(matrixs, sizeof(struct matrix)*(matrixcnt+1));
				if (matrixs == NULL) {
					fprintf(stderr, "out of memory for %s\n", optarg);
					exit(1);
				}
				m = matrixs + matrixcnt;
				matrixcnt ++;
			}
			
			p = strchr(optarg, ':');
			if (p == NULL) {
				m->ip = strdup(optarg);
				m->port = 11211;
			} else {
				*p = '\0';
				m->ip = strdup(optarg);
				*p = ':';
				p ++;
				m->port = atoi(p);
				if (m->port <= 0) m->port = 11211;
			}
			m->dstaddr.sin_family = AF_INET;
			m->dstaddr.sin_addr.s_addr = inet_addr(m->ip);
			m->dstaddr.sin_port = htons(m->port);
			
			fprintf(stderr, "adding %s:%d to server matrix\n", m->ip, m->port);
			break;
		case 'h':
		default:
			show_help();
			return 1;
		}
	}

	if (matrixcnt == 0) {
		fprintf(stderr, "please provide -s \"ip:port\" argument\n\n");
		show_help();
		exit(1);
	}

	freetotal = 100;
	freecurr = 0;
	freeconns = (struct conn **) calloc(sizeof(struct conn *), freetotal);

	if (freeconns == NULL) {
		fprintf(stderr, "OUT OF MEMORY FOR FREE CONNECTION STRUCTURE\n");
		exit(1);
	}

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "CAN'T CREATE NETWORK SOCKET\n");
		return 1;
	}

	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL)|O_NONBLOCK);

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
	setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
	setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));

	memset((char *) &server, 0, sizeof(server));
	server.sin_family = AF_INET;
	if (bindhost == NULL)
		server.sin_addr.s_addr = htonl(INADDR_ANY);
	else
		server.sin_addr.s_addr = inet_addr(bindhost);

	server.sin_port = htons(port);

	if (bind(sockfd, (struct sockaddr *) &server, sizeof(server))) {
		if (errno != EINTR) 
			fprintf(stderr, "bind errno = %d: %s\n", errno, strerror(errno));
		close(sockfd);
		exit(1);
	}

	if (listen(sockfd, 512)) {
		fprintf(stderr, "listen errno = %d: %s\n", errno, strerror(errno));
		close(sockfd);
		exit(1);
	}

	if (verbose_mode)
		fprintf(stderr, "memcached agent listen at port %d\n", port);

	if (todaemon && daemon(0, 0) == -1) {
		fprintf(stderr, "failed to be a daemon\n");
		exit(1);
	}

	signal(SIGTERM, server_exit);
	signal(SIGINT, server_exit);

	event_init();
	event_set(&ev_master, sockfd, EV_READ|EV_PERSIST, server_accept, NULL);
	event_add(&ev_master, 0);
	event_loop(0);
	close(sockfd);
	return 0;
}
