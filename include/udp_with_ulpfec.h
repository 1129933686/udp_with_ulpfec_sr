#pragma once
#ifdef MY_FEC_API	
#else
#if defined(WEBRTC_WIN)
#define MY_FEC_API __attribute__((dllimport))
#elif defined(WEBRTC_POSIX)
#define MY_FEC_API __attribute__((visibility("default")))
#endif
#endif // MY_CAL_API

// #include <winsock2.h>
#include "modules/rtp_rtcp/include/simple_client_server.h"

MY_FEC_API	void sendto_fec(SOCKET so, const char* buf, int len, int flags, const sockaddr* to, int tolen, int k, int r, int bitrate, double packet_loss_rate);

MY_FEC_API  int  recvfrom_fec(SOCKET so, char* buf, int len, int flags, sockaddr* from, int* fromlen);