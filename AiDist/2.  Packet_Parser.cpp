#include "packet_parser.h"

namespace AiSchedule
{
	void PacketParser::Feed(const char* data, size_t len)
	{
		m_buffer.insert(m_buffer.end(), data, data + len);
	}

	size_t PacketParser::ParseAll(std::vector<AiSchedulePacket>& out) {
		size_t parsed = 0;
		// 用协议提供的header_size，不用硬编码，永远不会错
		while (m_buffer.size() >= TransportHeader::header_size()) {
			// 先读传输头，拿到body长度
			TransportHeader header;
			if (!TransportHeader::deserialize_from_bytes(m_buffer.data(), m_buffer.size(), header)) {
				// 魔数不对，说明是脏数据，丢掉第一个字节重新对齐
				m_buffer.erase(m_buffer.begin());
				continue;
			}
			// 总长度 = 头大小 + body大小，判断缓冲区够不够一个完整包
			size_t packet_len = TransportHeader::header_size() + header.body_len;
			if (m_buffer.size() < packet_len) {
				// 不够一个完整包，等下次再读
				break;
			}
			// 解析完整包：第二个参数传当前包总长度，更规范
			AiSchedulePacket packet;
			if (AiSchedulePacket::deserialize(m_buffer.data(), packet_len, packet)) {
				out.push_back(packet);
				parsed++;
			}
			// 把已经解析的包从缓冲区删掉
			m_buffer.erase(m_buffer.begin(), m_buffer.begin() + packet_len);
		}
		return parsed;
	}

	void PacketParser::Reset()
	{
		m_buffer.clear();
	}

	bool PacketParser::CanParse() const
	{
		return m_buffer.size() >= TransportHeader::header_size();
	}

	bool PacketParser::TryParseOne(AiSchedulePacket& out_packet)
	{
		// 先解析传输头，看总长度是多少
		TransportHeader header;
		if (!TransportHeader::deserialize_from_bytes(m_buffer.data(), m_buffer.size(), header))
		{
			// 魔数不对，只删第一个字节重新对齐，不全清空，保留后面可能正确的数据
			m_buffer.erase(m_buffer.begin());
			return false;
		}

		// 看buffer里有没有足够一个完整包的数据：统一用协议提供的header_size
		size_t total_len = TransportHeader::header_size() + header.body_len;
		if (m_buffer.size() < total_len)
		{
			return false; // 数据不完整，等待更多数据
		}

		// 解析完整包
		if (!AiSchedulePacket::deserialize(m_buffer.data(), total_len, out_packet))
		{
			// 解析失败，只删当前错误包，保留后面数据
			m_buffer.erase(m_buffer.begin(), m_buffer.begin() + total_len);
			return false;
		}

		// 解析成功，丢弃buffer里这个包的数据，返回true
		m_buffer.erase(m_buffer.begin(), m_buffer.begin() + total_len);
		return true;
	}
}
