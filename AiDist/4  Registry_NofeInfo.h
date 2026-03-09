#pragma once

#include "global.h"

namespace AiSchedule
{
	// 节点状态枚举，清晰标记节点当前状态，方便负载均衡过滤
	enum class NodeStatus
	{
		OFFLINE,     // 离线，不分配任务
		ONLINE,      // 在线正常，可分配任务
		UNHEALTHY    // 心跳延迟，不健康，暂时不分配
	};

	// 存储单个Worker节点信息
	struct RegistryNodeInfo
	{
		// 默认构造，初始化所有字段，避免野值
		RegistryNodeInfo()
			: total_memory_mb(0)
			, available_memory_mb(0)
			, weight(100)
			, status(NodeStatus::OFFLINE)
			, last_heartbeat(std::chrono::steady_clock::now())
		{
		}

		std::string node_id;
		std::string node_addr;
		std::vector<std::string> support_models;
		int32_t total_memory_mb;	// 节点总GPU显存（MB）
		int32_t available_memory_mb; // 节点当前可用GPU显存（MB）
		int32_t weight;				// 负载均衡权重
		NodeStatus status;			// 节点当前状态
		std::chrono::steady_clock::time_point last_heartbeat; // 最后心跳时间，超时判断用
	};
}
