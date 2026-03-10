#pragma once

#include "registry_manager.h"
#include "protocol.h"
#include "packet_parser.h"
#include "tcpserver.h"
#include "eventloop.h"
#include "address.h"
#include "global.h"

namespace AiSchedule
{
	using namespace TcFrame;

	class RegistryServer
	{
	public:
		RegistryServer(EventLoop* loop, const Address& listen_addr);
		~RegistryServer();
		void Start();
		RegistryManager* GetManager();

		// 连接事件处理：新连接上来绑定Parser，断开清理
		void OnConnection(const TcpConnectionPtr& conn);

		// 消息事件：把收到的字节喂给Parser，解析后处理
		void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);

		void HandlePacket(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet);
		void HandleRegister(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet);
		void HandleHeartbeat(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet);
		void HandleNodeQuery(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet);
		void HandleRemove(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet);

		// 统一发响应工具
		void SendResponse(const std::shared_ptr<TcpConnection>& conn,
			uint64_t req_seq, PacketType resp_type, nlohmann::json body);

	private:
		EventLoop* m_loop;
		TcpServer m_tcp_server;
		RegistryManager m_registry_manager; // 注册中心管理器
		std::mutex m_conn_mtx; // 保护连接列表的线程安全锁（多线程操作连接列表必须加锁）
		std::vector<std::shared_ptr<TcpConnection>> m_all_connections; // 保存所有活跃连接，用来广播下线
	};
}
