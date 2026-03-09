#pragma once

#include "protocol.h"
#include "global.h"

namespace AiSchedule
{
	// Tcp粘包拆包解析器
	class PacketParser
	{
	public:
		PacketParser() = default;

		//  给parser喂收到的字节，喂完之后取出所有解析好的完整包
		void Feed(const char* data, size_t len);

		//  尝试解析所有完整包，解析好的放到output，返回解析到的包个数
		size_t ParseAll(std::vector<AiSchedulePacket>& output);

		// 清理缓冲区，丢弃所有未解析的字节数据
		void Reset();
	private:

		bool CanParse() const;

		bool TryParseOne(AiSchedulePacket& out_packet);

		std::vector<char> m_buffer; // 存储未解析的字节数据
	};
}
