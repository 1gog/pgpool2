/*
 * $Header$
 *
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2008	PgPool Global Development Group
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
 * Client program to send "node info" command.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
#include "utils/getopt_long.h"
#endif

#include "pcp/pcp.h"

static void usage(void);
static void myexit(PCPConnInfo* pcpConn);

int
main(int argc, char **argv)
{
	char host[MAX_DB_HOST_NAMELEN];
	int port;
	char user[MAX_USER_PASSWD_LEN];
	char pass[MAX_USER_PASSWD_LEN];
	int nodeID;
	BackendInfo *backend_info;
	int ch;
	int	optindex;
    bool verbose = false;
	bool debug = false;
	PCPConnInfo* pcpConn;
	PCPResultInfo* pcpResInfo;

	static struct option long_options[] = {
		{"debug", no_argument, NULL, 'd'},
		{"help", no_argument, NULL, 'h'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};
	
    while ((ch = getopt_long(argc, argv, "hdv", long_options, &optindex)) != -1) {
		switch (ch) {
		case 'd':
			debug = true;
			break;

		case 'v':
			verbose = true;
			break;

		case 'h':
		case '?':
		default:
			myexit(NULL);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 5)
		myexit(NULL);

	if (strlen(argv[0]) >= MAX_DB_HOST_NAMELEN)
		myexit(NULL);
	strcpy(host, argv[0]);

	port = atoi(argv[1]);
	if (port <= 1024 || port > 65535)
		myexit(NULL);

	if (strlen(argv[2]) >= MAX_USER_PASSWD_LEN)
		myexit(NULL);
	strcpy(user, argv[2]);

	if (strlen(argv[3]) >= MAX_USER_PASSWD_LEN)
		myexit(NULL);
	strcpy(pass, argv[3]);

	nodeID = atoi(argv[4]);
	if (nodeID < 0 || nodeID > MAX_NUM_BACKENDS)
		myexit(NULL);

	pcpConn = pcp_connect(host, port, user, pass, debug?stdout:NULL);
	if(PCPConnectionStatus(pcpConn) != PCP_CONNECTION_OK)
		myexit(pcpConn);

	pcpResInfo = pcp_node_info(pcpConn,nodeID);
	if(PCPResultStatus(pcpResInfo) != PCP_RES_COMMAND_OK)
		myexit(pcpConn);

	backend_info = (BackendInfo *) pcp_get_binary_data(pcpResInfo,0);

	if (verbose)
	{
		printf("Hostname: %s\nPort    : %d\nStatus  : %d\nWeight  : %f\n", 
			   backend_info->backend_hostname,
			   backend_info->backend_port,
			   backend_info->backend_status,
			   backend_info->backend_weight/RAND_MAX);
	} else {
		printf("%s %d %d %f\n", 
			   backend_info->backend_hostname,
			   backend_info->backend_port,
			   backend_info->backend_status,
			   backend_info->backend_weight/RAND_MAX);
	}

	pcp_disconnect(pcpConn);
	pcp_free_connection(pcpConn);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr, "pcp_node_info - display a pgpool-II node's information\n\n");
	fprintf(stderr, "Usage: pcp_node_info [-d] hostname port# username password nodeID\n");
	fprintf(stderr, "  -d, --debug    : enable debug message (optional)\n");
	fprintf(stderr, "  hostname       : pgpool-II hostname\n");
	fprintf(stderr, "  port#          : PCP port number\n");
	fprintf(stderr, "  username       : username for PCP authentication\n");
	fprintf(stderr, "  password       : password for PCP authentication\n");
	fprintf(stderr, "  nodeID         : ID of a node to get information for\n\n");
	fprintf(stderr, "Usage: pcp_node_info [options]\n");
    fprintf(stderr, "  Options available are:\n");
	fprintf(stderr, "  -h, --help     : print this help\n");
	fprintf(stderr, "  -v, --verbose  : display one line per information with a header\n");
}

static void
myexit(PCPConnInfo* pcpConn)
{
	if (pcpConn == NULL)
	{
		usage();
	}
	else
	{
		fprintf(stderr, "%s\n",pcp_get_last_error(pcpConn)?pcp_get_last_error(pcpConn):"Unknown Error");
		pcp_disconnect(pcpConn);
		pcp_free_connection(pcpConn);
	}
	exit(-1);
}


