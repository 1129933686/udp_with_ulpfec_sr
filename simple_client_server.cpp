#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "modules/rtp_rtcp/include/simple_client_server.h"
#if defined(WEBRTC_WIN)
of_status_t get_next_pkt(SOCKET so, void** pkt, int32_t* len)
{
    int32_t saved_len = *len; // 保存预期的数据包大小

    if ((*pkt = malloc(saved_len)) == NULL)
    {
        OF_PRINT_ERROR(("no memory (malloc failed for p)\n"))
            return OF_STATUS_ERROR;
    }

    // 阻塞式接收数据包
    *len = recvfrom(so, (char*)*pkt, saved_len, 0, NULL, NULL);
    if (*len > 0)
    {
        if (VERBOSITY > 1)
            printf("%s: pkt received, len=%u\n", __FUNCTION__, *len); // 成功接收
        return OF_STATUS_OK;
    }
    else
    {
        // 接收失败，打印错误信息
        perror("recvfrom");
        OF_PRINT_ERROR(("recvfrom failed\n"));
        free(*pkt);	/* don't forget to free it, otherwise it will leak */
        return OF_STATUS_ERROR;
    }
}

SOCKET init_socket()
{
    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (ret != 0) {
        printf("Failed to initialize Winsock\n");
        return INVALID_SOCKET;
    }
    else {
        printf("Winsock initialized successfully\n");
    }
    SOCKET		s;
    SOCKADDR_IN	bindAddr;
    UINT32		sz = 1024 * 1024;

    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET)
    {
        printf("Error: call to socket() failed\n");
        return INVALID_SOCKET;
    }
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons((short)DEST_PORT);
    bindAddr.sin_addr.s_addr = inet_addr(DEST_IP);
    if (bind(s, (SOCKADDR*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR)
    {
        printf("bind() failed. Port %d may be already in use\n", DEST_PORT);
        return INVALID_SOCKET;
    }
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz)) == -1) {
        printf("setsockopt() failed to set new UDP socket size to %u\n", sz);
        return INVALID_SOCKET;
    }
    return s;
}

#elif defined(WEBRTC_POSIX)
// 待验证
of_status_t get_next_pkt(SOCKET so, void** pkt, int32_t* len)
{
    int32_t saved_len = *len; // 保存预期的数据包大小

    if ((*pkt = malloc(saved_len)) == NULL)
    {
        OF_PRINT_ERROR(("no memory (malloc failed for p)\n"));
        return OF_STATUS_ERROR;
    }

    struct sockaddr_in from_addr;
    socklen_t from_addr_len = sizeof(from_addr);

    // 阻塞式接收数据包
    *len = recvfrom(so, *pkt, saved_len, 0, (struct sockaddr*)&from_addr, &from_addr_len);
    if (*len > 0)
    {
        if (VERBOSITY > 1)
            printf("%s: pkt received, len=%u\n", __func__, (unsigned int)*len); // 成功接收
        return OF_STATUS_OK;
    }
    else
    {
        // 接收失败，打印错误信息并释放内存
        perror("recvfrom");
        OF_PRINT_ERROR(("recvfrom failed\n"));
        free(*pkt);  
        *pkt = NULL;
        return OF_STATUS_ERROR;
    }
}

// of_status_t get_next_pkt(SOCKET so, void** pkt, int32_t* len)
// {
//     static bool first_call = true;
//     int32_t saved_len = *len; // 保存预期的数据包大小

//     if ((*pkt = malloc(saved_len)) == NULL)
// 	{
// 		OF_PRINT_ERROR(("no memory (malloc failed for p)\n"))
// 			return OF_STATUS_ERROR;
// 	}

//     if (first_call)
//     {
//         // 第一次调用时设置非阻塞模式，因为后续需要轮询
//         first_call = false;
//         *len = recvfrom(so, (char*)*pkt, saved_len, 0, NULL, NULL);
//         if (*len < 0)
//         {
//             perror("recvfrom");
//             fprintf(stderr, "recvfrom failed\n");
//                 free(*pkt);	/* don't forget to free it, otherwise it will leak */ 
//             return OF_STATUS_ERROR;
//         }

//         // 设置套接字为非阻塞模式
//         int flags = fcntl(so, F_GETFL, 0);
//         if (flags == -1 || fcntl(so, F_SETFL, flags | O_NONBLOCK) == -1)
//         {
//             perror("fcntl");
//             fprintf(stderr, "ERROR, fcntl failed to set non-blocking mode\n");
//             exit(-1);
//         }

//         if (VERBOSITY > 1)
//             printf("%s: pkt received 0, len=%u\n", __FUNCTION__, *len); // 成功接收
//         return OF_STATUS_OK;
//     }

//     // 后续调用在非阻塞模式下接收数据
//     *len = recvfrom(so, (char*)*pkt, saved_len, 0, NULL, NULL);
//     if (*len > 0)
//     {
//         if (VERBOSITY > 1)
//             printf("%s: pkt received 1, len=%u\n", __FUNCTION__, *len); // 成功接收
//         return OF_STATUS_OK;
//     }
//     else if (errno == EAGAIN || errno == EWOULDBLOCK)
//     {
//         // 没有可用数据包，稍作延迟后重试
//         for (int i = 0; i < 50; i++)
//         {
//             usleep(2000); // 延迟 2 毫秒
//             *len = recvfrom(so, (char*)*pkt, saved_len, 0, NULL, NULL);
//             if (*len > 0)
//             {
//                 if (VERBOSITY > 1)
//                     printf("%s: pkt received 2, len=%u\n", __FUNCTION__, *len); // 成功接收
//                 return OF_STATUS_OK;
//             }
//         }

//         // 测试结束，确认没有更多数据包
//         if (VERBOSITY > 1)
//             printf("%s: end of test, no packet after the sleep\n", __FUNCTION__); // 测试结束
//             free(*pkt);	/* don't forget to free it, otherwise it will leak */
//         return OF_STATUS_FAILURE;
//     }
//     else
//     {
//         perror("recvfrom");
//         fprintf(stderr, "ERROR, recvfrom failed\n");
//         free(*pkt);	/* don't forget to free it, otherwise it will leak */
//         return OF_STATUS_ERROR;
//     }

//     return OF_STATUS_ERROR; // 永远不会到达这里
// }

SOCKET init_socket()
{
    int s;
    struct sockaddr_in bindAddr;
    int sz = 1024 * 1024;

    // 创建 UDP 套接字
    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("Error: call to socket() failed");
        return -1;
    }

    // 设置绑定地址
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons((short)DEST_PORT);
    bindAddr.sin_addr.s_addr = inet_addr(DEST_IP);

    // 绑定套接字
    if (bind(s, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0)
    {
        perror("bind() failed. Port may be already in use");
        close(s);
        return -1;
    }

    // 设置接收缓冲区大小
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) < 0)
    {
        perror("setsockopt() failed to set new UDP socket size");
        close(s);
        return -1;
    }

    printf("Socket initialized successfully\n");
    return s;
}
#endif
