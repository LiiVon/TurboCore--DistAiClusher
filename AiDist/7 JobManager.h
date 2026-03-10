#pragma once

#include "jobinfo.h"

#include "global.h"

#include <mutex>

#include <unordered_map>

#include <vector>

#include <string>

#include <utility>

namespace AiSchedule
{
	class JobManager
	{
	public:
		// 添加/更新任务定义
		void AddOrUpdateJob(const JobInfo& job_info);
		// 移除任务（同时移除所有结果和进度）
		void RemoveJob(const std::string& job_id);
		// 根据ID获取任务定义
		JobInfo GetJob(const std::string& job_id);
		// 获取所有启用的任务（给定时调度用）
		std::vector<JobInfo> GetAllEnableJobs();

		// 保存任务/切片执行结果
		void SaveJobResult(const JobResult& job_result);
		// 获取任务最近N条执行结果（max_count<=0返回全部）
		std::vector<JobResult> GetJobResults(const std::string& job_id, int max_count = 100);

		// 更新任务执行进度（给调度中心用，切片完成后更新）
		void UpdateTaskProgress(const std::string& job_id, int32_t finished_slices, int32_t total_slices);
		// 获取任务当前进度，返回 (finished_slices, total_slices)
		std::pair<int32_t, int32_t> GetTaskProgress(const std::string& job_id);

		// 获取指定节点上所有未完成的切片（节点下线故障重调度用）
		std::vector<std::pair<std::string, int32_t>> GetUnfinishedSlicesForNode(const std::string& node_id);

	private:
		std::mutex m_mtx;
		std::unordered_map<std::string, JobInfo> m_jobs; 					// job_id -> 任务定义
		std::unordered_map<std::string, std::vector<JobResult>> m_job_results; 	// job_id -> 任务执行结果列表
		std::unordered_map<std::string, std::pair<int32_t, int32_t>> m_job_progress; // job_id -> (完成切片数, 总切片数) 任务进度
	};
}
