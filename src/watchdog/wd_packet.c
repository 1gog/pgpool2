/*
 * $Header$
 *
 * Handles watchdog connection, and protocol communication with pgpool-II
 *
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2014	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 */
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "pool.h"
#include "utils/elog.h"
#include "utils/json_writer.h"
#include "utils/json.h"
#include "pool_config.h"
#include "watchdog/watchdog.h"
#include "watchdog/wd_ext.h"
#include "query_cache/pool_memqcache.h"

typedef enum {
	WD_SEND_TO_MASTER = 0,
	WD_SEND_WITHOUT_MASTER,
	WD_SEND_ALL_NODES
} WD_SEND_TYPE;

static int wd_send_node_packet(WD_PACKET_NO packet_no, int *node_id_set, int count);
static int wd_chk_node_mask (WD_PACKET_NO packet_no, int *node_id_set, int count);

static void * wd_thread_negotiation(void * arg);
static int send_packet_for_all(WdPacket *packet);
static int send_packet_4_nodes(WdPacket *packet, WD_SEND_TYPE type);
static int hton_wd_packet(WdPacket * to, WdPacket * from);
static int ntoh_wd_packet(WdPacket * to, WdPacket * from);
static int hton_wd_node_packet(WdPacket * to, WdPacket * from);
static int ntoh_wd_node_packet(WdPacket * to, WdPacket * from);
static int hton_wd_lock_packet(WdPacket * to, WdPacket * from);
static int ntoh_wd_lock_packet(WdPacket * to, WdPacket * from);

static char* get_wd_node_function_json(char* func_name, int *node_id_set, int count);
static char* get_wd_simple_function_json(char* func);


static char* get_wd_failover_cmd_type_json(WDFailoverCMDTypes cmdType, char* reqType);
WDFailoverCMDResults wd_send_failover_sync_command(WDFailoverCMDTypes cmdType, char* syncReqType);
static int read_socket(int socket, void* buf, int len);
static WDIPCCmdResult*
issue_command_to_watchdog(char type, WD_COMMAND_ACTIONS command_action,int timeout_sec, char* data, int data_len, bool blocking);

int
wd_startup(void)
{
	int rtn;

	/* send add request packet */
	rtn = wd_send_packet_no(WD_ADD_REQ);
	return rtn;
}

int
wd_declare(void)
{
	int rtn;

	/* send declare new master packet */
	ereport(DEBUG1,
		(errmsg("watchdog standing for master"),
			 errdetail("send the packet to declare the new master")));

	rtn = wd_send_packet_no(WD_DECLARE_NEW_MASTER);
	return rtn;
}

int
wd_stand_for_master(void)
{
	int rtn;

	/* send stand for master packet */
	ereport(DEBUG1,
		(errmsg("watchdog standing for master"),
			 errdetail("send the packet to be the new master")));

	rtn = wd_send_packet_no(WD_STAND_FOR_MASTER);
	return rtn;
}

int
wd_notice_server_down(void)
{
	int rtn;

	wd_IP_down();
	/* send notice server down packet */
	rtn = wd_send_packet_no(WD_SERVER_DOWN);
	return rtn;
}

int
wd_update_info(void)
{
	int rtn;

	/* send info request packet */
	rtn = wd_send_packet_no(WD_INFO_REQ);
	return rtn;
}

/* send authentication failed packet */
int
wd_authentication_failed(int sock)
{
	int rtn;
	WdPacket send_packet;

	memset(&send_packet, 0, sizeof(WdPacket));

	send_packet.packet_no = WD_AUTH_FAILED;
	memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));

	rtn = wd_send_packet(sock, &send_packet);

	return rtn;
}

int
wd_send_packet_no(WD_PACKET_NO packet_no )
{
	int rtn = WD_OK;
	WdPacket packet;

	memset(&packet, 0, sizeof(WdPacket));

	/* set packet no and self information */
	packet.packet_no = packet_no;
	memcpy(&(packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));

	/* send packet for all watchdogs */
	rtn = send_packet_for_all(&packet);

	return rtn;
}

int
wd_create_send_socket(char * hostname, int port)
{
	int sock;
	int one = 1;
	size_t len = 0;
	struct sockaddr_in addr;
	struct hostent * hp;

	/* create socket */
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		/* socket create failed */
		ereport(WARNING,
			(errmsg("failed to create watchdog sending socket"),
				 errdetail("create socket failed with reason: \"%s\"", strerror(errno))));
		return -1;
	}

	/* set socket option */
	if ( setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) == -1 )
	{
		close(sock);
		ereport(WARNING,
			(errmsg("failed to create watchdog sending socket"),
				 errdetail("setsockopt(TCP_NODELAY) failed with reason: \"%s\"", strerror(errno))));
		return -1;
	}
	if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one)) == -1 )
	{
		close(sock);
		ereport(WARNING,
			(errmsg("failed to create watchdog sending socket"),
				 errdetail("setsockopt(SO_KEEPALIVE) failed with reason: \"%s\"", strerror(errno))));
		return -1;
	}

	/* set sockaddr_in */
	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	hp = gethostbyname(hostname);
	if ((hp == NULL) || (hp->h_addrtype != AF_INET))
	{
		hp = gethostbyaddr(hostname,strlen(hostname),AF_INET);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			close(sock);
			ereport(WARNING,
				(errmsg("failed to create watchdog sending socket"),
					 errdetail("gethostbyname for \"%s\" failed with error: \"%s\"", hostname,hstrerror(h_errno))));
			return -1;
		}
	}
	memmove((char *)&(addr.sin_addr), (char *)hp->h_addr, hp->h_length);
	addr.sin_port = htons(port);
	len = sizeof(struct sockaddr_in);

	/* try to connect */
	for (;;)
	{
		if (connect(sock,(struct sockaddr*)&addr, len) < 0)
		{
			if (errno == EINTR)
				continue;
			else if (errno == EISCONN)
				return sock;

			ereport(LOG,
				(errmsg("failed to create watchdog sending socket"),
					 errdetail("connect() reports failure \"%s\"",strerror(errno)),
						errhint("You can safely ignore this while starting up.")));
			break;
		}
		return sock;
	}
	close(sock);
	return -1;
}

int
wd_create_recv_socket(int port)
{
    size_t	len = 0;
    struct sockaddr_in addr;
    int one = 1;
    int sock = -1;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
		/* socket create failed */
		ereport(ERROR,
			(errmsg("failed to create watchdog receive socket"),
				 errdetail("create socket failed with reason: \"%s\"", strerror(errno))));
    }
    if ( fcntl(sock, F_SETFL, O_NONBLOCK) == -1)
    {
		/* failed to set nonblock */
		close(sock);
		ereport(ERROR,
			(errmsg("failed to create watchdog receive socket"),
				 errdetail("setting non blocking mode on socket failed with reason: \"%s\"", strerror(errno))));
    }
    if ( setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one)) == -1 )
    {
		/* setsockopt(SO_REUSEADDR) failed */
		close(sock);
		ereport(ERROR,
			(errmsg("failed to create watchdog receive socket"),
				 errdetail("setsockopt(SO_REUSEADDR) failed with reason: \"%s\"", strerror(errno))));
    }
    if ( setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) == -1 )
    {
        /* setsockopt(TCP_NODELAY) failed */
		close(sock);
		ereport(ERROR,
			(errmsg("failed to create watchdog receive socket"),
				 errdetail("setsockopt(TCP_NODELAY) failed with reason: \"%s\"", strerror(errno))));
    }
    if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one)) == -1 )
    {
        /* setsockopt(SO_KEEPALIVE) failed */
		close(sock);
		ereport(ERROR,
			(errmsg("failed to create watchdog receive socket"),
				 errdetail("setsockopt(SO_KEEPALIVE) failed with reason: \"%s\"", strerror(errno))));
    }

    addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    len = sizeof(struct sockaddr_in);

    if ( bind(sock, (struct sockaddr *) & addr, len) < 0 )
    {
		/* bind failed */
		char *host = "", *serv = "";
		char hostname[NI_MAXHOST], servname[NI_MAXSERV];
		if (getnameinfo((struct sockaddr *) &addr, len, hostname, sizeof(hostname), servname, sizeof(servname), 0) == 0) {
			host = hostname;
			serv = servname;
		}
		close(sock);
		ereport(ERROR,
			(errmsg("failed to create watchdog receive socket"),
				 errdetail("bind on \"%s:%s\" failed with reason: \"%s\"", host, serv, strerror(errno))));
    }

    if ( listen(sock, MAX_WATCHDOG_NUM * 2) < 0 )
    {
		/* listen failed */
		close(sock);
		ereport(ERROR,
			(errmsg("failed to create watchdog receive socket"),
				 errdetail("listen failed with reason: \"%s\"", strerror(errno))));
    }

    return sock;
}

int
wd_accept(int sock)
{
	int fd = -1;
	fd_set rmask;
	fd_set emask;
	int rtn;
    struct sockaddr addr;
    socklen_t addrlen = sizeof(struct sockaddr);

	for (;;)
	{
		FD_ZERO(&rmask);
		FD_ZERO(&emask);
		FD_SET(sock,&rmask);
		FD_SET(sock,&emask);

		rtn = select(sock+1, &rmask, NULL, &emask, NULL );
		if ( rtn < 0 )
		{
			if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
			{
				continue;
			}
			/* connection failed */
			break;
		}
		else if ( rtn == 0 )
		{
			/* connection failed */
			break;
		}
		else if ( FD_ISSET(sock, &emask) )
		{
			/* socket exception occurred */
			break;
		}
		else if ( FD_ISSET(sock, &rmask) )
		{
			fd = accept(sock, &addr, &addrlen);
			if (fd < 0)
			{
				if ( errno == EINTR || errno == 0 || errno == EAGAIN || errno == EWOULDBLOCK )
				{
					/* nothing to accept now */
					continue;
				}
				/* accept failed */
				return -1;
			}
			return fd;
		}
	}
	return -1;
}

/*
 * Note: Since wd_send_packet() is called from wd_thread_negotiation()
 * which is a thread function and our Exception Manager
 * and Memory Manager are not thread safe so do not use
 * ereport(ERROR,..) and MemoryContextSwitchTo() functions
 * All ereports other than ereport(ERROR) that do not performs longjump
 * are fine to be used from thread function
 */
int
wd_send_packet(int sock, WdPacket * snd_pack)
{
	fd_set wmask;
	struct timeval timeout;
	char * send_ptr = NULL;
	int send_size= 0;
	int buf_size = 0;
	int s = 0;
	int flag = 0;
	int rtn;
	WdPacket buf;

	memset(&buf,0,sizeof(WdPacket));
	if ((snd_pack->packet_no >= WD_INVALID) &&
		(snd_pack->packet_no <= WD_READY ))
	{
		hton_wd_packet((WdPacket *)&buf,snd_pack);
	}
	else if ((snd_pack->packet_no >= WD_START_RECOVERY) &&
	         (snd_pack->packet_no <= WD_NODE_FAILED))
	{
		hton_wd_node_packet((WdPacket *)&buf,snd_pack);
	}
	else
	{
		hton_wd_lock_packet((WdPacket *)&buf,snd_pack);
	}

	send_ptr = (char*)&buf;
	buf_size = sizeof(WdPacket);

	for (;;)
	{
		timeout.tv_sec = WD_SEND_TIMEOUT;
		timeout.tv_usec = 0;

		FD_ZERO(&wmask);
		FD_SET(sock,&wmask);
		rtn = select(sock+1, (fd_set *)NULL, &wmask, (fd_set *)NULL, &timeout);

		if (rtn < 0 )
		{
			if (errno == EAGAIN || errno == EINTR)
			{
				continue;
			}
			return WD_NG;
		}
		else if (rtn & FD_ISSET(sock, &wmask))
		{
			s = send(sock,send_ptr + send_size,buf_size - send_size ,flag);
			if (s < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
					continue;
				else
				{
					/* send failed */
					return WD_NG;
				}
			}
			else if (s == 0)
			{
				/* send failed */
				return WD_NG;
			}
			else /* s > 0 */
			{
				send_size += s;
				if (send_size == buf_size)
				{
					return WD_OK;
				}
			}
		}
	}
	return WD_NG;
}

/*
 * Note: Since wd_recv_packet() is called from wd_thread_negotiation()
 * which is a thread function and our Exception Manager
 * and Memory Manager are not thread safe so do not use
 * ereport(ERROR,..) and MemoryContextSwitchTo() functions
 * All ereports other than ereport(ERROR) that do not performs longjump
 * are fine to be used from thread function
 */
int
wd_recv_packet(int sock, WdPacket * recv_pack)
{
	int r = 0;
	WdPacket buf;
	char * read_ptr = (char *)&buf;
	int read_size = 0;
	int len = sizeof(WdPacket);

	memset(&buf,0,sizeof(WdPacket));
	for (;;)
	{
		r = recv(sock,read_ptr + read_size ,len - read_size, 0);
		if (r < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			else
            {
				ereport(WARNING,
					(errmsg("watchdog failed to receive packet"),
						 errdetail("recv() failed with reason: \"%s\"", strerror(errno))));
				return WD_NG;
            }
		}
		else if (r > 0)
		{
			read_size += r;
			if (read_size == len)
			{

				if (ntohl(buf.packet_no) <= WD_READY)
				{
					ntoh_wd_packet(recv_pack,&buf);
				}
				else if (ntohl((buf.packet_no) >= WD_START_RECOVERY) &&
	         			(ntohl(buf.packet_no) <= WD_NODE_FAILED))
				{
					ntoh_wd_node_packet(recv_pack,&buf);
				}
				else
				{
					ntoh_wd_lock_packet(recv_pack,&buf);
				}

				return WD_OK;
			}
		}
		else /* r == 0 */
		{
			return WD_NG;
		}
	}
	return WD_NG;
}

static void *
wd_thread_negotiation(void * arg)
{
	WdPacketThreadArg * thread_arg;
	int sock;
	uintptr_t rtn;
	WdPacket recv_packet;
	WdInfo * p;
	char pack_str[WD_MAX_PACKET_STRING];
	int pack_str_len;

	thread_arg = (WdPacketThreadArg *)arg;
	sock = thread_arg->sock;
	p = thread_arg->target;
	gettimeofday(&(thread_arg->packet->send_time), NULL);

	if (strlen(pool_config->wd_authkey))
	{
		/* calculate hash from packet */
		pack_str_len = wd_packet_to_string(thread_arg->packet, pack_str, sizeof(pack_str));
		wd_calc_hash(pack_str, pack_str_len, thread_arg->packet->hash);
	}

	/* packet send to target watchdog */
	rtn = (uintptr_t)wd_send_packet(sock, thread_arg->packet);
	if (rtn != WD_OK)
	{
		close(sock);
		ereport(WARNING,
			(errmsg("watchdog negotiation failed"),
				 errdetail("failed to send watchdog packet to \"%s:%d\"", p->hostname, p->wd_port)));
		pthread_exit((void *)rtn);
	}

	/* receive response packet */
	memset(&recv_packet,0,sizeof(WdPacket));
	rtn = (uintptr_t)wd_recv_packet(sock, &recv_packet);
	if (rtn != WD_OK)
	{
		p = thread_arg->target;
		close(sock);
		ereport(WARNING,
			(errmsg("watchdog negotiation failed"),
				 errdetail("failed to receive watchdog packet from \"%s:%d\"", p->hostname, p->wd_port)));
		pthread_exit((void *)rtn);
	}
	rtn = WD_OK;

	switch (thread_arg->packet->packet_no)
	{
		case WD_ADD_REQ:
			if (recv_packet.packet_no == WD_ADD_ACCEPT)
			{
				memcpy(thread_arg->target, &(recv_packet.wd_body.wd_info),sizeof(WdInfo));
			}
			else
			{
				p = &(recv_packet.wd_body.wd_info);
				if(recv_packet.packet_no == WD_ADD_REJECT)
				{
					ereport(WARNING,
						(errmsg("watchdog negotiation failed"),
							errdetail("watchdog add request is rejected by pgpool-II on %s:%d",
								   p->hostname, p->pgpool_port)));
				}
				else
					ereport(WARNING,
						(errmsg("watchdog negotiation failed"),
							 errdetail("invalid response received for watchdog add request from pgpool-II on %s:%d",
									   p->hostname, p->pgpool_port)));

				rtn = WD_NG;
			}
			break;
		case WD_STAND_FOR_MASTER:
			if (recv_packet.packet_no == WD_MASTER_EXIST)
			{
				p = &(recv_packet.wd_body.wd_info);
				wd_set_wd_info(p);
				rtn = WD_NG;
			}
			break;
		case WD_STAND_FOR_LOCK_HOLDER:
		case WD_DECLARE_LOCK_HOLDER:
			if (recv_packet.packet_no == WD_LOCK_HOLDER_EXIST)
			{
				rtn = WD_NG;
			}
			break;
		case WD_DECLARE_NEW_MASTER:
		case WD_RESIGN_LOCK_HOLDER:

			if (recv_packet.packet_no != WD_READY)
			{
				rtn = WD_NG;
			}
			break;
		case WD_START_RECOVERY:
		case WD_FAILBACK_REQUEST:
		case WD_DEGENERATE_BACKEND:
		case WD_PROMOTE_BACKEND:
			rtn = (recv_packet.packet_no == WD_NODE_FAILED) ? WD_NG : WD_OK;
			break;
		case WD_UNLOCK_REQUEST:
			rtn = (recv_packet.packet_no == WD_LOCK_FAILED) ? WD_NG : WD_OK;
			break;
		case WD_AUTH_FAILED:
			ereport(WARNING,
				(errmsg("watchdog negotiation failed"),
					 errdetail("watchdog authentication failed")));
			rtn = WD_NG;
			break;
		default:
			break;
	}
	close(sock);
	pthread_exit((void *)rtn);
}

static int
send_packet_for_all(WdPacket *packet)
{
	int rtn = WD_OK;

	/* send packet to master watchdog */
	if (WD_MYSELF->status != WD_MASTER)
		rtn = send_packet_4_nodes(packet, WD_SEND_TO_MASTER );

	/* send packet to other watchdogs */
	if (rtn == WD_OK)
	{
		rtn = send_packet_4_nodes(packet, WD_SEND_WITHOUT_MASTER);
	}

	return rtn;
}

static int
send_packet_4_nodes(WdPacket *packet, WD_SEND_TYPE type)
{
	int rtn;
	WdInfo * p = WD_List;
	int i,cnt;
	int sock;
	int rc;
	pthread_attr_t attr;
	pthread_t thread[MAX_WATCHDOG_NUM];
	WdPacketThreadArg thread_arg[MAX_WATCHDOG_NUM];

	if (packet == NULL)
	{
		return WD_NG;
	}

	/* thread init */
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	/* skip myself */
	p++;
	WD_MYSELF->is_contactable = true;

	/* send packet to other watchdogs */
	cnt = 0;
	while (p->status != WD_END)
	{
		/* don't send packet to pgpool in down */
		if (p->status == WD_DOWN ||
		    (packet->packet_no != WD_ADD_REQ && p->status == WD_INIT))
		{
			p->is_contactable = false;
			p++;
			continue;
		}

		if (type == WD_SEND_TO_MASTER )
		{
			if (p->status != WD_MASTER)
			{
				p++;
				continue;
			}
		}
		else if (type == WD_SEND_WITHOUT_MASTER )
		{
			if (p->status == WD_MASTER)
			{
				p++;
				continue;
			}
		}

		sock = wd_create_send_socket(p->hostname, p->wd_port);
		if (sock == -1)
		{
			ereport(LOG,
				(errmsg("watchdog sending packet for nodes"),
					 errdetail("packet for \"%s:%d\" is canceled", p->hostname, p->wd_port)));
			p->is_contactable = false;
			p++;
			continue;
		}
		else
		{
			p->is_contactable = true;
		}

		thread_arg[cnt].sock = sock;
		thread_arg[cnt].target = p;
		thread_arg[cnt].packet = packet;
		rc = watchdog_thread_create(&thread[cnt], &attr, wd_thread_negotiation, (void*)&thread_arg[cnt]);

		cnt ++;
		p++;
	}

	pthread_attr_destroy(&attr);

	/* no packet is sent */
	if (cnt == 0)
	{
		return WD_OK;
	}

	/* default return value */
	if ((packet->packet_no == WD_STAND_FOR_MASTER) ||
		(packet->packet_no == WD_STAND_FOR_LOCK_HOLDER) ||
		(packet->packet_no == WD_DECLARE_LOCK_HOLDER) ||
		(packet->packet_no == WD_START_RECOVERY))
	{
		rtn = WD_OK;
	}
	else
	{
		rtn = WD_NG;
	}

	/* receive the results */
	for (i=0; i<cnt; )
	{
		int result;
		rc = pthread_join(thread[i], (void **)&result);
		if ((rc != 0) && (errno == EINTR))
		{
			usleep(100);
			continue;
		}

		/*  aggregate results according to the packet type */
		if ((packet->packet_no == WD_STAND_FOR_MASTER) ||
			(packet->packet_no == WD_STAND_FOR_LOCK_HOLDER) ||
			(packet->packet_no == WD_DECLARE_LOCK_HOLDER) ||
			(packet->packet_no == WD_START_RECOVERY))
		{
			/* if any result is NG then return NG */
			if (result == WD_NG)
			{
				rtn = WD_NG;
			}
		}

		else
		{
			/* if any result is OK then return OK */
			if (result == WD_OK)
			{
				rtn = WD_OK;
			}
		}
		i++;
	}

	return rtn;
}

static int
hton_wd_packet(WdPacket * to, WdPacket * from)
{
	WdInfo * to_info = NULL;
	WdInfo * from_info = NULL;

	if ((to == NULL) || (from == NULL))
	{
		return WD_NG;
	}

	to_info = &(to->wd_body.wd_info);
	from_info = &(from->wd_body.wd_info);

	to->packet_no = htonl(from->packet_no);
	to->send_time.tv_sec = htonl(from->send_time.tv_sec);
	to->send_time.tv_usec = htonl(from->send_time.tv_usec);

	memcpy(to->hash, from->hash, sizeof(to->hash));

	to_info->status = htonl(from_info->status);
	to_info->tv.tv_sec = htonl(from_info->tv.tv_sec);
	to_info->tv.tv_usec = htonl(from_info->tv.tv_usec);
	to_info->pgpool_port = htonl(from_info->pgpool_port);
	to_info->wd_port = htonl(from_info->wd_port);

	memcpy(to_info->hostname, from_info->hostname, sizeof(to_info->hostname));
	memcpy(to_info->delegate_ip, from_info->delegate_ip, sizeof(to_info->delegate_ip));

	return WD_OK;
}

static int
ntoh_wd_packet(WdPacket * to, WdPacket * from)
{
	WdInfo * to_info = NULL;
	WdInfo * from_info = NULL;

	if ((to == NULL) || (from == NULL))
	{
		return WD_NG;
	}

	to_info = &(to->wd_body.wd_info);
	from_info = &(from->wd_body.wd_info);

	to->packet_no = ntohl(from->packet_no);
	to->send_time.tv_sec = ntohl(from->send_time.tv_sec);
	to->send_time.tv_usec = ntohl(from->send_time.tv_usec);

	memcpy(to->hash, from->hash, sizeof(to->hash));

	to_info->status = ntohl(from_info->status);
	to_info->tv.tv_sec = ntohl(from_info->tv.tv_sec);
	to_info->tv.tv_usec = ntohl(from_info->tv.tv_usec);
	to_info->pgpool_port = ntohl(from_info->pgpool_port);
	to_info->wd_port = ntohl(from_info->wd_port);

	memcpy(to_info->hostname, from_info->hostname, sizeof(to_info->hostname));
	memcpy(to_info->delegate_ip, from_info->delegate_ip, sizeof(to_info->delegate_ip));

	return WD_OK;
}

static int
hton_wd_node_packet(WdPacket * to, WdPacket * from)
{
	WdNodeInfo * to_info = NULL;
	WdNodeInfo * from_info = NULL;
	int i;

	if ((to == NULL) || (from == NULL))
	{
		return WD_NG;
	}

	to_info = &(to->wd_body.wd_node_info);
	from_info = &(from->wd_body.wd_node_info);

	to->packet_no = htonl(from->packet_no);
	to->send_time.tv_sec = htonl(from->send_time.tv_sec);
	to->send_time.tv_usec = htonl(from->send_time.tv_usec);

	memcpy(to->hash, from->hash, sizeof(to->hash));

	to_info->node_num = htonl(from_info->node_num);

	for (i = 0 ; i < from_info->node_num ; i ++)
	{
		to_info->node_id_set[i] = htonl(from_info->node_id_set[i]);
	}

	return WD_OK;
}

static int
ntoh_wd_node_packet(WdPacket * to, WdPacket * from)
{
	WdNodeInfo * to_info = NULL;
	WdNodeInfo * from_info = NULL;
	int i;

	if ((to == NULL) || (from == NULL))
	{
		return WD_NG;
	}

	to_info = &(to->wd_body.wd_node_info);
	from_info = &(from->wd_body.wd_node_info);

	to->packet_no = ntohl(from->packet_no);
	to->send_time.tv_sec = ntohl(from->send_time.tv_sec);
	to->send_time.tv_usec = ntohl(from->send_time.tv_usec);

	memcpy(to->hash, from->hash, sizeof(to->hash));

	to_info->node_num = ntohl(from_info->node_num);

	for (i = 0 ; i < to_info->node_num ; i ++)
	{
		to_info->node_id_set[i] = ntohl(from_info->node_id_set[i]);
	}

	return WD_OK;
}

static int
hton_wd_lock_packet(WdPacket * to, WdPacket * from)
{
	WdLockInfo * to_info = NULL;
	WdLockInfo * from_info = NULL;

	if ((to == NULL) || (from == NULL))
	{
		return WD_NG;
	}

	to_info = &(to->wd_body.wd_lock_info);
	from_info = &(from->wd_body.wd_lock_info);

	to->packet_no = htonl(from->packet_no);
	to->send_time.tv_sec = htonl(from->send_time.tv_sec);
	to->send_time.tv_usec = htonl(from->send_time.tv_usec);

	memcpy(to->hash, from->hash, sizeof(to->hash));

	to_info->lock_id = htonl(from_info->lock_id);

	return WD_OK;
}

static int
ntoh_wd_lock_packet(WdPacket * to, WdPacket * from)
{
	WdLockInfo * to_info = NULL;
	WdLockInfo * from_info = NULL;

	if ((to == NULL) || (from == NULL))
	{
		return WD_NG;
	}

	to_info = &(to->wd_body.wd_lock_info);
	from_info = &(from->wd_body.wd_lock_info);

	to->packet_no = ntohl(from->packet_no);
	to->send_time.tv_sec = ntohl(from->send_time.tv_sec);
	to->send_time.tv_usec = ntohl(from->send_time.tv_usec);

	memcpy(to->hash, from->hash, sizeof(to->hash));

	to_info->lock_id = ntohl(from_info->lock_id);

	return WD_OK;
}

int
wd_escalation(void)
{
	int  r;
	bool has_error = false;

	ereport(LOG,
		(errmsg("watchdog escalation"),
			 errdetail("escalating to master pgpool")));

	/* clear shared memory cache */
	if (pool_config->memory_cache_enabled && pool_is_shmem_cache() &&
	    pool_config->clear_memqcache_on_escalation)
	{
		ereport(LOG,
			(errmsg("watchdog escalation"),
				 errdetail("clearing all the query cache on shared memory")));

		pool_clear_memory_cache();
	}

	/* execute escalation command */
	if (strlen(pool_config->wd_escalation_command))
	{
		r = system(pool_config->wd_escalation_command);
		if (WIFEXITED(r))
		{
			if (WEXITSTATUS(r) == EXIT_SUCCESS)
				ereport(LOG,
					(errmsg("watchdog escalation successful")));
			else
			{
				ereport(WARNING,
						(errmsg("watchdog escalation command failed with exit status: %d", WEXITSTATUS(r))));
				has_error = true;
			}
		}
		else
		{
			ereport(WARNING,
				(errmsg("watchdog escalation command exit abnormally")));
			has_error = true;
		}
	}

	/* interface up as delegate IP */
	if (strlen(pool_config->delegate_IP) != 0)
	{
		r = wd_IP_up();
		if (r == WD_NG)
			has_error = true;
	}
	if (has_error)
		ereport(NOTICE,
			(errmsg("watchdog escalation successful, escalated to master pgpool with some errors")));
	else
		ereport(LOG,
			(errmsg("watchdog escalation successful, escalated to master pgpool")));
	return WD_OK;
}

static char* get_wd_simple_function_json(char* func)
{
	char* json_str;
	JsonNode* jNode = jw_create_with_object(true);
	jw_put_string(jNode, "Function", func);
	jw_finish_document(jNode);
	json_str = pstrdup(jw_get_json_string(jNode));
	jw_destroy(jNode);
	return json_str;
}

static char* get_wd_node_function_json(char* func_name, int *node_id_set, int count)
{
	char* json_str;
	int  i;
	JsonNode* jNode = jw_create_with_object(true);

	jw_put_string(jNode, "Function", func_name);
	jw_put_int(jNode, "NodeCount", count);
	jw_start_array(jNode, "NodeIdList");
	for (i=0; i < count; i++) {
		jw_put_int_value(jNode, node_id_set[i]);
	}
	jw_end_element(jNode);
	jw_finish_document(jNode);
	json_str = pstrdup(jw_get_json_string(jNode));
	jw_destroy(jNode);
	printf("\n%s\n",json_str);
	return json_str;
}

WdCommandResult
wd_start_recovery(void)
{
	char type;
	char* func = get_wd_simple_function_json(WD_FUNCTION_START_RECOVERY);

	WDIPCCmdResult *result = issue_command_to_watchdog(WD_FUNCTION_COMMAND, WD_COMMAND_ACTION_DEFAULT,pool_config->recovery_timeout, func, strlen(func), true);
	pfree(func);

	if (result == NULL)
	{
		ereport(LOG,
			(errmsg("start recovery command lock failed"),
				 errdetail("issue command to eatchdog returned NULL")));
		return COMMAND_FAILED;
	}
	
	type = result->type;
	pfree(result);
	if (type == WD_IPC_CMD_CLUSTER_IN_TRAN)
	{
		ereport(LOG,
			(errmsg("start recovery command lock failed"),
				 errdetail("watchdog cluster is not in stable state"),
					errhint("try again when the cluster is fully initialized")));
		return CLUSTER_IN_TRANSATIONING;
	}
	if (type == WD_IPC_CMD_RESULT_OK)
		return COMMAND_OK;

	return COMMAND_FAILED;
}

WdCommandResult
wd_end_recovery(void)
{
	char type;
	char* func = get_wd_simple_function_json(WD_FUNCTION_END_RECOVERY);
	
	WDIPCCmdResult *result = issue_command_to_watchdog(WD_FUNCTION_COMMAND, WD_COMMAND_ACTION_DEFAULT,2, func, strlen(func), true);
	pfree(func);

	if (result == NULL)
	{
		ereport(LOG,
				(errmsg("start recovery command lock failed"),
				 errdetail("issue command to eatchdog returned NULL")));
		return COMMAND_FAILED;
	}
	
	type = result->type;
	pfree(result);
	if (type == WD_IPC_CMD_CLUSTER_IN_TRAN)
	{
		ereport(LOG,
				(errmsg("start recovery command lock failed"),
				 errdetail("watchdog cluster is not in stable state"),
					errhint("try again when the cluster is fully initialized")));
		return CLUSTER_IN_TRANSATIONING;
	}
	if (type == WD_IPC_CMD_RESULT_OK)
		return COMMAND_OK;
	
	return COMMAND_FAILED;
}


WdCommandResult
wd_send_failback_request(int node_id)
{
	int n = node_id;
	char type;
	char* func;


	/* if failback packet is received already, do nothing */
	if (wd_chk_node_mask(WD_FAILBACK_REQUEST,&n,1))
		return COMMAND_OK;

	func = get_wd_node_function_json(WD_FUNCTION_FAILBACK_REQUEST,&n, 1);
	WDIPCCmdResult *result = issue_command_to_watchdog(WD_FUNCTION_COMMAND, WD_COMMAND_ACTION_DEFAULT,2, func, strlen(func), true);
	pfree(func);
	
	if (result == NULL)
	{
		ereport(LOG,
				(errmsg("start recovery command lock failed"),
				 errdetail("issue command to eatchdog returned NULL")));
		return COMMAND_FAILED;
	}
	
	type = result->type;
	pfree(result);
	if (type == WD_IPC_CMD_CLUSTER_IN_TRAN)
	{
		ereport(LOG,
				(errmsg("start recovery command lock failed"),
				 errdetail("watchdog cluster is not in stable state"),
					errhint("try again when the cluster is fully initialized")));
		return CLUSTER_IN_TRANSATIONING;
	}
	if (type == WD_IPC_CMD_RESULT_OK)
		return COMMAND_OK;
	
	return COMMAND_FAILED;
}
static char* get_wd_failover_cmd_type_json(WDFailoverCMDTypes cmdType, char* reqType)
{
	char* json_str;
	JsonNode* jNode = jw_create_with_object(true);

	jw_put_int(jNode, "FailoverCMDType", cmdType);
	jw_put_string(jNode, "SyncRequestType", reqType);
	jw_finish_document(jNode);
	json_str = pstrdup(jw_get_json_string(jNode));
	jw_destroy(jNode);
	printf("\n%s\n",json_str);
	return json_str;
}


WDFailoverCMDResults
wd_send_failover_sync_command(WDFailoverCMDTypes cmdType, char* syncReqType)
{
	int failoverResCmdType;
	int interlockingResult;
	json_value *root;
	
	char* json_data = get_wd_failover_cmd_type_json(cmdType, syncReqType);
	
	WDIPCCmdResult *result = issue_command_to_watchdog(WD_FAILOVER_CMD_SYNC_REQUEST, WD_COMMAND_ACTION_DEFAULT,pool_config->recovery_timeout, json_data, strlen(json_data), true);

	if (result == NULL || result->length <= 0)
	{
		ereport(LOG,
			(errmsg("start recovery command lock failed"),
				 errdetail("issue command to eatchdog returned NULL")));
		return FAILOVER_RES_ERROR;
	}

	printf("RESULT DATA LEN = %d data = \"%s\"\n",result->length,result->data);

	root = json_parse(result->data,result->length);
	/* The root node must be object */
	if (root == NULL || root->type != json_object)
	{
		ereport(NOTICE,
				(errmsg("unable to parse json data from replicate command")));
		return FAILOVER_RES_ERROR;
	}

	if (json_get_int_value_for_key(root, "FailoverCMDType", &failoverResCmdType))
	{
		json_value_free(root);
		return FAILOVER_RES_ERROR;
	}
	if (root && json_get_int_value_for_key(root, "InterlockingResult", &interlockingResult))
	{
		json_value_free(root);
		return FAILOVER_RES_ERROR;
	}
	json_value_free(root);
	pfree(result);

	if (failoverResCmdType != cmdType)
		return FAILOVER_RES_ERROR;

	if (interlockingResult < 0 || interlockingResult > FAILOVER_RES_BLOCKED)
		return FAILOVER_RES_ERROR;

	return interlockingResult;
}

WdCommandResult
wd_try_command_lock(void)
{
	WDIPCCmdResult* result;
	char type;

	result = issue_command_to_watchdog(WD_TRY_COMMAND_LOCK, WD_COMMAND_ACTION_DEFAULT,10, NULL, 0, true);
	if (result == NULL)
	{
		ereport(LOG,
			(errmsg("watchdog command lock failed"),
				 errdetail("issue command to eatchdog returned NULL")));
		return COMMAND_FAILED;
	}

	type = result->type;
	pfree(result);
	if (type == WD_IPC_CMD_CLUSTER_IN_TRAN)
	{
		ereport(LOG,
			(errmsg("watchdog command lock failed"),
				 errdetail("watchdog cluster is not in stable state"),
					errhint("try again when the cluster is fully initialized")));
		return CLUSTER_IN_TRANSATIONING;
	}

	if (type == WD_IPC_CMD_RESULT_OK)
		return COMMAND_OK;

	return COMMAND_FAILED;
}

void wd_command_unlock(void)
{
	/* we dont really care about results here */
	issue_command_to_watchdog(WD_COMMAND_UNLOCK, WD_COMMAND_ACTION_DEFAULT,10, NULL, 0, false);
}

WdCommandResult
wd_degenerate_backend_set(int *node_id_set, int count)
{
	char type;
	char* func;
	
	
	/* if failback packet is received already, do nothing */
	if (wd_chk_node_mask(WD_DEGENERATE_BACKEND,node_id_set,count))
		return COMMAND_OK;

	func = get_wd_node_function_json(WD_FUNCTION_DEGENERATE_REQUEST,node_id_set, count);
	WDIPCCmdResult *result = issue_command_to_watchdog(WD_FUNCTION_COMMAND, WD_COMMAND_ACTION_DEFAULT,2, func, strlen(func), true);
	pfree(func);
	
	if (result == NULL)
	{
		ereport(LOG,
				(errmsg("degenerate backend set command failed"),
				 errdetail("issue command to eatchdog returned NULL")));
		return COMMAND_FAILED;
	}

	type = result->type;
	pfree(result);
	if (type == WD_IPC_CMD_CLUSTER_IN_TRAN)
	{
		ereport(LOG,
				(errmsg("degenerate backend set command failed"),
				 errdetail("watchdog cluster is not in stable state"),
					errhint("try again when the cluster is fully initialized")));
		return CLUSTER_IN_TRANSATIONING;
	}
	if (type == WD_IPC_CMD_RESULT_OK)
		return COMMAND_OK;

	return COMMAND_FAILED;
}

WdCommandResult
wd_promote_backend(int node_id)
{
	int n = node_id;
	char type;
	char* func;
	
	/* if promote packet is received already, do nothing */
	if (wd_chk_node_mask(WD_PROMOTE_BACKEND,&n,1))
		return COMMAND_OK;

	func = get_wd_node_function_json(WD_FUNCTION_PROMOTE_REQUEST,&n, 1);
	WDIPCCmdResult *result = issue_command_to_watchdog(WD_FUNCTION_COMMAND, WD_COMMAND_ACTION_DEFAULT,2, func, strlen(func), true);
	pfree(func);
	
	if (result == NULL)
	{
		ereport(LOG,
				(errmsg("start recovery command lock failed"),
				 errdetail("issue command to eatchdog returned NULL")));
		return COMMAND_FAILED;
	}

	type = result->type;
	pfree(result);
	if (type == WD_IPC_CMD_CLUSTER_IN_TRAN)
	{
		ereport(LOG,
				(errmsg("start recovery command lock failed"),
				 errdetail("watchdog cluster is not in stable state"),
					errhint("try again when the cluster is fully initialized")));
		return CLUSTER_IN_TRANSATIONING;
	}
	if (type == WD_IPC_CMD_RESULT_OK)
		return COMMAND_OK;
	
	return COMMAND_FAILED;
}

int
open_wd_command_sock(bool throw_error)
{
	size_t	len;
	struct sockaddr_un addr;
	int sock = -1;

	/* We use unix domain stream sockets for the purpose */
	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
	{
		/* socket create failed */
		ereport(throw_error? ERROR:LOG,
			(errmsg("failed to connect to watchdog command server socket"),
				 errdetail("connect on \"%s\" failed with reason: \"%s\"", addr.sun_path, strerror(errno))));
		printf("failed to connect to watchdog command server socket\nconnect on \"%s\" failed with reason: \"%s\"\n", addr.sun_path, strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path),"%s",watchdog_ipc_address);
	len = sizeof(struct sockaddr_un);

	if (connect(sock, (struct sockaddr *) &addr, len) == -1)
	{
		close(sock);
		ereport(throw_error? ERROR:LOG,
			(errmsg("failed to connect to watchdog command server socket"),
				 errdetail("connect on \"%s\" failed with reason: \"%s\"", addr.sun_path, strerror(errno))));
		printf("failed to connect to watchdog command server socket\nconnect on \"%s\" failed with reason: \"%s\"\n", addr.sun_path, strerror(errno));
		return -1;
	}
	return sock;
}


static WDIPCCmdResult*
issue_command_to_watchdog(char type, WD_COMMAND_ACTIONS command_action,int timeout_sec, char* data, int data_len, bool blocking)
{
	struct timeval start_time,tv;
	int sock;
	WDIPCCmdResult* result = NULL;
	char res_type = 'P';
	int res_length;
	printf("\t\t\t %s:%d\n",__FUNCTION__,__LINE__);
	gettimeofday(&start_time, NULL);
	
	/* open the watchdog command socket for IPC */
	sock = open_wd_command_sock(false);
	if (sock < 0)
	{
		printf("\t\t\t %s:%d\n",__FUNCTION__,__LINE__);
		return NULL;
	}
	if (send(sock,&type,1,0) < 1)
	{
		printf("\t\t\t %s:%d\n",__FUNCTION__,__LINE__);
		close(sock);
		return NULL;
	}
	/*
	 * since the command action will be consumed locally,
	 * so no need to convert it to network byte order
	 */
	res_length = htonl(data_len);
	send(sock,&command_action,sizeof(command_action),0);
	send(sock,&res_length,sizeof(int),0);
	if (data && data_len > 0)
		send(sock,data,data_len,0);

	if (blocking)
	{
		/* if we are asked to wait for results */
		fd_set fds;
		struct timeval *timeout_st = NULL;
		if (timeout_sec > 0)
		{
			tv.tv_sec = timeout_sec;
			timeout_st = &tv;
		}
		FD_ZERO(&fds);
		FD_SET(sock,&fds);
		for (;;)
		{
			int select_res;
			select_res = select(sock+1,&fds,NULL,NULL,timeout_st);
			if (select_res > 0)
			{
				printf("\t\t\t %s:%d\n",__FUNCTION__,__LINE__);

				int ret;
				/* read the result type char */
				ret = read_socket(sock, &res_type, 1);
				printf("\t\t\t %s:%d ret = %d\n",__FUNCTION__,__LINE__,ret);

				if (ret != 1)
				{
					ereport(DEBUG1,
						(errmsg("error reading from IPC command socket"),
							 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
					close(sock);
					return result;
				}
				printf("\t\t\t %s:%d RESULT TYPE = %c %02X\n",__FUNCTION__,__LINE__,res_type,res_type);

				/* read the result data length */
				ret = read_socket(sock, &res_length, 4);
				if (ret != 4)
				{
					ereport(DEBUG1,
						(errmsg("error reading from IPC command socket"),
							 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
					close(sock);
					return result;
				}
				result = palloc(sizeof(WDIPCCmdResult));
				result->type = res_type;
				result->length = ntohl(res_length);
				result->data = NULL;

				if (result->length > 0)
				{
					int ret;
					/* read the result data length */
					result->data = palloc(result->length);
					ret = read_socket(sock, result->data, result->length);
					if (ret != result->length)
					{
						pfree(result->data);
						pfree(result);
						ereport(DEBUG1,
							(errmsg("error reading from IPC command socket"),
								 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
						close(sock);
						return NULL;
					}
				}
				break;
			}
		}
	}
	close(sock);
	return result;
}


WDIPCCommandResult*
issue_wd_command(char type, WD_COMMAND_ACTIONS command_action,int timeout_sec, char* data, int data_len, bool blocking)
{
	struct timeval start_time,tv;
	int sock;
	gettimeofday(&start_time, NULL);
	/* open the watchdog command socket for IPC */
	sock = open_wd_command_sock(false);
	if (sock < 0)
		return NULL;

	if (send(sock,&type,1,0) < 1)
	{
		close(sock);
		return NULL;
	}
	/*
	 * since the command action will be consumed locally,
	 * so no need to convert it to network byte order
	 */
	send(sock,&command_action,sizeof(command_action),0);
	send(sock,&data_len,sizeof(int),0);
	if (data && data_len > 0)
		send(sock,data,data_len,0);

	if (blocking)
	{
		/* if the command is blocking */
		fd_set fds;
		struct timeval *timeout_st = NULL;
		if (timeout_sec > 0)
		{
			tv.tv_sec = timeout_sec;
			timeout_st = &tv;
		}
		FD_ZERO(&fds);
		FD_SET(sock,&fds);
		for (;;)
		{

			int select_res;
			select_res = select(sock+1,&fds,NULL,NULL,timeout_st);
			if (select_res > 0)
			{
				
				/* read the result */

				int ret;
				int header_size = sizeof(char) + sizeof(int) + sizeof(int) + sizeof(int);
				WDIPCCommandResult* command_res = palloc(sizeof(WDIPCCommandResult));
				ret = read_socket(sock, command_res, header_size);
				if (ret != header_size)
				{
					ereport(DEBUG1,
						(errmsg("error reading from IPC command socket"),
							 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
					close(sock);
					return NULL;
				}
				command_res->node_results = NULL;
				if (command_res->resultSlotsCount > 0)
				{
					int i;
					/* get all the result slots */
					for (i =0 ; i < command_res->resultSlotsCount; i++)
					{
						WDIPCCommandNodeResultData* resultSlot = palloc(sizeof(WDIPCCommandNodeResultData));
						ret = read_socket(sock, &(resultSlot->node_id), sizeof(resultSlot->node_id));
						if (ret != sizeof(resultSlot->node_id))
						{
							ereport(LOG,
								(errmsg("error reading from IPC command socket"),
									 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
							close(sock);
							return NULL;
						}
						ret = read_socket(sock, resultSlot->nodeName, sizeof(resultSlot->nodeName));
						if (ret != sizeof(resultSlot->nodeName))
						{
							ereport(LOG,
								(errmsg("error reading from IPC command socket"),
									 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
							close(sock);
							return NULL;
						}
						ret = read_socket(sock, &(resultSlot->data_len), sizeof(resultSlot->data_len));
						if (ret != sizeof(resultSlot->data_len))
						{
							ereport(LOG,
								(errmsg("error reading from IPC command socket"),
									 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
							close(sock);
							return NULL;
						}
						if (resultSlot->data_len > 0)
						{
							resultSlot->data = palloc(resultSlot->data_len);
							ret = read_socket(sock, resultSlot->data, resultSlot->data_len);
							if (ret != resultSlot->data_len)
							{
								ereport(LOG,
									(errmsg("error reading from IPC command socket"),
										 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
								close(sock);
								return NULL;
							}
						}
						command_res->node_results = lappend(command_res->node_results,resultSlot);
					}
				}
				close(sock);
				return command_res;
			}
			else if (select_res == 0) /* timeout */
			{
				close(sock);
				return NULL;
			}
		}
	}
	close(sock);
	return NULL;
}

static int read_socket(int socket, void* buf, int len)
{
	int read_len = 0;
	while (read_len < len)
	{
		int nret;
		nret =  read(socket, buf + read_len, len - read_len);
		if (nret <= 0)
			return nret;
		read_len +=nret;
	}
	return read_len;
}


static int
wd_send_node_packet(WD_PACKET_NO packet_no, int *node_id_set, int count)
{
	int rtn = 0;
	WdPacket packet;

	memset(&packet, 0, sizeof(WdPacket));
	/* set add request packet */
	packet.packet_no = packet_no;
	memcpy(packet.wd_body.wd_node_info.node_id_set,node_id_set,sizeof(int)*count);
	packet.wd_body.wd_node_info.node_num = count;

	/* send packet to all watchdogs */
	rtn = send_packet_for_all(&packet);

	return rtn;
}

int
wd_send_lock_packet(WD_PACKET_NO packet_no,  WD_LOCK_ID lock_id)
{
	int rtn = 0;
	WdPacket packet;

	memset(&packet, 0, sizeof(WdPacket));

	/* set add request packet */
	packet.packet_no = packet_no;
	packet.wd_body.wd_lock_info.lock_id= lock_id;

	/* send packet to all watchdogs */
	rtn = send_packet_for_all(&packet);

	return rtn;
}

/* check mask, and if maskted return 1 and clear it, otherwise return 0 */
static int
wd_chk_node_mask (WD_PACKET_NO packet_no, int *node_id_set, int count)
{
	int rtn = 0;
	unsigned char mask = 0;
	int i;
	int offset = 0;
	mask = 1 << (packet_no - WD_START_RECOVERY);
	for ( i = 0 ; i < count ; i ++)
	{
		offset = *(node_id_set+i);
		if ((*(WD_Node_List + offset) & mask) != 0)
		{
			*(WD_Node_List + offset) ^= mask;
			rtn = 1;
		}
	}
	return rtn;
}

/* set mask */
int
wd_set_node_mask (WD_PACKET_NO packet_no, int *node_id_set, int count)
{
	int rtn = 0;
	unsigned char mask = 0;
	int i;
	int offset = 0;
	mask = 1 << (packet_no - WD_START_RECOVERY);
	for ( i = 0 ; i < count ; i ++)
	{
		offset = *(node_id_set+i);
		*(WD_Node_List + offset) |= mask;
	}
	return rtn;
}

/* calculate hash for authentication using packet contents */
void
wd_calc_hash(const char *str, int len, char *buf)
{
	char pass[(MAX_PASSWORD_SIZE + 1) / 2];
	char username[(MAX_PASSWORD_SIZE + 1) / 2];
	size_t pass_len;
	size_t username_len;
	size_t authkey_len;

	/* use first half of authkey as username, last half as password */
	authkey_len = strlen(pool_config->wd_authkey);

	username_len = authkey_len / 2;
	pass_len = authkey_len - username_len;
	snprintf(username, username_len + 1, "%s", pool_config->wd_authkey);
	snprintf(pass, pass_len + 1, "%s", pool_config->wd_authkey + username_len);

	/* calculate hash using md5 encrypt */
	pool_md5_encrypt(pass, username, strlen(username), buf + MD5_PASSWD_LEN + 1);
	buf[(MD5_PASSWD_LEN+1)*2-1] = '\0';

	pool_md5_encrypt(buf+MD5_PASSWD_LEN+1, str, len, buf);
	buf[MD5_PASSWD_LEN] = '\0';
}

int
wd_packet_to_string(WdPacket *pkt, char *str, int maxlen)
{
	int len;

	len = snprintf(str, maxlen, "no=%d tv_sec=%ld tv_usec=%ld",
	               pkt->packet_no, pkt->send_time.tv_sec, pkt->send_time.tv_usec);

	return len;
}

