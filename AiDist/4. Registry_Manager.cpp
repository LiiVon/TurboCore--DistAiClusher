#include "registry_manager.h"
#include "logger.h"
#include "global.h"

namespace AiSchedule
{
	using namespace TcFrame;

	std::string RegistryManager::RegisterWorker(const RegistryNodeInfo& info)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		// 用地址+时间戳生成唯一id，保证全局唯一
		std::string node_id = info.node_addr + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

		m_nodes[node_id] = info;
		m_nodes[node_id].node_id = node_id; // 回写生成的node_id，保证信息一致
		m_nodes[node_id].status = NodeStatus::ONLINE; // 注册成功，标记为在线
		// 更新模型反向索引
		for (const auto& model : info.support_models)
		{
			m_model_to_nodes[model].push_back(node_id);
		}
		LOG_INFO("New worker registered, node_id: %s, addr: %s", node_id.c_str(), info.node_addr.c_str());
		return node_id;
	}

	void RegistryManager::UpdateHeartbeat(const std::string& node_id, int32_t available_memory_mb, const std::vector<std::string>& online_models)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_nodes.count(node_id)) {
			auto& node = m_nodes[node_id];
			node.last_heartbeat = std::chrono::steady_clock::now();
			node.available_memory_mb = available_memory_mb;
			node.support_models = online_models;
			node.status = NodeStatus::ONLINE; // 收到心跳，恢复为在线状态
			LOG_DEBUG("Worker heartbeat updated, node_id: %s, available_mem: %dMB", node_id.c_str(), available_memory_mb);
		}
		else {
			LOG_WARN("Update heartbeat for unknown node: %s", node_id.c_str());
		}
	}

	void RegistryManager::RemoveWorker(const std::string& node_id)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_nodes.count(node_id))
		{
			LOG_WARN("Try to remove unknown node: %s, ignore", node_id.c_str());
			return;
		}

		std::string addr = m_nodes[node_id].node_addr;
		// 从模型反向索引中移除
		for (const auto& model : m_nodes[node_id].support_models)
		{
			auto& vec = m_model_to_nodes[model];
			vec.erase(std::remove(vec.begin(), vec.end(), node_id), vec.end());
		}

		m_nodes.erase(node_id);
		LOG_INFO("Worker removed, node_id: %s, addr: %s", node_id.c_str(), addr.c_str());
	}

	std::vector<RegistryNodeInfo> RegistryManager::GetOnlineNodesByModel(const std::string& model_name)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<RegistryNodeInfo> result;
		if (!m_model_to_nodes.count(model_name))
		{
			return result;
		}

		for (const auto& node_id : m_model_to_nodes[model_name])
		{
			if (m_nodes.count(node_id))
			{
				auto& node = m_nodes[node_id];
				// 只保留心跳30秒内、状态为ONLINE的节点
				auto duration = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now() - node.last_heartbeat
				).count();
				if (duration < 30 && node.status == NodeStatus::ONLINE) {
					result.push_back(node);
				}
			}
		}

		// 按可用显存降序排序，优先选空闲显存大的节点，符合负载均衡预期
		std::sort(result.begin(), result.end(), [](const RegistryNodeInfo& a, const RegistryNodeInfo& b) {
			return a.available_memory_mb > b.available_memory_mb;
			});

		return result;
	}

	std::vector<std::string> RegistryManager::ScanTimeoutNodes() {
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<std::string> removed_nodes; // 保存本次被移除的节点ID
		auto now = std::chrono::steady_clock::now();
		for (auto it = m_nodes.begin(); it != m_nodes.end(); ) {
			auto diff_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_heartbeat).count();
			if (diff_seconds > 30) { // 超过30秒没心跳，踢掉
				removed_nodes.push_back(it->first); // 把节点ID存起来
				it = m_nodes.erase(it);
			}
			else {
				++it;
			}
		}
		return removed_nodes; // 返回被移除的列表，给RegistryServer广播用
	}

	bool RegistryManager::HasNode(const std::string& node_id) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_nodes.count(node_id) > 0;
	}
}
