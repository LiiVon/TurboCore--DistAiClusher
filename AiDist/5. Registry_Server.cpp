#include "registry_server.h"

#include "json.hpp"

#include "protocol.h"

#include "logger.h"

#include "global.h"

#include <algorithm>

#include <chrono>

#include <functional>

#include <memory>

namespace AiSchedule
{
	using namespace TcFrame;

	RegistryServer::RegistryServer(EventLoop* loop, const Address& listen_addr)
		: m_loop(loop)
		, m_tcp_server(loop, listen_addr, 4)
	{
		// 绑定连接回调：处理连接建立/断开
		m_tcp_server.SetConnectionCallback(
			[this](const std::shared_ptr<TcpConnection>& conn)
			{
				OnConnection(conn);
			});

		// 绑定消息回调：处理收到的消息
		m_tcp_server.SetMessageCallback(
			[this](const std::shared_ptr<TcpConnection>& conn, Buffer* buf)
			{
				OnMessage(conn, buf);
			});

		LOG_INFO("RegistryServer construct success, listen on %s:%d",
			listen_addr.GetIp().c_str(), listen_addr.GetPort());
	}

	RegistryServer::~RegistryServer()
	{
		LOG_INFO("RegistryServer destructing...");
	}

	void RegistryServer::Start()
	{
		m_tcp_server.Start();
		// 启动定时扫描超时节点，10秒一次
		auto scan_task = std::make_shared<std::function<void()>>();
		*scan_task = [this, scan_task]() {
			// 扫描超时节点，拿到本次移除的节点列表
			std::vector<std::string> timeout_nodes = m_registry_manager.ScanTimeoutNodes();
			// 如果有超时节点，广播给所有连接，通知下游节点下线
			if (!timeout_nodes.empty()) {
				std::lock_guard<std::mutex> lock(m_conn_mtx);
				for (auto& node_id : timeout_nodes) {
					LOG_WARN("Node heartbeat timeout, remove and broadcast offline: %s", node_id.c_str());
					nlohmann::json offline_body;
					offline_body["node_id"] = node_id;
					// 遍历所有活跃连接发通知
					for (auto& conn : m_all_connections) {
						if (conn->IsConnected()) {
							SendResponse(conn, 0, PacketType::WORKER_OFFLINE, offline_body);
						}
					}
				}
			}
			// 10秒后再次执行
			m_loop->RunAfter(10 * 1000, *scan_task);
			};
		m_loop->RunAfter(10 * 1000, *scan_task);
		LOG_INFO("RegistryServer started.");
	}

	RegistryManager* RegistryServer::GetManager()
	{
		return &m_registry_manager;
	}

	void RegistryServer::OnConnection(const TcpConnectionPtr& conn)
	{
		std::lock_guard<std::mutex> lock(m_conn_mtx);
		if (conn->IsConnected())
		{
			LOG_INFO("New connection: " + conn->GetName());
			// 每个连接分配独立的Parser，不同连接数据不混，避免粘包错误
			auto parser = std::make_shared<PacketParser>();
			conn->SetContext(parser);
			m_all_connections.push_back(conn); // 新连接加入列表，后续广播用
		}
		else
		{
			LOG_INFO("Connection disconnected: " + conn->GetName());
			// 断开连接从列表移除，避免给断开的连接发消息
			auto it = std::remove_if(m_all_connections.begin(), m_all_connections.end(),
				[&conn](const std::shared_ptr<TcpConnection>& item) { return item.get() == conn.get(); });
			m_all_connections.erase(it, m_all_connections.end());
		}
	}

	void RegistryServer::OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf)
	{
		// 从连接上下文拿出当前连接的独立Parser
		auto parser = std::any_cast<std::shared_ptr<PacketParser>>(conn->GetContext());
		parser->Feed(buf->Peek(), buf->ReadableBytes());
		buf->Retrieve(buf->ReadableBytes()); // 消息已经被parser消费，清空缓冲区

		std::vector<AiSchedulePacket> packets;
		size_t parsed = parser->ParseAll(packets);
		if (parsed == 0)
		{
			return;
		}
		// 每个完整报文单独处理
		for (auto& packet : packets)
		{
			HandlePacket(conn, packet);
		}
	}

	void RegistryServer::HandlePacket(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet)
	{
		switch (packet.base_header.type)
		{
		case PacketType::REGISTER_WORKER:
			HandleRegister(conn, packet);
			break;
		case PacketType::HEARTBEAT:
			HandleHeartbeat(conn, packet);
			break;
		case PacketType::NODE_QUERY: // 专门用来按模型查询在线节点
			HandleNodeQuery(conn, packet);
			break;
		case PacketType::WORKER_REMOVE:
			HandleRemove(conn, packet);
			break;
		case PacketType::TASK_QUERY: // 任务查询归调度中心处理，注册中心不处理
			LOG_WARN("Task query is handled by scheduler, registry ignore this packet");
			break;
		default:
			LOG_WARN("RegistryServer receive unhandled packet type: %d, ignore", (int)packet.base_header.type);
			break;
		}
	}

	void RegistryServer::HandleRegister(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet)
	{
		// 反序列化注册请求
		WorkerRegisterReq req;
		try
		{
			req = WorkerRegisterReq::from_json(packet.body);
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("HandleRegister parse request failed: %s", e.what());
			// 发送失败响应
			WorkerRegisterResp resp;
			resp.success = false;
			resp.message = std::string("parse failed: ") + e.what();
			SendResponse(conn, packet.base_header.seq, PacketType::REGISTER_RESP, resp.to_json());
			return;
		}

		// 处理注册逻辑，填充节点基础信息
		RegistryNodeInfo node_info;
		node_info.node_addr = req.node_addr;
		node_info.support_models = req.support_models;
		node_info.total_memory_mb = req.total_memory_mb;
		node_info.available_memory_mb = req.total_memory_mb; // 刚注册时可用内存等于总内存
		node_info.weight = req.weight;
		node_info.last_heartbeat = std::chrono::steady_clock::now();

		std::string node_id = m_registry_manager.RegisterWorker(node_info);

		// 发送成功响应
		WorkerRegisterResp resp;
		resp.success = true;
		resp.node_id = node_id;
		resp.message = "register success";
		SendResponse(conn, packet.base_header.seq, PacketType::REGISTER_RESP, resp.to_json());
		LOG_INFO("Worker register success, node_id: %s, support %zu models", node_id.c_str(), node_info.support_models.size());
	}

	void RegistryServer::HandleHeartbeat(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet)
	{
		Heartbeat hb;
		try
		{
			hb = Heartbeat::from_json(packet.body);
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("HandleHeartbeat parse request failed: %s", e.what());
			return;
		}

		if (m_registry_manager.HasNode(hb.node_id)) {
			m_registry_manager.UpdateHeartbeat(hb.node_id, hb.available_memory_mb, hb.online_models);
			LOG_DEBUG("Receive heartbeat from node: %s, available memory: %d MB", hb.node_id.c_str(), hb.available_memory_mb);
			// 返回心跳ack，让Worker知道注册中心在线
			HeartbeatResp resp;
			resp.ack = true;
			resp.message = "ok";
			SendResponse(conn, packet.base_header.seq, PacketType::HEARTBEAT_RESP, resp.to_json());
		}
		else {
			HeartbeatResp resp;
			resp.ack = false;
			resp.message = "node not registered, please re-register";
			SendResponse(conn, packet.base_header.seq, PacketType::HEARTBEAT_RESP, resp.to_json());
			LOG_WARN("Heartbeat from unknown node: %s, request re-register", hb.node_id.c_str());
		}
	}

	void RegistryServer::HandleNodeQuery(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet)
	{
		// 处理调度中心查询：按模型名查询在线Worker节点列表
		NodeQueryReq req;
		try
		{
			req = NodeQueryReq::from_json(packet.body);
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("HandleNodeQuery parse request failed: %s", e.what());
			return;
		}

		// 从管理器获取过滤排序后的在线节点
		std::vector<RegistryNodeInfo> nodes = m_registry_manager.GetOnlineNodesByModel(req.model_name);

		// 序列化节点列表为json返回
		nlohmann::json resp_body = nlohmann::json::array();
		for (const auto& node : nodes)
		{
			nlohmann::json node_json;
			node_json["node_id"] = node.node_id;
			node_json["node_addr"] = node.node_addr;
			node_json["available_memory_mb"] = node.available_memory_mb;
			node_json["weight"] = node.weight;
			resp_body.push_back(node_json);
		}

		SendResponse(conn, packet.base_header.seq, PacketType::NODE_QUERY_RESP, resp_body);
		LOG_INFO("Handle node query for model %s, return %zu online nodes", req.model_name.c_str(), nodes.size());
	}

	void RegistryServer::HandleRemove(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet)
	{
		// 处理Worker主动下线请求
		WorkerRemoveReq req;
		try
		{
			req = WorkerRemoveReq::from_json(packet.body);
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("HandleRemove parse failed: %s", e.what());
			return;
		}

		// 先从管理器移除节点
		m_registry_manager.RemoveWorker(req.node_id);
		LOG_INFO("Worker remove success, node_id: %s", req.node_id.c_str());

		// 广播给所有连接，通知所有下游（调度中心/网关）该节点下线，方便重调度
		std::lock_guard<std::mutex> lock(m_conn_mtx);
		nlohmann::json offline_body;
		offline_body["node_id"] = req.node_id;
		for (auto& item : m_all_connections)
		{
			if (item->IsConnected())
			{
				SendResponse(item, 0, PacketType::WORKER_OFFLINE, offline_body);
			}
		}
		LOG_INFO("Broadcast worker offline notification done, node_id: %s", req.node_id.c_str());
	}

	void RegistryServer::SendResponse(const std::shared_ptr<TcpConnection>& conn,
		uint64_t req_seq, PacketType resp_type, nlohmann::json body)
	{
		// 构造统一格式的响应包
		AiSchedulePacket resp_packet;
		resp_packet.base_header.seq = req_seq; // 响应复用请求序列号，对端可以精准匹配请求和响应
		resp_packet.base_header.type = resp_type;
		resp_packet.base_header.sender_id = "registry_server";
		resp_packet.body = body;

		// 序列化发送
		std::vector<char> out_bytes;
		resp_packet.serialize(out_bytes);
		conn->Send(out_bytes.data(), (int)out_bytes.size());
	}
}
