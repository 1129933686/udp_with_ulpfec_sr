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

// 初次发送，序号清零
void ForwardErrorCorrection::NumberClear(SOCKET so, int flags, const sockaddr* to, int tolen) {
	std::vector<uint8_t> send_data = {0x20, 0x02, 0x04, 0x22};
	if (sendto(so, reinterpret_cast<const char*>(send_data.data()), static_cast<int>(send_data.size()), flags, to, tolen) == SOCKET_ERROR) {
		OF_PRINT_ERROR(("sendto() failed!\n"))
		return;
	}
	printf("FEC:sending number clear signal successfully.\n");
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
	int ret;
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
				// 生成旧包的唯一键，从哈希表中删除
				uint16_t old_fec_key = (static_cast<uint16_t>((*it)->pkt->group_number) << 8)
					| static_cast<uint16_t>((*it)->pkt->sequence_number);
				existing_fec_keys_.erase(old_fec_key);
				// 从列表中删除旧的FEC包
				it = received_fec_packets_.erase(it); 
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

	// ========== 优化后的重复包检测（O(1)复杂度） ==========
    // 生成唯一键：group_number（8位）左移8位 + sequence_number（8位），组合为uint16_t
	uint16_t fec_key = (static_cast<uint16_t>(received_packet.pkt->group_number) << 8)
		| static_cast<uint16_t>(received_packet.pkt->sequence_number);

	// 哈希表查重：存在则直接返回（丢弃重复包）
	if (existing_fec_keys_.count(fec_key)) {
		return;
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

		// 找到插入位置：遍历列表，找到第一个比新包“大”的元素，插入到其前面
		auto insert_it = received_fec_packets_.begin();
		while (insert_it != received_fec_packets_.end()) {
			// 正序排序
			if (SortablePacket::LessThan()(fec_packet.get(), insert_it->get())) {
				break;
			}
			++insert_it;
		}
		// 插入到指定位置，维持列表有序
		received_fec_packets_.insert(insert_it, std::move(fec_packet));
		// 将新包的键存入哈希表
		existing_fec_keys_.insert(fec_key);

		// 溢出时同步删除哈希表中的键
		if (received_fec_packets_.size() > max_fec_packets) {
			const auto& old_fec = received_fec_packets_.front();
			// 生成旧包的唯一键，从哈希表中删除
			uint16_t old_fec_key = (static_cast<uint16_t>(old_fec->pkt->group_number) << 8)
				| static_cast<uint16_t>(old_fec->pkt->sequence_number);
			existing_fec_keys_.erase(old_fec_key);
			// 移除旧包（原有逻辑不变）
			received_fec_packets_.pop_front();
		}
		RTC_DCHECK_LE(received_fec_packets_.size(), max_fec_packets);
	}
}


void ForwardErrorCorrection::InsertMediaPacket(
	RecoveredPacketList* recovered_packets,
	const ReceivedPacket& received_packet) {
	// ========== 第一步：哈希表O(1)查重 ==========
	// 生成媒体包唯一键：group_number（8位）<<8 + sequence_number（8位）→ uint16_t（无冲突）
	uint16_t media_key = (static_cast<uint16_t>(received_packet.group_number) << 8)
		| static_cast<uint16_t>(received_packet.sequence_number);
	// 查重：存在则直接返回（丢弃重复包）
	if (media_packet_index_.count(media_key)) {
		return;
	}

	// ========== 第二步：创建新媒体包 ==========
	// 创建一个新的RecoveredPacket对象来存储接收到的媒体包信息。
	std::unique_ptr<RecoveredPacket> recovered_packet(new RecoveredPacket());
	recovered_packet->group_number = received_packet.group_number;
	recovered_packet->sequence_number = received_packet.sequence_number;
	recovered_packet->pkt = received_packet.pkt;

	// ========== 第三步：有序插入（保留原有LessThan比较，维持列表有序） ==========
	SortablePacket::LessThan less_than;
	// 找到第一个比新包“大”（更新）的元素位置，插入到前面（维持旧→新升序）
	auto insert_it = recovered_packets->begin();
	while (insert_it != recovered_packets->end() && less_than(*insert_it, recovered_packet.get())) {
		++insert_it;
	}
	// 插入到指定位置（原有有序插入逻辑不变）
	auto pkt_it = recovered_packets->insert(insert_it, std::move(recovered_packet));
	RecoveredPacket* recovered_packet_ptr = pkt_it->get();

	// ========== 第四步：仅更新索引哈希表 ==========
	media_packet_index_[media_key] = pkt_it; 

	// ========== 核心优化：哈希表O(1)匹配takeout_seq1（不变） ==========
	bool hasMatch = true;
	while (hasMatch) {
		hasMatch = false;
		uint16_t takeout_key = (static_cast<uint16_t>(takeout_seq1.group_number) << 8)
			| static_cast<uint16_t>(takeout_seq1.sequence_number);

		auto find_it = media_packet_index_.find(takeout_key);
		if (find_it != media_packet_index_.end()) {
			auto& packet = *(find_it->second);
			// 创建RecoveredPacket的副本
		    auto buffer_packet = std::make_unique<ForwardErrorCorrection::RecoveredPacket>();
			buffer_packet->group_number = packet->group_number;
			buffer_packet->sequence_number = packet->sequence_number;
			buffer_packet->pkt = packet->pkt;
			// 更新takeout_seq1并入队到buffer_packets
			takeout_seq1 = getNextSeq(*buffer_packet);
			buffer_packets.push_back(std::move(buffer_packet));
			hasMatch = true;
		}
	}

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
				// 生成旧包的唯一键，从哈希表中删除
				uint16_t old_fec_key = (static_cast<uint16_t>((*fec_packet_it)->pkt->group_number) << 8)
					| static_cast<uint16_t>((*fec_packet_it)->pkt->sequence_number);
				existing_fec_keys_.erase(old_fec_key);
				// 如果无法恢复该包，则丢弃当前 FEC 包
				fec_packet_it = received_fec_packets_.erase(fec_packet_it);
				continue; // 继续下一个 FEC 包的处理
			}

			++num_recovered_packets; // 成功恢复一个包，计数加一

			// 生成媒体包唯一键：group_number（8位）<<8 + sequence_number（8位）→ uint16_t（无冲突）
			uint16_t media_key = (static_cast<uint16_t>(recovered_packet->group_number) << 8)
				| static_cast<uint16_t>(recovered_packet->sequence_number);

			// 查重：存在则继续下一个 FEC 包的处理
			if (media_packet_index_.count(media_key)) {
				continue;
			}

			// 有序插入
			SortablePacket::LessThan less_than;
			// 找到第一个比新包“大”（更新）的元素位置，插入到前面（维持旧→新升序）
			auto insert_it = recovered_packets->begin();
			while (insert_it != recovered_packets->end() && less_than(*insert_it, recovered_packet.get())) {
				++insert_it;
			}

			// 插入到指定位置（原有有序插入逻辑不变）
			auto pkt_it = recovered_packets->insert(insert_it, std::move(recovered_packet));
			RecoveredPacket* recovered_packet_ptr = pkt_it->get();
			// 第四步：仅更新索引哈希表
			media_packet_index_[media_key] = pkt_it; // 一次插入，同时支持查重和定位

			// 哈希表O(1)匹配takeout_seq1
			bool hasMatch = true;
			while (hasMatch) {
				hasMatch = false;
				uint16_t takeout_key = (static_cast<uint16_t>(takeout_seq1.group_number) << 8)
					| static_cast<uint16_t>(takeout_seq1.sequence_number);

				auto find_it = media_packet_index_.find(takeout_key);
				if (find_it != media_packet_index_.end()) {
					auto& packet = *(find_it->second);
					// 创建RecoveredPacket的副本
					auto buffer_packet = std::make_unique<ForwardErrorCorrection::RecoveredPacket>();
					buffer_packet->group_number = packet->group_number;
					buffer_packet->sequence_number = packet->sequence_number;
					buffer_packet->pkt = packet->pkt;
					// 更新takeout_seq1并入队到buffer_packets
					takeout_seq1 = getNextSeq(*buffer_packet);
					buffer_packets.push_back(std::move(buffer_packet));
					hasMatch = true;
				}
			}

			// 更新所有覆盖该媒体包的 FEC 包的状态
			UpdateCoveringFecPackets(*recovered_packet_ptr);

			// 丢弃旧的已恢复包，保持列表的整洁
			DiscardOldRecoveredPackets(recovered_packets);

			// 生成旧包的唯一键，从哈希表中删除
			uint16_t old_fec_key = (static_cast<uint16_t>((*fec_packet_it)->pkt->group_number) << 8)
				| static_cast<uint16_t>((*fec_packet_it)->pkt->sequence_number);
			existing_fec_keys_.erase(old_fec_key);

			// 删除当前处理的 FEC 包，因为它已经被处理
			fec_packet_it = received_fec_packets_.erase(fec_packet_it);

			// 一旦恢复了一个包，检查是否可以恢复其他包
			// 由于丢失的包数量现在可能减少，因此重新开始处理 FEC 包
			fec_packet_it = received_fec_packets_.begin();
		}
		else if (packets_missing == 0) {
			// 如果当前 FEC 包下所有媒体包都已收到或恢复，则删除该 FEC 包
			// 生成旧包的唯一键，从哈希表中删除
			uint16_t old_fec_key = (static_cast<uint16_t>((*fec_packet_it)->pkt->group_number) << 8)
				| static_cast<uint16_t>((*fec_packet_it)->pkt->sequence_number);
			existing_fec_keys_.erase(old_fec_key);
			// 删除该 FEC 包
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
	// 将超出大小的recovered_packet移出，并判断是否移入buffer_packets
	while (recovered_packets->size() > max_media_packets) {
		auto& frontRecovered = recovered_packets->front();
		// ========== 步骤1：生成队头包的唯一哈希键 ==========
		uint16_t media_key = (static_cast<uint16_t>(frontRecovered->group_number) << 8)
			| static_cast<uint16_t>(frontRecovered->sequence_number);
		// ========== 步骤2：复用环形序号判断逻辑，判断是否满足出队条件 ==========
		bool isMatch = false;
		// 条件：frontRecovered 比 takeout_seq1 新，或者两者序号完全相等
        // 分两步判断：先判断是否相等，再判断是否更新
		bool is_equal = (frontRecovered->group_number == takeout_seq1.group_number) &&
			            (frontRecovered->sequence_number == takeout_seq1.sequence_number);
		// IsNewerSequenceNumber(a_g, a_s, b_g, b_s) → 返回 (a_g,a_s) 是否比 (b_g,b_s) 新
		bool is_newer = IsNewerSequenceNumber(
			frontRecovered->group_number, frontRecovered->sequence_number,
			takeout_seq1.group_number, takeout_seq1.sequence_number
		);

		// 满足条件：相等 或 更旧（这里逻辑要注意：出队时队头是旧包，需要判断是否“需要处理”）
		// 修正逻辑：队头包是列表中最旧的包（因为列表是降序），超出大小时要判断是否“仍需处理”
		// 需处理的条件：队头包 不早于 takeout_seq1（即 相等 或 更新）
		if (is_equal || is_newer) {
			isMatch = true;
		}
		if (isMatch) {
			// 更新takeout_seq1并入队到buffer_packets
			takeout_seq1 = getNextSeq(*frontRecovered);
			buffer_packets.push_back(std::move(frontRecovered));
			// ========== 关键：出队前删除哈希表索引 ==========
			media_packet_index_.erase(media_key); // 同步删除哈希表索引
			// 移除前端元素（自动释放内存）
			recovered_packets->pop_front();
		}
		else {
			// 不满足条件时，直接移除该元素（队头包已过时，无需处理）
			// ========== 关键：出队前删除哈希表索引 ==========
			media_packet_index_.erase(media_key); // 同步删除哈希表索引
			// 移除队头元素（列表出队）
			recovered_packets->pop_front();
		}
	}
	RTC_DCHECK_LE(recovered_packets->size(), max_media_packets);
}


void ForwardErrorCorrection::ResetState(
	RecoveredPacketList* recovered_packets) {
	while (!recovered_packets->empty()) {
		auto& frontRecovered = recovered_packets->front();

		// ========== 步骤1：生成包的唯一哈希键（与插入时逻辑一致） ==========
		uint16_t media_key = (static_cast<uint16_t>(frontRecovered->group_number) << 8)
			| static_cast<uint16_t>(frontRecovered->sequence_number);

		// ========== 步骤2：复用环形序号判断逻辑，判断是否满足入队条件 ==========
		bool isMatch = false;

		// 1. 判断是否与 takeout_seq1 完全相等
		bool is_equal = (frontRecovered->group_number == takeout_seq1.group_number) &&
			(frontRecovered->sequence_number == takeout_seq1.sequence_number);

		// 2. 判断是否比 takeout_seq1 新（复用修正后的环形判断逻辑）
		bool is_newer = ForwardErrorCorrection::IsNewerSequenceNumber(
			frontRecovered->group_number, frontRecovered->sequence_number,
			takeout_seq1.group_number, takeout_seq1.sequence_number
		);

		// 满足条件：相等 或 更旧（这里逻辑要注意：出队时队头是旧包，需要判断是否“需要处理”）
		// 修正逻辑：队头包是列表中最旧的包（因为列表是降序），超出大小时要判断是否“仍需处理”
		// 需处理的条件：队头包 不早于 takeout_seq1（即 相等 或 更新）
		if (is_equal || is_newer) {
			isMatch = true;
		}

		if (isMatch) {
			// 更新takeout_seq1并入队到fec_buffer_packets
			takeout_seq1 = getNextSeq(*frontRecovered);
			buffer_packets.push_back(std::move(frontRecovered));
			// ========== 关键：出队前删除哈希表索引 ==========
			media_packet_index_.erase(media_key); // 同步删除索引
			// 移除前端元素（自动释放内存）
			recovered_packets->pop_front();
		}
		else {
			// 不满足条件时，直接移除该元素（已过时，无需处理）
			// ========== 关键：出队前删除哈希表索引（线程安全） ==========
			media_packet_index_.erase(media_key); // 同步删除索引
			// 移除队头元素（列表出队）
			recovered_packets->pop_front();
		}
	}

	// 清空冗余包列表和冗余包的哈希表
	existing_fec_keys_.clear();
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
			// 判断是否需要进行重置操作
			if (expected_packet_size == 4) {
				uint8_t* data_ptr = static_cast<uint8_t*>(pkt_with_fpi);
				if (data_ptr[0] == 0x20 && data_ptr[1] == 0x02 && data_ptr[2] == 0x04 && data_ptr[3] == 0x22) {
					// 打印重置包信息
					printf("FEC:received number clear signal successfully\n");
					// 清空recovered_packets（判断是否加入buffer_packets）、received_fec_packets_
					ResetState(&recovered_packets);
					// 重置序号
					takeout_seq1 = { 0, 0 };
				}
			}
			else {
				// 非重置包，继续正常处理
				auto received_packet = std::make_unique<ForwardErrorCorrection::ReceivedPacket>();
				received_packet->pkt = rtc::scoped_refptr<ForwardErrorCorrection::Packet>(new ForwardErrorCorrection::Packet());
				uint8_t* data_ptr = static_cast<uint8_t*>(pkt_with_fpi);
				received_packet->pkt->packet_mask = (data_ptr[0] << 8) | data_ptr[1];
				received_packet->pkt->group_number = data_ptr[2];
				received_packet->pkt->sequence_number = data_ptr[3];
				received_packet->pkt->k = data_ptr[4];
				received_packet->pkt->data_length = (data_ptr[5] << 8) | data_ptr[6];
				//printf("receiving symbol: group_number=%u, sequence_number=%u, data_length=%u\n", received_packet->pkt->group_number, received_packet->pkt->sequence_number, received_packet->pkt->data_length);
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
		// 打印包信息
		//printf("received SRC symbol: group_number=%u, sequence_number=%u, data_length=%u\n", buffer_packet->pkt->group_number, buffer_packet->pkt->sequence_number, buffer_packet->pkt->data_length);
		// 拷贝数据给上层应用
		memcpy(buf, buffer_packet->pkt->data, buffer_packet->pkt->data_length);
		len = buffer_packet->pkt->data_length;
		//if (len == 0) {
		//	printf("Error: received packet with zero length!\n");
		//}
		buffer_packets.pop_front();
	}
	else {
		// 正常情况下不会走到这里
		len = 0;
	}

	return len;
}

// 计算下一个takeout_seq1的值
SeqInfo ForwardErrorCorrection::getNextSeq(const RecoveredPacket& packet) {
	SeqInfo nextSeq;
	nextSeq.group_number = packet.group_number;
	nextSeq.sequence_number = packet.sequence_number + 1;

	if (nextSeq.sequence_number >= packet.pkt->k) {
		nextSeq.sequence_number = 0;
		nextSeq.group_number = nextSeq.group_number + 1;
	}
	return nextSeq;
}