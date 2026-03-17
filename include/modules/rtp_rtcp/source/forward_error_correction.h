/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_RTP_RTCP_SOURCE_FORWARD_ERROR_CORRECTION_H_
#define MODULES_RTP_RTCP_SOURCE_FORWARD_ERROR_CORRECTION_H_

#include <stddef.h>
#include <stdint.h>
#include <list>
#include <memory>
#include <vector>
#include <chrono>  
#include <immintrin.h> // 包含 AVX2 和 SSE 指令集，用于xor加速
#include <unordered_set>   // 支持 std::unordered_set
#include <unordered_map>  // 支持 std::unordered_map（索引哈希表用）
#include "absl/container/inlined_vector.h"
#include "api/scoped_refptr.h"
#include "modules/rtp_rtcp/source/forward_error_correction_internal.h"

typedef struct {
    int8_t group_number;
    int8_t sequence_number;
} SeqInfo;
class ForwardErrorCorrection {

 public:
#pragma pack(push, 1)  // 保存当前对齐方式，设置为1字节对齐
  class Packet {
   public:
    Packet();
    virtual ~Packet();

    // Add a reference.
    virtual int32_t AddRef();

    // Release a reference. Will delete the object if the reference count
    // reaches zero.
    virtual int32_t Release();

    uint16_t packet_mask;            // 2字节的掩码内容
    uint8_t  group_number;           // 1字节的组号信息
    uint8_t  sequence_number;        // 1字节的组内序号信息
    uint8_t  k;                      // 1字节的数据包数量信息
    uint16_t data_length;            // 2字节的数据长度信息

    static constexpr size_t kMaxDataSize = 2000; // Maximum size of data in a packet.
    uint8_t data[kMaxDataSize];      // 数据字段

    int32_t ref_count_;  // Counts the number of references to a packet.
  };
#pragma pack(pop)  // 恢复默认对齐方式

  // TODO(holmer): Refactor into a proper class.
  class SortablePacket {
  public:
      // Functor which returns true if the sequence number of `first`
      // is < the sequence number of `second`. Should only ever be called for
      // packets belonging to the same SSRC.
      struct LessThan {
          template <typename S, typename T>
          bool operator()(const S& first, const T& second);
      };

      uint8_t group_number;
      uint8_t sequence_number;
  };

  // Used for the input to DecodeFec().
  class ReceivedPacket : public SortablePacket {
  public:
      ReceivedPacket();
      ~ReceivedPacket();

      rtc::scoped_refptr<Packet> pkt;
  };

  // The recovered list parameter of DecodeFec() references structs of
  // this type.
  // TODO(holmer): Refactor into a proper class.
  class RecoveredPacket : public SortablePacket {
  public:
      RecoveredPacket();
      ~RecoveredPacket();

      rtc::scoped_refptr<Packet> pkt;  // Pointer to the packet storage.
  };

  // Used to link media packets to their protecting FEC packets.
  //
  // TODO(holmer): Refactor into a proper class.
  class ProtectedPacket : public SortablePacket {
  public:
      ProtectedPacket();
      ~ProtectedPacket();

      rtc::scoped_refptr<Packet> pkt;
  };

  using ProtectedPacketList = std::list<std::unique_ptr<ProtectedPacket>>;

  // Used for internal storage of received FEC packets in a list.
  //
  // TODO(holmer): Refactor into a proper class.
  class ReceivedFecPacket : public SortablePacket {
  public:
      ReceivedFecPacket();
      ~ReceivedFecPacket();

      ProtectedPacketList protected_packets;
      rtc::scoped_refptr<Packet> pkt;
  };


  using PacketList = std::list<std::unique_ptr<Packet>>;

  using RecoveredPacketList = std::list<std::unique_ptr<RecoveredPacket>>;

  using ReceivedFecPacketList = std::list<std::unique_ptr<ReceivedFecPacket>>;

  ~ForwardErrorCorrection();

  int EncodeFec(const std::vector<std::unique_ptr<Packet>>& media_packets,
                int32_t r,
                int num_important_packets,
                bool use_unequal_protection,
                FecMaskType fec_mask_type,
                std::vector<std::unique_ptr<Packet>>& fec_packets);

  // sendto_fec
  void SendByUlpfec(SOCKET so,
                    const char* buf,
                    int len,
                    int flags,
                    const sockaddr* to,
                    int tolen,
                    int k,
                    int r,
                    int bitrate,
                    double packet_loss_rate);

  void MediaPacketsInit(int k);

  void FecPacketsInit(int r);

  void NumberClear(SOCKET so, int flags, const sockaddr* to, int tolen);
	
  struct DecodeFecResult {
      // Number of recovered media packets using FEC.
      size_t num_recovered_packets = 0;
  };

  DecodeFecResult DecodeFec(const ReceivedPacket& received_packet,
                            RecoveredPacketList* recovered_packets);

  // Reset internal states from last frame and clear `recovered_packets`.
  // Frees all memory allocated by this class.
  void ResetState(RecoveredPacketList* recovered_packets);

  //recvfrom_fec
  int RecvByUlpfec(SOCKET so,
      char* buf,
      int len,
      int flags,
      sockaddr* from,
      int* fromlen);

  // 邢启航09_09添加内容：统计发送丢失数据包数量
  UINT32 total_lose_src_packets = 0;

  int32_t total_sent_packets = 0;

  int32_t total_sent_src_packets = 0;

  int32_t total_sent_fec_packets = 0;

  int32_t total_received_fec_packets = 0;

  int32_t total_received_packets = 0;

  int32_t total_received_src_packets = 0;

  int32_t total_src_packets_by_recovered = 0;

//   PacketList media_packets;

  PacketList send_buffer_packets;

  std::vector<std::vector<uint8_t>> recv_data_list;

  std::vector<std::unique_ptr<Packet>> media_packets;

  std::vector<std::unique_ptr<Packet>> fec_packets;

  int current_packet_index_ = 0; // 当前使用的 Packet 索引

 private:

  void XorPayloads( uint16_t src_data_length,
                           const uint8_t* src, 
                           uint16_t* dst_data_length,
                           uint8_t* dst, 
                           size_t length);

  // Inserts the `received_packet` into the internal received FEC packet list
  // or into `recovered_packets`.
  void InsertPacket(const ReceivedPacket& received_packet,
      RecoveredPacketList* recovered_packets);

  // Inserts the `received_packet` into `recovered_packets`. Deletes duplicates.
  void InsertMediaPacket(RecoveredPacketList* recovered_packets,
      const ReceivedPacket& received_packet);

  // Assigns pointers to the recovered packet from all FEC packets which cover
  // it.
  // Note: This reduces the complexity when we want to try to recover a packet
  // since we don't have to find the intersection between recovered packets and
  // packets covered by the FEC packet.
  void UpdateCoveringFecPackets(const RecoveredPacket& packet);

  // Insert `received_packet` into internal FEC list. Deletes duplicates.
  void InsertFecPacket(const RecoveredPacketList& recovered_packets,
      const ReceivedPacket& received_packet);

  // Assigns pointers to already recovered packets covered by `fec_packet`.
  static void AssignRecoveredPackets(
      const RecoveredPacketList& recovered_packets,
      ReceivedFecPacket* fec_packet);

  // Attempt to recover missing packets, using the internally stored
  // received FEC packets.
  size_t AttemptRecovery(RecoveredPacketList* recovered_packets);

  // Recover a missing packet.
  bool RecoverPacket(const ReceivedFecPacket& fec_packet,
      RecoveredPacket* recovered_packet);

  // Get the number of missing media packets which are covered by `fec_packet`.
  // An FEC packet can recover at most one packet, and if zero packets are
  // missing the FEC packet can be discarded. This function returns 2 when two
  // or more packets are missing.
  static int NumCoveredPacketsMissing(const ReceivedFecPacket& fec_packet);

  static bool IsNewerSequenceNumber(uint8_t group_num1, uint8_t seq_num1, uint8_t group_num2, uint8_t seq_num2);

  // Discards old packets in `recovered_packets`, which are no longer relevant
  // for recovering lost packets.
  void DiscardOldRecoveredPackets(RecoveredPacketList* recovered_packets);

  SeqInfo getNextSeq(const RecoveredPacket& packet);

  uint8_t packet_masks_[kUlpfecMaxMediaPackets * kUlpfecMaxPacketMaskSize];

  size_t packet_mask_size_;

  int kNumImportantPackets = 0;

  bool kUseUnequalProtection = false;

  int group_number = 0;

  int sequence_number = 0;

  ReceivedFecPacketList received_fec_packets_;

  RecoveredPacketList recovered_packets;

  RecoveredPacketList buffer_packets;

  size_t max_media_packets = 16;  // recovered_packets中最大数据包数量

  size_t max_fec_packets = 16;    // received_fec_packets_中最大FEC包数量

  int packet_size = 0;

  int fec_head_size = 7; // ULPFEC头部大小
  // recovered_packets和received_fec_packets_中包的索引哈希表
  std::unordered_map<uint16_t, RecoveredPacketList::iterator> media_packet_index_;
  std::unordered_set<uint16_t> existing_fec_keys_;

  SeqInfo takeout_seq1 = { 0, 0 };
};

#endif  // MODULES_RTP_RTCP_SOURCE_FORWARD_ERROR_CORRECTION_H_
