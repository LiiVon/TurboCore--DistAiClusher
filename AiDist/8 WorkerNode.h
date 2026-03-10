#pragma once

#include "protocol.h"

#include "eventloop.h"

#include "address.h"

#include "tcpconnection.h"

#include "tcpclient.h"

#include "packet_parser.h"

#include "global.h"

#include <atomic>

namespace AiSchedule
{
	using namespace TcFrame;
	class WorkerNode
	{
	public:
		WorkerNode(EventLoop* loop
			, const Address& registry_addr
			, std::string node_addr
			, int32_t total_memory_mb
			, int32_t weight
			, std::vector<std::string> support_models);

		~WorkerNode();

		void Start();  // 启动Worker节点，连接注册中心并开始心跳
		void Stop();
		void LoadModels(); // 加载支持的模型到显存
	private:
		// 连接注册中心回调
		void OnRegistryConnected();
		void OnRegistryDisconnected();

		// 接收注册中心消息回调
		void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buffer);
		void HandlePacket(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet);

		// 处理调度任务
		void HandleDispatch(const std::shared_ptr<TcpConnection>& conn, const AiSchedulePacket& packet);

		// 执行任务，模拟执行并上报结果
		void RunInferenceTask(const TaskDispatch& task);

		// 上报任务给调度中心
		void ReportResult(TaskResultReport result);

		// 发送心跳包给注册中心
		void SendHeartbeat();
		// 发起注册请求给注册中心
		void  DoRegister();

	private:
		EventLoop* m_loop;
		std::unique_ptr<TcpClient> m_registry_client; // 连接注册中心的TCP客户端
		Address m_registry_addr; // 注册中心地址
		std::string m_node_id; // 本节点ID，注册成功后由注册中心分配
		std::string m_node_addr; // 本节点对外服务地址，注册时告诉注册中心
		int32_t m_total_memory_mb; // 总GPU显存，注册时告诉注册中心
		int32_t m_weight; // 负载均衡权重，注册时告诉注册中心

		std::vector<std::string> m_support_models; // 支持的模型列表，注册时告诉注册中心
		std::atomic<int32_t> m_available_memory_mb; // 当前可用GPU显存，原子保证线程安全
		PacketParser m_parser;
	};
}
