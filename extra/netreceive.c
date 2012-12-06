/*-
 * Copyright (c) 2004 Robert N. M. Watson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/poll.h>

#include <netinet/in.h>
#include <netdb.h>          /* getaddrinfo */

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         /* close */

#include <fcntl.h>
#include <time.h>	/* clock_getres() */

#define MAXSOCK 20

#include <pthread.h>

static int round_to(int n, int l)
{
	return ((n + l - 1)/l)*l;
}

/*
 * Each socket uses multiple threads so the receiver is
 * more efficient. A collector thread runs the stats.
 */
struct td_desc {
	pthread_t td_id;
	uint64_t count;	/* rx counter */
	int fd;
	char *buf;
	int buflen;
};

static void
usage(void)
{

	fprintf(stderr, "netreceive port [nthreads]\n");
	exit(-1);
}

static __inline void
timespec_add(struct timespec *tsa, struct timespec *tsb)
{

        tsa->tv_sec += tsb->tv_sec;
        tsa->tv_nsec += tsb->tv_nsec;
        if (tsa->tv_nsec >= 1000000000) {
                tsa->tv_sec++;
                tsa->tv_nsec -= 1000000000;
        }
}

static __inline void
timespec_sub(struct timespec *tsa, struct timespec *tsb)
{

        tsa->tv_sec -= tsb->tv_sec;
        tsa->tv_nsec -= tsb->tv_nsec;
        if (tsa->tv_nsec < 0) {
                tsa->tv_sec--;
                tsa->tv_nsec += 1000000000;
        }
}

static void *
rx_body(void *data)
{
	struct td_desc *t = data;
	struct pollfd fds;

	fds.fd = t->fd;
	fds.events = POLLIN;

	for (;;) {
		if (poll(&fds, 1, -1) < 0) 
			perror("poll on thread");
		if (fds.revents & POLLIN) {
			for (;;) {
				int y = recv(t->fd, t->buf, t->buflen, MSG_DONTWAIT);
				if (y < 0)
					break;
				t->count++;
			}
		}
	}
	return NULL;
}

int
main(int argc, char *argv[])
{
	struct addrinfo hints, *res, *res0;
	char *dummy, *packet;
	int port;
	int error, v;
	const char *cause = NULL;
	int s[MAXSOCK];
	int nsock;
	int nthreads = 1;

	struct td_desc **tp;

	int td_len = round_to(sizeof(struct td_desc), 64); // cache align
	printf("td len %d -> %d\n", (int)sizeof(struct td_desc) , td_len);
	if (argc < 2)
		usage();

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;

	port = strtoul(argv[1], &dummy, 10);
	if (port < 1 || port > 65535 || *dummy != '\0')
		usage();
	if (argc > 2)
		nthreads = strtoul(argv[2], &dummy, 10);
	if (nthreads < 1 || nthreads > 64)
		usage();

	packet = malloc(65536);
	if (packet == NULL) {
		perror("malloc");
		return (-1);
	}
	bzero(packet, 65536);

	error = getaddrinfo(NULL, argv[1], &hints, &res0);
	if (error) {
		perror(gai_strerror(error));
		return (-1);
		/*NOTREACHED*/
	}

	nsock = 0;
	for (res = res0; res && nsock < MAXSOCK; res = res->ai_next) {
		s[nsock] = socket(res->ai_family, res->ai_socktype,
		res->ai_protocol);
		if (s[nsock] < 0) {
			cause = "socket";
			continue;
		}

		v = 128 * 1024;
		if (setsockopt(s[nsock], SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) < 0) {
			cause = "SO_RCVBUF";
			close(s[nsock]);
			continue;
		}
		if (bind(s[nsock], res->ai_addr, res->ai_addrlen) < 0) {
			cause = "bind";
			close(s[nsock]);
			continue;
		}
		(void) listen(s[nsock], 5);
		nsock++;
	}
	if (nsock == 0) {
		perror(cause);
		return (-1);
		/*NOTREACHED*/
	}

	printf("netreceive %d sockets x %d threads listening on UDP port %d\n",
		nsock, nthreads, (u_short)port);

	{
		int nt = nsock * nthreads;
		int lb = round_to(nt * sizeof (struct td_desc *), 64);
		char *m = calloc(1, lb + td_len * nt);
		int i, si;
		/* pointers plus the structs */
		if (m == NULL) {
			perror("no room for pointers!");
			exit(1);
		}
		tp = (struct td_desc **)m;
		m += lb;	/* skip the pointers */
		for (si = i = 0; i < nt; i++, m += td_len) {
			tp[i] = (struct td_desc *)m;
			tp[i]->fd = s[si];
			if (++si == nsock)
				si = 0;
			if (pthread_create(&tp[i]->td_id, NULL, rx_body, tp[i])) {
				perror("unable to create thread");
				exit(1);
			}
		}
	}

    {
	uint64_t c0, c1;
	struct timespec now, then, delta;
	/* now the parent collects and prints results */
	c0 = c1 = 0;
	clock_gettime(CLOCK_REALTIME, &then);
	fprintf(stderr, "start at %ld.%09ld\n", then.tv_sec, then.tv_nsec);
	while (1) {
		int i, nt = nsock * nthreads;
		int64_t dn;
		uint64_t pps;

		if (poll(NULL, 0, 500) < 0) 
			perror("poll");
		c0 = 0;
		for (i = 0; i < nt; i++) {
			c0 += tp[i]->count;
		}
		dn = c0 - c1;
		clock_gettime(CLOCK_REALTIME, &now);
		delta = now;
		timespec_sub(&delta, &then);
		then = now;
		pps = dn;
		pps = (pps * 1000000000) / (delta.tv_sec*1000000000 + delta.tv_nsec + 1);
		fprintf(stderr, "%d pkts in %ld.%09ld ns %ld pps\n",
			(int)dn, delta.tv_sec, delta.tv_nsec, (long)pps);
		c1 = c0;
	}
    }
	
	/*NOTREACHED*/
	freeaddrinfo(res0);
}
