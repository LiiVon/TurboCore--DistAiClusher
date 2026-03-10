#pragma once

#include "global.h"

#include "eventloop.h"

#include "tcpclient.h"

#include "protocol.h"

#include <string>

#include <vector>

#include <mutex>

#include <unordered_map>

#include <functional>

namespace AiSchedule {
	using namespace TcFrame;

	// 后端服务配置
	struct BackendConfig
	{
		std::string service_name; // 服务名：registry/scheduler
		std::string ip;
		int port;
		// 连接池：每个服务维护一个长连接，不用每次建连
		std::shared_ptr<TcpClient> client;
		bool connected;
	};

	// HTTP请求结构体（适配cpp-httplib，或者你自己的HTTP解析）
	struct GatewayHttpRequest {
		std::string path;
		std::string method;
		nlohmann::json body;
	};

	// 网关核心类
	class Gateway {
	public:
		Gateway(EventLoop* loop, int listen_port,
			std::string registry_ip, int registry_port,
			std::string scheduler_ip, int scheduler_port);
		~Gateway();

		// 启动网关
		void Start();
		// 停止网关
		void Stop();

	private:
		// 初始化后端连接
		void InitBackendConnections();
		// 处理HTTP请求：所有用户请求入口
		void HandleHttpRequest(const GatewayHttpRequest& req,
			std::function<void(int, std::string)> resp_callback);

		// 转发请求给后端服务，拿到结果回调返回给用户
		void ForwardToBackend(BackendConfig& backend, AiSchedulePacket& packet,
			std::function<void(int, nlohmann::json)> result_callback);
		// 后端响应回调，处理结果返回给用户
		void OnBackendResponse(AiSchedulePacket& packet,
			std::function<void(int, nlohmann::json)> result_callback);

		// 各个接口路由处理
		void HandleSubmitTask(const GatewayHttpRequest& req,
			std::function<void(int, std::string)> resp_callback);

		void HandleQueryTask(const GatewayHttpRequest& req,
			std::function<void(int, std::string)> resp_callback);

		void HandleListNodes(const GatewayHttpRequest& req,
			std::function<void(int, std::string)> resp_callback);

		void HandleListJobs(const GatewayHttpRequest& req,
			std::function<void(int, std::string)> resp_callback);

	private:
		EventLoop* m_loop;
		int m_listen_port;               // 网关监听端口
		std::vector<BackendConfig> m_backends; // 后端服务配置
		std::mutex m_mtx;
		bool m_running = false;

		// 等待后端响应的请求表：seq -> 回调，处理异步响应
		std::unordered_map<uint64_t, std::function<void(nlohmann::json)>> m_pending_reqs;
		uint64_t m_next_seq = 1; // 自增序列号
	};
}
