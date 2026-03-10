#include "workernode.h"
#include "logger.h"
#include "protocol.h"
#include "global.h"
#include <chrono>
#include <thread>
#include <functional>

namespace AiSchedule
{
	using namespace TcFrame;

	WorkerNode::WorkerNode(EventLoop* loop
		, const Address& registry_addr
		, std::string node_addr
		, int32_t total_memory_mb
		, int32_t weight
		, std::vector<std::string> support_models)
		: m_loop(loop)
		, m_registry_addr(registry_addr)
		, m_node_addr(std::move(node_addr))
		, m_total_memory_mb(total_memory_mb)
		, m_weight(weight)
		, m_support_models(std::move(support_models))
		, m_available_memory_mb(total_memory_mb)
		, m_registry_client(std::make_unique<TcpClient>(m_loop, m_registry_addr))
	{
		m_registry_client->SetConnectionCallback(
			[this](const TcpConnectionPtr& conn) {
				if (conn->IsConnected())
				{
					OnRegistryConnected();
				}
				else
				{
					OnRegistryDisconnected();
				}
			});
		m_registry_client->SetMessageCallback(
			[this](const TcpConnectionPtr& conn, Buffer* buf) {
				OnMessage(conn, buf);
			});
	}

	WorkerNode::~WorkerNode()
	{
		LOG_DEBUG("WorkerNode destructor");

		// 主动发送下线请求给注册中心
		if (!m_node_id.empty() && m_registry_client->GetConnection()
			&& m_registry_client->GetConnection()->IsConnected())
		{
			WorkerRemoveReq req;
			req.node_id = m_node_id;

			AiSchedulePacket packet;
			packet.base_header.seq = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
			packet.base_header.type = PacketType::WORKER_REMOVE;
			packet.base_header.sender_id = m_node_id;
			packet.body = req.to_json();

			std::vector<char> out_bytes;
			packet.serialize(out_bytes);
			m_registry_client->GetConnection()->Send(out_bytes.data(), (int)out_bytes.size());
			LOG_INFO("Send主动下线请求 to registry, node_id: %s", m_node_id.c_str());
		}
	}

	void WorkerNode::LoadModels() {
		LOG_INFO("Start loading supported models...");
		for (const auto& model_name : m_support_models) {
			// ========== 这里替换成你实际的模型加载代码 ==========
			// 示例：假设每个7B模型占用14336MB（14GB）
			// int32_t model_size = 14336;
			// m_available_memory_mb -= model_size;
			// m_models[model_name] = std::make_unique<ModelInference>(model_path);
			LOG_INFO("Model %s loaded success", model_name.c_str());
		}
		LOG_INFO("All models loaded, available memory: %dMB", m_available_memory_mb.load());
	}

	void WorkerNode::Start()
	{
		LoadModels(); // 先加载模型，再连接注册中心
		m_registry_client->Connect();
		LOG_INFO("WorkerNode start, connect to registry: %s:%d",
			m_registry_addr.GetIp().c_str(), m_registry_addr.GetPort());
	}

	void WorkerNode::Stop()
	{
		LOG_INFO("WorkerNode stopping, node_id: %s", m_node_id.c_str());
		if (m_registry_client)
		{
			m_registry_client->Disconnect();
		}
	}

	void WorkerNode::OnRegistryConnected()
	{
		LOG_INFO("Connected to registry: %s:%d", m_registry_addr.GetIp().c_str(), m_registry_addr.GetPort());
		DoRegister();

		// 启动心跳定时器，每30秒发送一次心跳，不用静态变量，更安全
		auto heartbeat_task = std::make_shared<std::function<void()>>();
		*heartbeat_task = [this, heartbeat_task]() {
			SendHeartbeat();
			m_loop->RunAfter(30 * 1000, *heartbeat_task);
			};

		// 1秒后发第一次心跳
		m_loop->RunAfter(1 * 1000, *heartbeat_task);
	}

	void WorkerNode::OnRegistryDisconnected()
	{
		LOG_WARN("Disconnected from registry, will try reconnect after 3s");
		m_node_id.clear();
		// 3秒自动重连
		m_loop->RunAfter(3 * 1000, [this]() {
			m_registry_client->Connect();
			});
	}

	void WorkerNode::OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf)
	{
		// 喂给粘包解析器
		m_parser.Feed(buf->Peek(), buf->ReadableBytes());
		buf->Retrieve(buf->ReadableBytes());

		std::vector<AiSchedulePacket> packets;
		size_t parsed_cnt = m_parser.ParseAll(packets);
		if (parsed_cnt == 0)
		{
			LOG_WARN("Received message but failed to parse any packet, maybe invalid data");
			return;
		}

		for (auto& packet : packets)
		{
			HandlePacket(conn, packet);
		}
	}

	void WorkerNode::HandlePacket(const std::shared_ptr<TcpConnection>& conn, AiSchedulePacket& packet)
	{
		switch (packet.base_header.type)
		{
			// 处理注册中心的注册响应
		case PacketType::REGISTER_RESP:
		{
			WorkerRegisterResp resp;
			try
			{
				resp = WorkerRegisterResp::from_json(packet.body);
			}
			catch (const std::exception& e)
			{
				LOG_ERROR("Parse register response failed: %s", e.what());
				break;
			}

			if (resp.success)
			{
				m_node_id = resp.node_id;
				LOG_INFO("Worker register SUCCESS, assigned node_id: %s", m_node_id.c_str());
			}
			else
			{
				LOG_ERROR("Worker register FAILED: %s", resp.message.c_str());
			}
			break;
		}
		// 处理心跳响应，检测是否被注册中心剔除
		case PacketType::HEARTBEAT_RESP:
		{
			HeartbeatResp resp;
			try {
				resp = HeartbeatResp::from_json(packet.body);
			}
			catch (const std::exception& e) {
				LOG_ERROR("Parse heartbeat resp failed: %s", e.what());
				break;
			}
			if (!resp.ack) {
				LOG_WARN("Heartbeat ack failed: %s, will re-register after 3s", resp.message.c_str());
				// 注册中心不认可，断开重连重新注册
				m_registry_client->Disconnect();
				m_loop->RunAfter(3 * 1000, [this]() {
					m_registry_client->Connect();
					});
			}
			else {
				LOG_DEBUG("Heartbeat ack success");
			}
			break;
		}
		// 处理调度中心分发来的任务
		case PacketType::TASK_DISPATCH:
		{
			HandleDispatch(conn, packet);
			break;
		}
		default:
		{
			LOG_WARN("Worker receive unhandled packet type: %d, ignore", (int)packet.base_header.type);
			break;
		}
		}
	}

	void WorkerNode::HandleDispatch(const std::shared_ptr<TcpConnection>& conn, const AiSchedulePacket& packet)
	{
		TaskDispatch task;
		try
		{
			task = TaskDispatch::from_json(packet.body);
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("Parse TaskDispatch failed: %s", e.what());
			return;
		}
		LOG_INFO("Received TaskDispatch, task_id: %s, model: %s",
			task.task_id.c_str(), task.model_name.c_str());
		// 执行推理任务
		RunInferenceTask(task);
	}

	void WorkerNode::RunInferenceTask(const TaskDispatch& task)
	{
		// 推理前占用显存：假设每个输入占用256MB，可根据实际模型修改
		int32_t used_mem = (int32_t)task.inputs.size() * 256;
		m_available_memory_mb -= used_mem;

		LOG_INFO("Running inference task, task_id: %s, slice_id: %d, used %dMB",
			task.task_id.c_str(), task.slice_id, used_mem);

		// ========== 这里替换成你实际的推理代码 ==========
		// 模拟推理耗时，1-5秒随机，贴近真实推理场景
		int simulate_ms = 1000 + (std::rand() % 4000);
		std::this_thread::sleep_for(std::chrono::milliseconds(simulate_ms));

		// 构造结果上报，完全对齐协议定义
		TaskResultReport result;
		result.task_id = task.task_id;
		result.slice_id = task.slice_id;
		result.success = true;
		result.cost_ms = simulate_ms;
		result.message = "inference execute success";
		result.outputs = { "simulate_result_" + task.task_id + "_slice_" + std::to_string(task.slice_id) };
		// 从任务中获取调度中心地址，正确上报
		result.scheduler_ip = task.scheduler_ip;
		result.scheduler_port = task.scheduler_port;

		// 推理完成释放显存
		m_available_memory_mb += used_mem;

		LOG_INFO("Inference task finished, task_id: %s slice: %d cost %dms",
			task.task_id.c_str(), task.slice_id, simulate_ms);

		ReportResult(result);
	}

	void WorkerNode::ReportResult(TaskResultReport result)
	{
		AiSchedulePacket packet;
		packet.base_header.seq = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
		packet.base_header.type = PacketType::TASK_RESULT_REPORT;
		packet.base_header.sender_id = m_node_id;
		packet.body = result.to_json();

		std::vector<char> out_bytes;
		packet.serialize(out_bytes);

		// 按照任务里的地址，连接调度中心上报结果，发完释放
		Address scheduler_addr(result.scheduler_ip.c_str(), result.scheduler_port);
		TcpClient* report_client = new TcpClient(m_loop, scheduler_addr);
		report_client->SetConnectionCallback([report_client, out_bytes, this, result](const TcpConnectionPtr& conn) {
			if (conn->IsConnected())
			{
				conn->Send(out_bytes.data(), (int)out_bytes.size());
				LOG_INFO("Report task result success to scheduler, task_id: %s, slice_id: %d",
					result.task_id.c_str(), result.slice_id);
				// 发完主动断开，释放client，不会内存泄漏
				conn->Shutdown();
				delete report_client;
			}
			});
		report_client->Connect();
	}

	void WorkerNode::SendHeartbeat()
	{
		if (m_node_id.empty())
		{
			LOG_WARN("Can't send heartbeat, not registered yet");
			return;
		}

		// 获取当前动态更新的可用显存
		int32_t available_memory_mb = m_available_memory_mb.load();

		Heartbeat hb;
		hb.node_id = m_node_id;
		hb.available_memory_mb = available_memory_mb;
		hb.online_models = m_support_models;

		AiSchedulePacket packet;
		packet.base_header.seq = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
		packet.base_header.type = PacketType::HEARTBEAT;
		packet.base_header.sender_id = m_node_id;
		packet.body = hb.to_json();

		std::vector<char> out_bytes;
		packet.serialize(out_bytes);

		auto conn = m_registry_client->GetConnection();
		if (conn && conn->IsConnected())
		{
			conn->Send(out_bytes.data(), (int)out_bytes.size());
			LOG_DEBUG("Sent heartbeat to registry, node_id: %s available %dMB",
				m_node_id.c_str(), available_memory_mb);
		}
		else
		{
			LOG_WARN("Registry not connected, heartbeat not sent");
		}
	}

	void WorkerNode::DoRegister()
	{
		WorkerRegisterReq req;
		req.node_addr = m_node_addr;
		req.total_memory_mb = m_total_memory_mb;
		req.weight = m_weight;
		req.support_models = m_support_models;

		AiSchedulePacket packet;
		packet.base_header.seq = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
		packet.base_header.type = PacketType::REGISTER_WORKER;
		packet.base_header.sender_id = m_node_addr; // 注册前用node_addr当sender_id，合理
		packet.body = req.to_json();

		std::vector<char> out_bytes;
		packet.serialize(out_bytes);

		auto conn = m_registry_client->GetConnection();
		if (conn && conn->IsConnected())
		{
			conn->Send(out_bytes.data(), (int)out_bytes.size());
			LOG_INFO("Sent register request to registry, node_addr: %s", m_node_addr.c_str());
		}
		else
		{
			LOG_ERROR("Registry not connected, register request not sent");
		}
	}
}
