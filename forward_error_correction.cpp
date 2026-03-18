#include <thread>
#include <fstream>
#if defined(WEBRTC_WIN)	
#include <ws2tcpip.h> 
#elif defined(WEBRTC_POSIX)
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "modules/rtp_rtcp/include/simple_client_server.h"
#include "modules/rtp_rtcp/source/forward_error_correction.h" 
#include "modules/rtp_rtcp/source/fec_private_tables_bursty.h"
#include "modules/rtp_rtcp/source/fec_private_tables_random.h"
#include "absl/algorithm/container.h"
#include "rtc_base/numerics/mod_ops.h"

#define print_message 0 // 是否打印发送和接收的包信息

ForwardErrorCorrection::~ForwardErrorCorrection() = default;

ForwardErrorCorrection::Packet::Packet() : ref_count_(0), packet_mask(0), group_number(0), sequence_number(0), k(0), data_length(0) {
	memset(data, 0, kMaxDataSize);
}

ForwardErrorCorrection::Packet::~Packet() = default;

int32_t ForwardErrorCorrection::Packet::AddRef() {
	return ++ref_count_;
}

int32_t ForwardErrorCorrection::Packet::Release() {
	int32_t ref_count;
	ref_count = --ref_count_;
	if (ref_count == 0)
		delete this;
	return ref_count;
}

ForwardErrorCorrection::ReceivedPacket::ReceivedPacket() = default; 

ForwardErrorCorrection::ReceivedPacket::~ReceivedPacket() = default;

ForwardErrorCorrection::ProtectedPacket::ProtectedPacket() = default;

ForwardErrorCorrection::ProtectedPacket::~ProtectedPacket() = default;

ForwardErrorCorrection::ReceivedFecPacket::ReceivedFecPacket() = default;

ForwardErrorCorrection::ReceivedFecPacket::~ReceivedFecPacket() = default;

ForwardErrorCorrection::RecoveredPacket::RecoveredPacket() = default;

ForwardErrorCorrection::RecoveredPacket::~RecoveredPacket() = default;

PacketMaskTable::PacketMaskTable(FecMaskType fec_mask_type,
	int num_media_packets)
	: table_(PickTable(fec_mask_type, num_media_packets)) {
}

PacketMaskTable::~PacketMaskTable() = default;

const uint8_t* PacketMaskTable::PickTable(FecMaskType fec_mask_type,
	int num_media_packets) {
	RTC_DCHECK_GE(num_media_packets, 0);
	RTC_DCHECK_LE(static_cast<size_t>(num_media_packets), kUlpfecMaxMediaPackets);

	if (fec_mask_type != kFecMaskRandom &&
		num_media_packets <=
		static_cast<int>(kPacketMaskBurstyTbl[0])) {
		return &kPacketMaskBurstyTbl[0];
	}

	return &kPacketMaskRandomTbl[0];
}

// 初始化k个数据包
void ForwardErrorCorrection::MediaPacketsInit(int k) {
	media_packets.resize(k);
	for (int i = 0; i < k; i++) {
		media_packets[i] = std::make_unique<Packet>();
	}
}

// 初始化r个冗余包
void ForwardErrorCorrection::FecPacketsInit(int r) {
	fec_packets.resize(r);
	for (int i = 0; i < r; i++) {
		fec_packets[i] = std::make_unique<Packet>();
	}
}

int ForwardErrorCorrection::EncodeFec(const std::vector<std::unique_ptr<Packet>>& media_packets,
	int32_t r,
	int num_important_packets,
	bool use_unequal_protection,
	FecMaskType fec_mask_type,
	std::vector<std::unique_ptr<Packet>>& fec_packets) {
	const size_t num_media_packets = media_packets.size();

	// 检查参数有效性
	RTC_DCHECK_GT(num_media_packets, 0);
	RTC_DCHECK_GE(num_important_packets, 0);
	RTC_DCHECK_LE(num_important_packets, num_media_packets);
	RTC_DCHECK_LE(num_media_packets, 16);

	// 准备生成的FEC包
	int num_fec_packets = r;
	RTC_DCHECK_LE(num_fec_packets, num_media_packets);
	if (num_fec_packets == 0) {
		return 0;
	}

	// 创建包掩码表
	int num_media_packets_int = static_cast<int>(num_media_packets);
	PacketMaskTable mask_table(fec_mask_type, num_media_packets_int);
	packet_mask_size_ = PacketMaskSize(num_media_packets);
	memset(packet_masks_, 0, num_fec_packets * packet_mask_size_);
	GeneratePacketMasks(num_media_packets, num_fec_packets,
		num_important_packets, use_unequal_protection,
		&mask_table, packet_masks_);

	// 生成FEC包
	for (int i = 0; i < num_fec_packets; ++i) {
		// 获取当前FEC包
		auto& fec_packet = fec_packets[i];

		// 遍历media_packets，生成FEC数据
		for (size_t j = 0; j < media_packets.size(); ++j) {
			if (packet_masks_[i * packet_mask_size_ + j / 8] & (1 << (7 - (j % 8)))) {
				XorPayloads(media_packets[j]->data_length, media_packets[j]->data, &(fec_packet->data_length), fec_packet->data, packet_size);
			}
		}

		// 填充FEC包头信息
		memcpy(&fec_packet->packet_mask, &packet_masks_[i * packet_mask_size_], packet_mask_size_);
		fec_packet->group_number = group_number;
		fec_packet->sequence_number = sequence_number;
		fec_packet->k = static_cast<uint8_t>(num_media_packets);

		// 更新sequence_number
		sequence_number++;
	}

	return num_fec_packets;
}

// 异或运算生成FEC数据
void ForwardErrorCorrection::XorPayloads(uint16_t src_data_length,
	const uint8_t* src,
	uint16_t* dst_data_length,
	uint8_t* dst,
	size_t length) {
	// 构建源头部缓冲区
	uint8_t src_header[2];
	src_header[0] = static_cast<uint8_t>(src_data_length >> 8);
	src_header[1] = static_cast<uint8_t>(src_data_length & 0xFF);

	// 构建目标头部缓冲区
	uint8_t dst_header[2];
	dst_header[0] = static_cast<uint8_t>((*dst_data_length) >> 8);
	dst_header[1] = static_cast<uint8_t>((*dst_data_length) & 0xFF);

	// 头部字段XOR
	dst_header[0] ^= src_header[0];
	dst_header[1] ^= src_header[1];

	// 更新目标数据长度
	*dst_data_length = (static_cast<uint16_t>(dst_header[0]) << 8) | static_cast<uint16_t>(dst_header[1]);

	// 数据部分进行异或运算
	size_t i = 0;

	// 使用AVX2指令集进行256位（32字节）XOR操作
	for (; i + 31 < length; i += 32) {
		__m256i src_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
		__m256i dst_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
		__m256i result_vec = _mm256_xor_si256(src_vec, dst_vec);
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), result_vec);
	}

	// 使用SSE2指令集进行128位（16字节）XOR操作
	for (; i + 15 < length; i += 16) {
		__m128i src_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
		__m128i dst_vec = _mm_loadu_si128(reinterpret_cast<__m128i*>(dst + i));
		__m128i result_vec = _mm_xor_si128(src_vec, dst_vec);
		_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), result_vec);
	}

	// 处理剩余字节（不足16字节）
	for (; i < length; ++i) {
		dst[i] ^= src[i];
	}
}

size_t PacketMaskSize(size_t num_sequence_numbers) {
	RTC_DCHECK_LE(num_sequence_numbers, 8 * kUlpfecPacketMaskSizeLBitSet);
	if (num_sequence_numbers > 8 * kUlpfecPacketMaskSizeLBitClear) {
		return kUlpfecPacketMaskSizeLBitSet;
	}
	return kUlpfecPacketMaskSizeLBitClear;
}


void GeneratePacketMasks(int num_media_packets,
	int num_fec_packets,
	int num_imp_packets,
	bool use_unequal_protection,
	PacketMaskTable* mask_table,
	uint8_t* packet_mask) {
	RTC_DCHECK_GT(num_media_packets, 0);
	RTC_DCHECK_GT(num_fec_packets, 0);
	RTC_DCHECK_LE(num_fec_packets, num_media_packets);
	RTC_DCHECK_LE(num_imp_packets, num_media_packets);
	RTC_DCHECK_GE(num_imp_packets, 0);

	const int num_mask_bytes = PacketMaskSize(num_media_packets); 

	if (!use_unequal_protection || num_imp_packets == 0) {
		rtc::ArrayView<const uint8_t> mask =
			mask_table->LookUp(num_media_packets, num_fec_packets);
		memcpy(packet_mask, &mask[0], mask.size());
	}
}  

rtc::ArrayView<const uint8_t> PacketMaskTable::LookUp(int num_media_packets,
	int num_fec_packets) {
	RTC_DCHECK_GT(num_media_packets, 0);
	RTC_DCHECK_GT(num_fec_packets, 0);
	RTC_DCHECK_LE(num_media_packets, kUlpfecMaxMediaPackets);
	RTC_DCHECK_LE(num_fec_packets, num_media_packets);

	if (num_media_packets <= 12) {
		return LookUpInFecTable(table_, num_media_packets - 1, num_fec_packets - 1);
	}

	int mask_length =
		static_cast<int>(PacketMaskSize(static_cast<size_t>(num_media_packets)));

	for (int row = 0; row < num_fec_packets; row++) {
		for (int col = 0; col < mask_length; col++) {
			fec_packet_mask_[row * mask_length + col] =
				((col * 8) % num_fec_packets == row && (col * 8) < num_media_packets
					? 0x80
					: 0x00) |
				((col * 8 + 1) % num_fec_packets == row &&
					(col * 8 + 1) < num_media_packets
					? 0x40
					: 0x00) |
				((col * 8 + 2) % num_fec_packets == row &&
					(col * 8 + 2) < num_media_packets
					? 0x20
					: 0x00) |
				((col * 8 + 3) % num_fec_packets == row &&
					(col * 8 + 3) < num_media_packets
					? 0x10
					: 0x00) |
				((col * 8 + 4) % num_fec_packets == row &&
					(col * 8 + 4) < num_media_packets
					? 0x08
					: 0x00) |
				((col * 8 + 5) % num_fec_packets == row &&
					(col * 8 + 5) < num_media_packets
					? 0x04
					: 0x00) |
				((col * 8 + 6) % num_fec_packets == row &&
					(col * 8 + 6) < num_media_packets
					? 0x02
					: 0x00) |
				((col * 8 + 7) % num_fec_packets == row &&
					(col * 8 + 7) < num_media_packets
					? 0x01
					: 0x00);
		}
	}
	return { &fec_packet_mask_[0],
			static_cast<size_t>(num_fec_packets * mask_length) };
}

rtc::ArrayView<const uint8_t> LookUpInFecTable(const uint8_t* table,
	int media_packet_index,
	int fec_index) {
	RTC_DCHECK_LT(media_packet_index, table[0]);

	// Skip over the table size.
	const uint8_t* entry = &table[1];

	uint8_t entry_size_increment = 2;  // 0-16 are 2 byte wide, then changes to 6.

	// Hop over un-interesting array entries.
	for (int i = 0; i < media_packet_index; ++i) {
		if (i == 16)
			entry_size_increment = 6;
		uint8_t count = entry[0];
		++entry;  // skip over the count.
		for (int j = 0; j < count; ++j) {
			entry += entry_size_increment * (j + 1);  // skip over the data.
		}
	}

	if (media_packet_index == 16)
		entry_size_increment = 6;

	RTC_DCHECK_LT(fec_index, entry[0]);
	++entry;  // Skip over the size.

	// Find the appropriate data in the second dimension.

	// Find the specific data we're looking for.
	for (int i = 0; i < fec_index; ++i)
		entry += entry_size_increment * (i + 1);  // skip over the data.

	size_t size = entry_size_increment * (fec_index + 1);
	return { &entry[0], size };
}

// bitrate等于0时表示不限制发送速率
void ForwardErrorCorrection::SendByUlpfec(SOCKET so, const char* buf, int len, int flags, const sockaddr* to, int tolen, int k, int r, int bitrate, double packet_loss_rate) {
	int ret = -1;
	// 邢启航09_06添加内容：随机丢包模拟
	static auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	static std::mt19937 gen(seed);  // 用时间种子初始化
	static std::uniform_real_distribution<> dis(0.0, 1.0);

	// 更新packet_size（只在数据长度变大时更新）
	if (len > packet_size) {
		packet_size = len;
	}

	// 存储当前数据包
	auto& packet = media_packets[current_packet_index_];
	packet->packet_mask = 0; // 数据包的包掩码为0
	packet->group_number = group_number;
	packet->sequence_number = sequence_number;
	packet->k = k;
	packet->data_length = htons(static_cast<uint16_t>(len));
	memcpy(packet->data, buf, len);
	// 更新sequence_number
	sequence_number++;

	// 更新当前 Packet 索引
	current_packet_index_ = (current_packet_index_ + 1) % k;

	// 发送数据包
	// 情况1：bitrate == 0 时，不限制发送速率，直接发送
	if (bitrate == 0) {
		double rand_val = dis(gen);
		if (rand_val >= packet_loss_rate) {
			if ((ret = sendto(so, reinterpret_cast<const char*>(&packet->packet_mask), len + fec_head_size, 0, to, tolen)) == SOCKET_ERROR) {
				OF_PRINT_ERROR(("sendto() failed!\n"))
					ret = -1;
				return;
			}
#if print_message
			printf("sending SRC symbol: group_number=%u, sequence_number=%u, data_length=%u\n", packet->group_number, packet->sequence_number, ntohs(packet->data_length));
#endif
		}
		else {
#if print_message
			printf("SRC symbol dropped (simulated loss): group_number=%u, sequence_number=%u, data_length=%u\n", packet->group_number, packet->sequence_number, ntohs(packet->data_length));
#endif
		}
	}
	else {
		// 情况2：bitrate > 0 时，计算发送时间间隔
		auto start = std::chrono::high_resolution_clock::now(); //记录当前时间
		double rand_val = dis(gen);
		if (rand_val >= packet_loss_rate) {
			if ((ret = sendto(so, reinterpret_cast<const char*>(&packet->packet_mask), len + fec_head_size, 0, to, tolen)) == SOCKET_ERROR) {
				OF_PRINT_ERROR(("sendto() failed!\n"))
					ret = -1;
				return;
			}
#if print_message
			printf("sending SRC symbol: group_number=%u, sequence_number=%u, data_length=%u\n", packet->group_number, packet->sequence_number, ntohs(packet->data_length));
#endif
		}
		else {
#if print_message
			printf("SRC symbol dropped (simulated loss): group_number=%u, sequence_number=%u, data_length=%u\n", packet->group_number, packet->sequence_number, ntohs(packet->data_length));
#endif
		}

		auto end = std::chrono::high_resolution_clock::now(); // 记录当前时间
		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		double timeTaken = duration.count() / 1000.0; // 实际时间
		double desiredTime = (packet_size + 7) * 8 / static_cast<double>(bitrate); // 理想时间

		double sleepTime = desiredTime - timeTaken;
		if (sleepTime > 0) {
			std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleepTime));
		}
	}

	// 当数据包列表中有k个数据包时，执行一次编码函数
	if (current_packet_index_ == 0) {
		EncodeFec(media_packets, r, kNumImportantPackets, kUseUnequalProtection, fec_mask_type, fec_packets);

		// 发送冗余包
		for (const auto& fec_packet : fec_packets) {
			// 情况1：bitrate == 0 时，不限制发送速率，直接发送
			if (bitrate == 0) {
				double rand_val_fec = dis(gen);
				if (rand_val_fec >= packet_loss_rate) {
					if ((ret = sendto(so, reinterpret_cast<const char*>(&fec_packet->packet_mask), packet_size + fec_head_size, 0, to, tolen)) == SOCKET_ERROR) {
						OF_PRINT_ERROR(("sendto() failed!\n"))
							ret = -1;
						return;
					}
#if print_message
					printf("sending FEC symbol: group_number=%u, sequence_number=%u, data_length=%u\n", fec_packet->group_number, fec_packet->sequence_number, packet_size);
#endif
				}
				else {
#if print_message
					// 模拟丢包，直接跳过发送
					printf("FEC symbol dropped (simulated loss): group_number=%u, sequence_number=%u, data_length=%u\n", fec_packet->group_number, fec_packet->sequence_number, packet_size);
#endif
				}
			}
			else {
				auto start = std::chrono::high_resolution_clock::now(); //记录当前时间
				double rand_val_fec = dis(gen);
				if (rand_val_fec >= packet_loss_rate) {
					if ((ret = sendto(so, reinterpret_cast<const char*>(&fec_packet->packet_mask), packet_size + fec_head_size, 0, to, tolen)) == SOCKET_ERROR) {
						OF_PRINT_ERROR(("sendto() failed!\n"))
							ret = -1;
						return;
					}
#if print_message
					printf("sending FEC symbol: group_number=%u, sequence_number=%u, data_length=%u\n", fec_packet->group_number, fec_packet->sequence_number, packet_size);
#endif
				}
				else {
					// 模拟丢包，直接跳过发送
#if print_message
					printf("FEC symbol dropped (simulated loss): group_number=%u, sequence_number=%u, data_length=%u\n", fec_packet->group_number, fec_packet->sequence_number, packet_size);
#endif
				}
				auto end = std::chrono::high_resolution_clock::now(); // 记录当前时间
				auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
				double timeTaken = duration.count() / 1000.0; // 实际时间
				double desiredTime = (packet_size + 7) * 8 / static_cast<double>(bitrate); // 理想时间

				double sleepTime = desiredTime - timeTaken;
				if (sleepTime > 0) {
					std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleepTime));
				}
			}
		}
		// 更新group_number和sequence_number
		group_number++;
		sequence_number = 0;

		// 邢启航09_07添加内容：冗余包列表清零
		for (auto& fec_packet : fec_packets) {
			if (fec_packet) {
				fec_packet->packet_mask = 0;
				fec_packet->group_number = 0;
				fec_packet->sequence_number = 0;
				fec_packet->k = 0;
				fec_packet->data_length = 0;
				memset(fec_packet->data, 0, ForwardErrorCorrection::Packet::kMaxDataSize);
				fec_packet->ref_count_ = 0;
			}
		}
	}
}

ForwardErrorCorrection::DecodeFecResult ForwardErrorCorrection::DecodeFec(
	const ReceivedPacket& received_packet,
	RecoveredPacketList* recovered_packets) {
	// 确保传入的 recovered_packets 指针是有效的
	RTC_DCHECK(recovered_packets);
	// 如果已恢复的包数量达到了最大值，检查是否需要重置 FEC 解码状态
	if (recovered_packets->size() == max_media_packets) {
		// 获取最后一个已恢复的包
		const RecoveredPacket* back_recovered_packet = recovered_packets->back().get();
		// 计算接收到的包和最后一个恢复的包之间的组号差异
		const unsigned int seq_num_diff = MinDiff(received_packet.pkt->group_number, back_recovered_packet->pkt->group_number);
		// 如果组号差异大于2，说明组号之间有很大的间隔
		if (seq_num_diff > 2) {
			// 在日志中记录这一信息，并重置 FEC 解码状态
			printf("Big gap in media/ULPFEC group numbers. No need to keep the old packets in the FEC buffers, thus resetting them.");
			ResetState(recovered_packets);
		}
	}
	// 插入数据，media包/FEC包
	InsertPacket(received_packet, recovered_packets);

	DecodeFecResult decode_result;
	// 尝试恢复包
	decode_result.num_recovered_packets = AttemptRecovery(recovered_packets);
	return decode_result;
}

void ForwardErrorCorrection::InsertPacket(
	const ReceivedPacket& received_packet,
	RecoveredPacketList* recovered_packets) {
    // **第一部分：丢弃旧的FEC包**
    // 如果`received_fec_packets_`（已接收的FEC包列表）不为空
    // 则检查是否需要丢弃旧的FEC包。
	if (!received_fec_packets_.empty()) {
        // 遍历`received_fec_packets_`列表，逐个检查FEC包的序列号是否过旧。
		auto it = received_fec_packets_.begin();
		while (it != received_fec_packets_.end()) {
			// 计算当前接收到的包与FEC包之间的组号差异
			uint16_t group_num_diff = MinDiff(received_packet.pkt->group_number, (*it)->pkt->group_number);
            // 如果组号差异大于2，说明该FEC包过旧，
            // 不再可能用于恢复任何丢失的媒体包，因此将其从列表中移除。
			if (group_num_diff > 2) {
				it = received_fec_packets_.erase(it); // 从列表中删除旧的FEC包
			}
			else {
                // 由于`received_fec_packets_`列表是按顺序排序的，
                // 如果当前包的序列号差异不超过阈值，则后续包也一定满足条件。
                // 因此可以直接退出循环，避免不必要的迭代。
				break;
			}
		}
	}

    // **第二部分：根据包的类型（FEC包或媒体包）进行不同处理**
	if (received_packet.pkt->sequence_number > (received_packet.pkt->k - 1)) {
        // 处理接收到的FEC包，通过解析包掩码来确定哪些媒体数据包被保护，并尝试使用FEC包来恢复任何丢失的媒体数据包。
        // 同时，它负责维护一个有序的FEC包列表，确保处理效率和资源使用的平衡。
		InsertFecPacket(*recovered_packets, received_packet);
	}
	else {
        // 处理接收到的媒体数据包，并将其插入到恢复包列表中
		InsertMediaPacket(recovered_packets, received_packet);
	}

    // **第三部分：丢弃旧的恢复包**
    // 调用`DiscardOldRecoveredPackets`函数，清理恢复包列表中过旧的包。
    // 这可以防止恢复包列表无限增长，节省内存，同时确保只保留有用的包。
	DiscardOldRecoveredPackets(recovered_packets);
}

void ForwardErrorCorrection::InsertFecPacket(
	const RecoveredPacketList& recovered_packets,
	const ReceivedPacket& received_packet) {

    // 检查重复的FEC包。
	for (const auto& existing_fec_packet : received_fec_packets_) {
		if (existing_fec_packet->pkt->sequence_number == received_packet.pkt->sequence_number &&
			existing_fec_packet->pkt->group_number == received_packet.pkt->group_number) {
			return; // 丢弃重复的FEC包数据。
		}
	}
    // 创建一个新的ReceivedFecPacket对象来存储接收到的FEC包信息。
	std::unique_ptr<ReceivedFecPacket> fec_packet(new ReceivedFecPacket());
	fec_packet->group_number = received_packet.group_number;
	fec_packet->sequence_number = received_packet.sequence_number;
	fec_packet->pkt = received_packet.pkt;

    // 定义一个 uint16_t 类型的 packet_mask 变量，并将 fec_packet->pkt->packet_mask 赋给该变量
	uint16_t packet_mask = fec_packet->pkt->packet_mask;
    // 遍历 packet_mask 的每个位，检查哪些媒体包被保护
	for (uint16_t bit_idx = 0; bit_idx < 16; ++bit_idx) {
		if (packet_mask & (1 << (15 - bit_idx))) {
			std::unique_ptr<ProtectedPacket> protected_packet(new ProtectedPacket());
            // 计算受保护的包的组号和序列号
			protected_packet->group_number = fec_packet->group_number;
			protected_packet->sequence_number = bit_idx;
            // 初始化时不关联具体数据包
			protected_packet->pkt = nullptr;
			fec_packet->protected_packets.push_back(std::move(protected_packet));
		}
	}

    // 如果FEC包的包掩码全为零，说明这个FEC包无法保护任何媒体包，可以直接丢弃。
	if (fec_packet->protected_packets.empty()) {
		printf("Received FEC packet has an all-zero packet mask.");
	}
	else {
        // 将FEC包中受保护的媒体包列表（protected_packets）与已经恢复的媒体包列表（recovered_packets）进行比较，
        // 找到两者的交集（即FEC包保护的媒体包中哪些已经被恢复）。
        // 对于这些已经恢复的媒体包，函数会更新FEC包中对应受保护包的指针，使其指向实际的媒体包数据。
		AssignRecoveredPackets(recovered_packets, fec_packet.get());
        // 将新的FEC包添加到已接收的FEC包列表中，并按序列号排序。
		received_fec_packets_.push_back(std::move(fec_packet));
		received_fec_packets_.sort(SortablePacket::LessThan());

        // 如果列表过大，则移除最旧的FEC包。
		if (received_fec_packets_.size() > max_fec_packets) {
			received_fec_packets_.pop_front();
		}
		RTC_DCHECK_LE(received_fec_packets_.size(), max_fec_packets);
	}
}

void ForwardErrorCorrection::InsertMediaPacket(
	RecoveredPacketList* recovered_packets,
	const ReceivedPacket& received_packet) {
	// 在已恢复的包列表中搜索重复的包。
	for (const auto& recovered_packet : *recovered_packets) {
		if (recovered_packet->pkt->sequence_number == received_packet.pkt->sequence_number &&
			recovered_packet->pkt->group_number == received_packet.pkt->group_number) {
			return; // 重复的包，不需要添加到列表中。
		}
	}

	// 创建一个新的RecoveredPacket对象来存储接收到的媒体包信息。
	std::unique_ptr<RecoveredPacket> recovered_packet(new RecoveredPacket());
	recovered_packet->group_number = received_packet.group_number;
	recovered_packet->sequence_number = received_packet.sequence_number;
	recovered_packet->pkt = received_packet.pkt;

	// TODO: 考虑使用二分搜索找到正确的位置插入新包，以避免排序。
	RecoveredPacket* recovered_packet_ptr = recovered_packet.get();

	// 将新恢复的包添加到已恢复的包列表中。
	recovered_packets->push_back(std::move(recovered_packet));

	// 对已恢复的包列表进行排序，以确保它们按序列号顺序排列。
	recovered_packets->sort(SortablePacket::LessThan());

	// 更新覆盖FEC包的信息，根据新加入的媒体包调整。
	UpdateCoveringFecPackets(*recovered_packet_ptr);
}

//解码恢复丢失的包，每次恢复一个，恢复一个即删除掉该FEC，再从头开始，直至所有包都恢复，和我们学过的高斯消元法比较类似
size_t ForwardErrorCorrection::AttemptRecovery(
	RecoveredPacketList* recovered_packets) {
	size_t num_recovered_packets = 0; // 记录成功恢复的包数量
	// 创建一个迭代器，用于遍历接收到的 FEC 包
	auto fec_packet_it = received_fec_packets_.begin();
	// 遍历所有接收到的 FEC 包
	while (fec_packet_it != received_fec_packets_.end()) {
		// 通过当前 FEC 包计算缺失的媒体包数量
		int packets_missing = NumCoveredPacketsMissing(**fec_packet_it);

		// 一次只能恢复一个媒体包，如果缺失的包超过两个，先判断其他的 FEC 包
		if (packets_missing == 1) {
			// 可以恢复一个包
			std::unique_ptr<RecoveredPacket> recovered_packet(new RecoveredPacket());
			recovered_packet->pkt = nullptr; // 初始化恢复包的指针
			// 尝试恢复包，恢复过程是对 FEC 编码的逆运算
			if (!RecoverPacket(**fec_packet_it, recovered_packet.get())) {
				// 如果无法恢复该包，则丢弃当前 FEC 包
				fec_packet_it = received_fec_packets_.erase(fec_packet_it);
				continue; // 继续下一个 FEC 包的处理
			}

			++num_recovered_packets; // 成功恢复一个包，计数加一

			auto* recovered_packet_ptr = recovered_packet.get();
            // 将恢复的包添加到恢复包列表中，并更新覆盖该包的 FEC 包指针
            // TODO: 考虑使用二分搜索来找到插入位置，以提高效率
			recovered_packets->push_back(std::move(recovered_packet));
			recovered_packets->sort(SortablePacket::LessThan()); // 对恢复包进行排序
			// 更新所有覆盖该媒体包的 FEC 包的状态
			UpdateCoveringFecPackets(*recovered_packet_ptr);
			// 丢弃旧的已恢复包，保持列表的整洁
			DiscardOldRecoveredPackets(recovered_packets);
			// 删除当前处理的 FEC 包，因为它已经被处理
			fec_packet_it = received_fec_packets_.erase(fec_packet_it);

            // 一旦恢复了一个包，检查是否可以恢复其他包
            // 由于丢失的包数量现在可能减少，因此重新开始处理 FEC 包
			fec_packet_it = received_fec_packets_.begin();
		}
		else if (packets_missing == 0) {
			// 如果当前 FEC 包下所有媒体包都已收到或恢复，则删除该 FEC 包
			fec_packet_it = received_fec_packets_.erase(fec_packet_it);
		}
		else {
			// 如果丢失的包超过一个，则暂时无法恢复，继续处理下一个 FEC 包
			fec_packet_it++;
		}
	}

	return num_recovered_packets; // 返回成功恢复的包数量
}

int ForwardErrorCorrection::NumCoveredPacketsMissing(
	const ReceivedFecPacket& fec_packet) {
	int packets_missing = 0;
	for (const auto& protected_packet : fec_packet.protected_packets) {
		if (protected_packet->pkt == nullptr) {
			++packets_missing;
			if (packets_missing > 1) {
				break;  // We can't recover more than one packet.
			}
		}
	}
	return packets_missing;
}

void ForwardErrorCorrection::AssignRecoveredPackets(
	const RecoveredPacketList& recovered_packets,
	ReceivedFecPacket* fec_packet) {
	ProtectedPacketList* protected_packets = &fec_packet->protected_packets;
	std::vector<RecoveredPacket*> recovered_protected_packets;

	// Find intersection between the (sorted) containers `protected_packets`
	// and `recovered_packets`, i.e. all protected packets that have already
	// been recovered. Update the corresponding protected packets to point to
	// the recovered packets.
	auto it_p = protected_packets->cbegin();
	auto it_r = recovered_packets.cbegin();
	SortablePacket::LessThan less_than;
	while (it_p != protected_packets->end() && it_r != recovered_packets.end()) {
		if (less_than(*it_p, *it_r)) {
			++it_p;
		}
		else if (less_than(*it_r, *it_p)) {
			++it_r;
		}
		else {  // *it_p == *it_r.
			// This protected packet has already been recovered.
			(*it_p)->pkt = (*it_r)->pkt;
			++it_p;
			++it_r;
		}
	}
}

void ForwardErrorCorrection::UpdateCoveringFecPackets(
	const RecoveredPacket& packet) {
	for (auto& fec_packet : received_fec_packets_) {
		// Is this FEC packet protecting the media packet `packet`?
		auto protected_it = absl::c_lower_bound(
			fec_packet->protected_packets, &packet, SortablePacket::LessThan());
		if (protected_it != fec_packet->protected_packets.end() &&
			(*protected_it)->group_number == packet.pkt->group_number &&
			(*protected_it)->sequence_number == packet.pkt->sequence_number) {
			// Found an FEC packet which is protecting `packet`.
			(*protected_it)->pkt = packet.pkt;
		}
	}
}

bool ForwardErrorCorrection::RecoverPacket(const ReceivedFecPacket& fec_packet,
	RecoveredPacket* recovered_packet) {
	//初始化恢复包的pkt对象
	recovered_packet->pkt = new Packet();
	memcpy(recovered_packet->pkt->data, fec_packet.pkt->data, packet_size);
	recovered_packet->pkt->data_length = fec_packet.pkt->data_length;

	// 遍历FEC包中保护的所有媒体包。
	for (const auto& protected_packet : fec_packet.protected_packets) {
		if (protected_packet->pkt == nullptr) {
            // 如果protected_packet的pkt指针为nullptr，说明这就是我们要恢复的包。
            // 设置恢复包的组号和序列号为当前受保护包的组号和序列号。
			recovered_packet->group_number = protected_packet->group_number;
			recovered_packet->sequence_number = protected_packet->sequence_number;
			recovered_packet->pkt->packet_mask = 0; // 恢复包的包掩码初始化为0
			recovered_packet->pkt->group_number = protected_packet->group_number;
			recovered_packet->pkt->sequence_number = protected_packet->sequence_number;
			recovered_packet->pkt->k = fec_packet.pkt->k;
		}
		else {
            // 如果protected_packet的pkt指针不为nullptr，说明这个包已经被接收，
            // 我们使用它来通过异或操作恢复丢失的包。
			XorPayloads(protected_packet->pkt->data_length, protected_packet->pkt->data, &(recovered_packet->pkt->data_length), recovered_packet->pkt->data, packet_size);
		}
	}

    // 如果上述步骤都成功，则返回true，表示包恢复成功。
	return true;
}

template <typename S, typename T>
bool ForwardErrorCorrection::SortablePacket::LessThan::operator()(
	const S& first,
	const T& second) {
	return IsNewerSequenceNumber(second->group_number, second->sequence_number, first->group_number, first->sequence_number);
}

bool ForwardErrorCorrection::IsNewerSequenceNumber(uint8_t group_num1, uint8_t seq_num1, uint8_t group_num2, uint8_t seq_num2) {
	// 首先比较 group_number
	if (group_num1 != group_num2) {
		// 如果 group_num1 比 group_num2 大且差值小于16，或者 group_num1 比 group_num2 小且差值大于16，则 group_num1 更新
		return (static_cast<uint8_t>(group_num1 - group_num2) < 128);
	}
	// 如果 group_number 相同，则比较 sequence_number
	return seq_num1 > seq_num2;
}

void ForwardErrorCorrection::DiscardOldRecoveredPackets(
	RecoveredPacketList* recovered_packets) {
	// 将超出 max_media_packets 数量的恢复包移动到 buffer_packets 列表中以供重用
	while (recovered_packets->size() > max_media_packets) {
		auto& buffer_packet = recovered_packets->front();
		buffer_packets.push_back(std::move(buffer_packet));
		recovered_packets->pop_front();
	}
	RTC_DCHECK_LE(recovered_packets->size(), max_media_packets);
}

void ForwardErrorCorrection::ResetState(
	RecoveredPacketList* recovered_packets) {
	// Move all recovered packets to buffer_packets for reuse.
	for (const auto& recovered_packet : *recovered_packets) {
		auto buffer_packet = std::make_unique<ForwardErrorCorrection::RecoveredPacket>();
		buffer_packet->pkt = recovered_packet->pkt;
		buffer_packets.push_back(std::move(buffer_packet));
	}
	// Free the memory for any existing recovered packets, if the caller hasn't.
	recovered_packets->clear();
	received_fec_packets_.clear();
}

// recvfrom_fec
int ForwardErrorCorrection::RecvByUlpfec(SOCKET so, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
	static int ret;
	static int expected_packet_size;
	void* pkt_with_fpi = NULL;

	// buffer_packets为空时，才接收新包并进行FEC解码
	while (buffer_packets.empty()) {
		expected_packet_size = 2000;
		ret = get_next_pkt(so, &pkt_with_fpi, &expected_packet_size); // 这里有recvfrom操作
		if (ret == OF_STATUS_OK) {
			// 正确接收到一个包
			auto received_packet = std::make_unique<ForwardErrorCorrection::ReceivedPacket>();
			received_packet->pkt = rtc::scoped_refptr<ForwardErrorCorrection::Packet>(new ForwardErrorCorrection::Packet());
			uint8_t* data_ptr = static_cast<uint8_t*>(pkt_with_fpi);
			received_packet->pkt->packet_mask = (data_ptr[0] << 8) | data_ptr[1];
			received_packet->pkt->group_number = data_ptr[2];
			received_packet->pkt->sequence_number = data_ptr[3];
			received_packet->pkt->k = data_ptr[4];
			received_packet->pkt->data_length = (data_ptr[5] << 8) | data_ptr[6];
			memcpy(received_packet->pkt->data, data_ptr + fec_head_size, expected_packet_size - fec_head_size); // 拷贝数据部分

			// received_packet->group_number和sequence_number用于排序
			received_packet->group_number = received_packet->pkt->group_number;
			received_packet->sequence_number = received_packet->pkt->sequence_number;

			// 邢启航09_12:数据已拷贝完毕，可以安全释放（防止内存泄漏）
			free(pkt_with_fpi);
			pkt_with_fpi = NULL;

			// 记录最长数据字段长度
			if ((expected_packet_size - fec_head_size) > packet_size) {
				packet_size = expected_packet_size - fec_head_size;
			}

			// 执行FEC解码
			auto decode_result = DecodeFec(*received_packet, &recovered_packets);
		}
		else if (ret == OF_STATUS_ERROR) {
			// 接收出错，直接返回0表示无数据
			return 0;
		}
	}

	// 从buffer_packets中取出一个已恢复的包返回给上层应用
	if (!buffer_packets.empty()) {
		// 取出第一个包
		auto& buffer_packet = buffer_packets.front();
#if print_message
		printf("received SRC symbol: group_number=%u, sequence_number=%u, data_length=%u\n", buffer_packet->pkt->group_number, buffer_packet->pkt->sequence_number, buffer_packet->pkt->data_length);
#endif
		// 拷贝数据给上层应用
		memcpy(buf, buffer_packet->pkt->data, buffer_packet->pkt->data_length);
		len = buffer_packet->pkt->data_length;
		buffer_packets.pop_front();
	}
	else {
		// 正常情况下不会走到这里
		len = 0;
	}

	return len;
}