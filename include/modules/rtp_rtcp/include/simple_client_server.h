/* $Id: simple_client_server.h 207 2014-12-10 19:47:50Z roca $ */
/*
 * OpenFEC.org AL-FEC Library.
 * (c) Copyright 2009-2014 INRIA - All rights reserved
 * Contact: vincent.roca@inria.fr
 *
 * This software is governed by the CeCILL-C license under French law and
 * abiding by the rules of distribution of free software.  You can  use,
 * modify and/ or redistribute the software under the terms of the CeCILL-C
 * license as circulated by CEA, CNRS and INRIA at the following URL
 * "http://www.cecill.info".
 *
 * As a counterpart to the access to the source code and  rights to copy,
 * modify and redistribute granted by the license, users are provided only
 * with a limited warranty  and the software's author,  the holder of the
 * economic rights,  and the successive licensors  have only  limited
 * liability.
 *
 * In this respect, the user's attention is drawn to the risks associated
 * with loading,  using,  modifying and/or developing or reproducing the
 * software by the user in light of its specific status of free software,
 * that may mean  that it is complicated to manipulate,  and  that  also
 * therefore means  that it is reserved for developers  and  experienced
 * professionals having in-depth computer knowledge. Users are therefore
 * encouraged to load and test the software's suitability as regards their
 * requirements in conditions enabling the security of their systems and/or
 * data to be ensured and,  more generally, to use and operate it in the
 * same conditions as regards security.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL-C license and that you accept its terms.
 */
#ifndef SIMPLE_CLIENT_SERVER_H_
#define SIMPLE_CLIENT_SERVER_H_

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h> 
#include <ctype.h>
#include <sys/types.h>
#include <time.h>
#if defined(WEBRTC_WIN)
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#include <ws2tcpip.h>
#elif defined(WEBRTC_POSIX)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <stdint.h>
#include "modules/rtp_rtcp/include/of_debug.h"

 /*
  * OS dependant definitions
  */
#ifndef SOCKET
#define SOCKET		int
#endif

#ifndef SOCKADDR
#define SOCKADDR	struct sockaddr
#endif

#ifndef SOCKADDR_IN
#define SOCKADDR_IN	struct sockaddr_in
#endif

#ifndef INVALID_SOCKET
#define INVALID_SOCKET	(-1)
#endif

#ifndef SOCKET_ERROR
#define SOCKET_ERROR	(-1)
#endif

  //#define closesocket	close
#define SLEEP(t)	usleep(t*1000)


/*
 * Simulation parameters...
 * Change as required
 */
#define SYMBOL_SIZE	1024	/* symbol size, in bytes (must be multiple of 4 in this simple example) */
#define	DEFAULT_K	10		/* default k value */
#define VERBOSITY	0		/* Define the verbosity level:
                     *	0 : no trace
                     *	1 : main traces
                     *	2 : full traces with packet dumps */

#define DEST_IP		"127.0.0.1"	/* Destination IPv4 address */
#define DEST_PORT   10978     /* Destination port (UDP) */

#pragma pack(1) // �ṹ�������֮����ܴ洢�������ж�����䣺�ṹ����������ԱӦ�ð���1�ֽڶ��뷽ʽ��������
struct videoStruct
{
    unsigned int  sysWord;
    unsigned char  idWord;
    unsigned int  nowTime;
    unsigned char  versionNumber;
    unsigned char  totalChannel; //2/3
    unsigned char  whichChannel; //1 2 3
    unsigned char  videoFormat;  //1
    unsigned short packetSize;    //5623
    unsigned char  videoData[10000] = { 0 };

};
#pragma pack() // ���� #pragma pack(1) ������

typedef enum {
    OF_STATUS_OK = 0,
    OF_STATUS_FAILURE,
    OF_STATUS_ERROR,
    OF_STATUS_FATAL_ERROR
} of_status_t;

SOCKET init_socket( );

/**
 * �ú����ڴ����UDP�׽����Ͻ������ݰ���
 * ������һ����СΪ *len �Ļ���������ʹ��ʵ�ʽ��յ������ݸ��� pkt/len ������
 * ��һ�ε���ʱ������ģʽ��������Ϊ�ͻ��˿����ڷ�����֮ǰ���������ӣ���
 * ֮���Է�����������ѯ��ģʽ�����������ʹ�ڵȴ�һ��ʱ�䣨0.2�룩����δ�յ����ݰ���
 * �򷵻� OF_STATUS_FAILURE����ʾ���ͷ�������ֹͣ���д��䡣
 */
of_status_t get_next_pkt(SOCKET so, void** pkt, int32_t* len);

#endif //SIMPLE_CLIENT_SERVER_H_

