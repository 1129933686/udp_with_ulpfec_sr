#define _WINSOCK_DEPRECATED_NO_WARNINGS
#if defined(WEBRTC_WIN)
#define MY_FEC_API __attribute__((dllexport))
#elif defined(WEBRTC_POSIX)
#define MY_FEC_API __attribute__((visibility("default")))
#endif
#include <fstream>
#include "modules/rtp_rtcp/include/simple_client_server.h"
#include "modules/rtp_rtcp/source/forward_error_correction.h" 
#include "modules/rtp_rtcp/source/forward_error_correction_internal.h"
#include "modules/rtp_rtcp/source/fec_private_tables_bursty.h"
#include "modules/rtp_rtcp/source/fec_private_tables_random.h"
#include "udp_with_ulpfec.h"

void sendto_fec(SOCKET so, const char* buf, int len, int flags, const sockaddr* to, int tolen, int k, int r, int bitrate, double packet_loss_rate) {
	static ForwardErrorCorrection fec_instance;
	static bool is_initialized = false; // 静态变量，用于跟踪是否已初始化

	// 仅在第一次调用时初始化
	if (!is_initialized) {
		fec_instance.MediaPacketsInit(k); 
		fec_instance.FecPacketsInit(r);
        fec_instance.NumberClear(so, flags, to, tolen); 
		is_initialized = true; // 标记为已初始化
	}

	fec_instance.SendByUlpfec(so, buf, len, flags, to, tolen, k, r, bitrate, packet_loss_rate);
}

int recvfrom_fec(SOCKET so, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
	int ret;
	static ForwardErrorCorrection fec_decoder;
	ret = fec_decoder.RecvByUlpfec(so, buf, len, flags, from, fromlen);
	return ret;
}


