#pragma once

#include "registry_nodeinfo.h"
#include "global.h"

// 核心管理类:注册中心管理器，负责维护Worker节点信息、处理注册/心跳/查询请求等核心逻辑
namespace AiSchedule
{
	class RegistryManager
	{
	public:
		RegistryManager() = default;

		// 注册新Worker，返回分配的唯一node_id
		std::string RegisterWorker(const RegistryNodeInfo& info);

		// 更新心跳，刷新最后心跳时间和可用内存
		void UpdateHeartbeat(const std::string& node_id, int32_t available_memory_mb, const std::vector<std::string>& online_models);

		// 移除下线Worker（超时手动调这个）
		void RemoveWorker(const std::string& node_id);

		// 根据模型名，获取所有在线支持这个模型的节点信息（按可用内存降序，优先给调度选空闲的）
		std::vector<RegistryNodeInfo> GetOnlineNodesByModel(const std::string& model_name);

		// 定时扫描：每隔10秒扫一次，移除超过30秒没心跳的节点，放到你的base定时器里调用
		std::vector<std::string> ScanTimeoutNodes();

		bool HasNode(const std::string& node_id) const;

	private:
		mutable std::mutex m_mutex;
		std::unordered_map<std::string, RegistryNodeInfo> m_nodes; // node_id -> 节点信息
		std::unordered_map<std::string, std::vector<std::string>> m_model_to_nodes; // model_name -> node_id列表
	};
}
