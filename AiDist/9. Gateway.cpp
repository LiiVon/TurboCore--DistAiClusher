#include "gateway.h"
#include "logger.h"
#include "json.hpp"
#include "protocol.h"
#include "packet_parser.h"
// 用cpp-httplib，轻量HTTP服务，只需要引入头文件，编译时带上就行
#include "httplib.h"

namespace AiSchedule
{
	Gateway::Gateway(EventLoop* loop, int listen_port,
		std::string registry_ip, int registry_port,
		std::string scheduler_ip, int scheduler_port)
		: m_loop(loop), m_listen_port(listen_port)
	{
		// 初始化后端配置
		m_backends.push_back({
			"registry", registry_ip, registry_port, nullptr, false
			});
		m_backends.push_back({
			"scheduler", scheduler_ip, scheduler_port, nullptr, false
			});
		LOG_INFO("Gateway construct success, listen port: %d", m_listen_port);
		LOG_INFO("Backend registry: %s:%d", registry_ip.c_str(), registry_port);
		LOG_INFO("Backend scheduler: %s:%d", scheduler_ip.c_str(), scheduler_port);
	}

	Gateway::~Gateway() {
		LOG_INFO("Gateway destructing...");
		Stop();
	}

	void Gateway::InitBackendConnections()
	{
		for (auto& backend : m_backends)
		{
			// 每个后端创建一个TCP长连接，复用
			Address server_addr(backend.ip, backend.port);
			backend.client = std::make_shared<TcpClient>(m_loop, server_addr, backend.service_name);
			backend.client->SetMessageCallback(
				[this, &backend](const TcpConnectionPtr& conn, Buffer* buf) {
					// 粘包解析，复用你现有的PacketParser
					static PacketParser parser;
					std::vector<AiSchedulePacket> packets;
					parser.Feed(buf->Peek(), buf->ReadableBytes());
					buf->Retrieve(buf->ReadableBytes());
					size_t parsed = parser.ParseAll(packets);
					for (auto& packet : packets) {
						OnBackendResponse(packet, [](auto...) {});
					}
				}
			);
			backend.client->Connect();
			backend.connected = true;
			LOG_INFO("Backend connected: %s %s:%d",
				backend.service_name.c_str(), backend.ip.c_str(), backend.port);
		}
	}

	void Gateway::Start() {
		InitBackendConnections();
		m_running = true;

		// 启动HTTP服务，开一个单独线程跑httplib的监听（httplib是同步的，不影响你EventLoop）
		std::thread http_thread([this]() {
			httplib::Server svr;

			// 注册接口：对齐你业务，就是这几个常用的
			svr.Post("/api/v1/submit_task", [this](const httplib::Request& req, httplib::Response& res) {
				// 解析请求
				GatewayHttpRequest g_req;
				g_req.path = req.path;
				g_req.method = req.method;
				try {
					g_req.body = nlohmann::json::parse(req.body);
					HandleHttpRequest(g_req, [&res](int code, std::string body) {
						res.status = code;
						res.set_content(body, "application/json");
						});
				}
				catch (const std::exception& e) {
					res.status = 400;
					nlohmann::json err;
					err["code"] = 400;
					err["message"] = std::string("parse request failed: ") + e.what();
					res.set_content(err.dump(), "application/json");
				}
				});

			svr.Get("/api/v1/query_task", [this](const httplib::Request& req, httplib::Response& res) {
				GatewayHttpRequest g_req;
				g_req.path = req.path;
				g_req.method = req.method;
				// 从query参数拿task_id
				std::string task_id = req.get_param_value("task_id");
				g_req.body["task_id"] = task_id;
				HandleHttpRequest(g_req, [&res](int code, std::string body) {
					res.status = code;
					res.set_content(body, "application/json");
					});
				});

			svr.Get("/api/v1/list_nodes", [this](const httplib::Request& req, httplib::Response& res) {
				GatewayHttpRequest g_req;
				g_req.path = req.path;
				g_req.method = req.method;
				std::string model_name = req.get_param_value("model_name");
				g_req.body["model_name"] = model_name;
				HandleHttpRequest(g_req, [&res](int code, std::string body) {
					res.status = code;
					res.set_content(body, "application/json");
					});
				});

			svr.Get("/api/v1/list_jobs", [this](const httplib::Request& req, httplib::Response& res) {
				GatewayHttpRequest g_req;
				g_req.path = req.path;
				g_req.method = req.method;
				HandleHttpRequest(g_req, [&res](int code, std::string body) {
					res.status = code;
					res.set_content(body, "application/json");
					});
				});

			// 启动监听
			LOG_INFO("Gateway HTTP server started on 0.0.0.0:%d", m_listen_port);
			svr.listen("0.0.0.0", m_listen_port);
			});
		// 线程分离，不用join，主Loop退出就结束
		http_thread.detach();
	}

	void Gateway::Stop() {
		if (!m_running) return;
		m_running = false;
		for (auto& backend : m_backends) {
			if (backend.client && backend.connected) {
				backend.client->Disconnect();
			}
		}
		LOG_INFO("Gateway stopped");
	}

	void Gateway::HandleHttpRequest(const GatewayHttpRequest& req,
		std::function<void(int, std::string)> resp_callback)
	{
		LOG_INFO("Gateway receive request: %s %s", req.method.c_str(), req.path.c_str());

		// 根据路径路由到不同处理函数
		if (req.path == "/api/v1/submit_task") {
			HandleSubmitTask(req, resp_callback);
		}
		else if (req.path == "/api/v1/query_task") {
			HandleQueryTask(req, resp_callback);
		}
		else if (req.path == "/api/v1/list_nodes") {
			HandleListNodes(req, resp_callback);
		}
		else if (req.path == "/api/v1/list_jobs") {
			HandleListJobs(req, resp_callback);
		}
		else {
			// 404
			nlohmann::json err;
			err["code"] = 404;
			err["message"] = "api not found";
			resp_callback(404, err.dump());
		}
	}

	// 转发请求给后端核心逻辑
	void Gateway::ForwardToBackend(BackendConfig& backend, AiSchedulePacket& packet,
		std::function<void(int, nlohmann::json)> result_callback)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (!backend.connected || !backend.client->IsConnected()) {
			LOG_ERROR("Backend %s not connected, forward failed", backend.service_name.c_str());
			nlohmann::json err;
			err["code"] = 503;
			err["message"] = std::string("backend ") + backend.service_name + " not available";
			result_callback(503, err);
			return;
		}

		// 存pending回调，后端响应回来触发
		uint64_t seq = m_next_seq++;
		packet.base_header.seq = seq;
		m_pending_reqs[seq] = [result_callback](nlohmann::json body) {
			result_callback(200, body);
			};

		// 序列化发送给后端
		std::vector<char> out_bytes;
		packet.serialize(out_bytes);
		backend.client->Send(out_bytes.data(), out_bytes.size());
		LOG_DEBUG("Forward request to backend %s, seq: %lu", backend.service_name.c_str(), seq);
	}

	// 后端响应回来，处理回调
	void Gateway::OnBackendResponse(AiSchedulePacket& packet,
		std::function<void(int, nlohmann::json)> result_callback)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		uint64_t seq = packet.base_header.seq;
		if (m_pending_reqs.count(seq)) {
			auto callback = m_pending_reqs[seq];
			callback(packet.body);
			m_pending_reqs.erase(seq);
			LOG_DEBUG("Backend response received, seq: %lu", seq);
		}
		else {
			LOG_WARN("Backend response for unknown seq: %lu", seq);
		}
	}

	// 处理提交任务：转发给调度中心
	void Gateway::HandleSubmitTask(const GatewayHttpRequest& req,
		std::function<void(int, std::string)> resp_callback)
	{
		// 从HTTP body解析提交请求，转成你内部协议，转发给调度中心
		SubmitTaskReq submit_req;
		try {
			submit_req.model_name = req.body.at("model_name").get<std::string>();
			submit_req.input_file_url = req.body.at("input_file_url").get<std::string>();
			if (req.body.contains("slice_size")) {
				submit_req.slice_size = req.body.at("slice_size").get<int32_t>();
			}
			else {
				submit_req.slice_size = 0; // 默认自动切片
			}
		}
		catch (const std::exception& e) {
			nlohmann::json err;
			err["code"] = 400;
			err["message"] = std::string("invalid request: ") + e.what();
			resp_callback(400, err.dump());
			return;
		}

		// 找scheduler后端
		auto it = std::find_if(m_backends.begin(), m_backends.end(),
			[](const BackendConfig& b) { return b.service_name == "scheduler"; });
		if (it == m_backends.end()) {
			nlohmann::json err;
			err["code"] = 500;
			err["message"] = "scheduler backend not configured";
			resp_callback(500, err.dump());
			return;
		}

		// 构造请求转发
		AiSchedulePacket packet = AiSchedulePacket::build(
			0, PacketType::SUBMIT_TASK, "gateway", submit_req
		);
		ForwardToBackend(*it, packet, [resp_callback](int code, nlohmann::json body) {
			resp_callback(code, body.dump());
			});
	}

	// 处理查询任务进度：转发给调度中心
	void Gateway::HandleQueryTask(const GatewayHttpRequest& req,
		std::function<void(int, std::string)> resp_callback)
	{
		TaskQueryReq query_req;
		query_req.task_id = req.body.at("task_id").get<std::string>();

		auto it = std::find_if(m_backends.begin(), m_backends.end(),
			[](const BackendConfig& b) { return b.service_name == "scheduler"; });
		if (it == m_backends.end()) {
			nlohmann::json err;
			err["code"] = 500;
			err["message"] = "scheduler backend not configured";
			resp_callback(500, err.dump());
			return;
		}

		AiSchedulePacket packet = AiSchedulePacket::build(
			0, PacketType::TASK_QUERY, "gateway", query_req
		);
		ForwardToBackend(*it, packet, [resp_callback](int code, nlohmann::json body) {
			resp_callback(code, body.dump());
			});
	}

	// 处理查询在线节点：转发给注册中心
	void Gateway::HandleListNodes(const GatewayHttpRequest& req,
		std::function<void(int, std::string)> resp_callback)
	{
		NodeQueryReq query_req;
		query_req.model_name = req.body.at("model_name").get<std::string>();

		auto it = std::find_if(m_backends.begin(), m_backends.end(),
			[](const BackendConfig& b) { return b.service_name == "registry"; });
		if (it == m_backends.end()) {
			nlohmann::json err;
			err["code"] = 500;
			err["message"] = "registry backend not configured";
			resp_callback(500, err.dump());
			return;
		}

		AiSchedulePacket packet = AiSchedulePacket::build(
			0, PacketType::NODE_QUERY, "gateway", query_req
		);
		ForwardToBackend(*it, packet, [resp_callback](int code, nlohmann::json body) {
			resp_callback(code, body.dump());
			});
	}

	// 处理获取所有任务：转发给调度中心
	void Gateway::HandleListJobs(const GatewayHttpRequest& req,
		std::function<void(int, std::string)> resp_callback)
	{
		auto it = std::find_if(m_backends.begin(), m_backends.end(),
			[](const BackendConfig& b) { return b.service_name == "scheduler"; });
		if (it == m_backends.end()) {
			nlohmann::json err;
			err["code"] = 500;
			err["message"] = "scheduler backend not configured";
			resp_callback(500, err.dump());
			return;
		}

		// 调度中心返回所有任务列表，网关直接转发
		nlohmann::json req_body;
		AiSchedulePacket packet;
		packet.base_header.seq = 0;
		packet.base_header.type = PacketType::JOB_LIST_REQ; // 你调度中心加个这个类型就行
		packet.base_header.sender_id = "gateway";
		packet.body = req_body;
		ForwardToBackend(*it, packet, [resp_callback](int code, nlohmann::json body) {
			resp_callback(code, body.dump());
			});
	}
} // namespace AiSchedule
