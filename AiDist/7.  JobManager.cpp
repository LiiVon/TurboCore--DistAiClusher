#include "jobinfo.h"

#include "jobmanager.h"

#include "logger.h"

namespace AiSchedule
{
	using namespace TcFrame;

	void JobManager::AddOrUpdateJob(const JobInfo& job_info)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		bool is_update = m_jobs.count(job_info.job_id) > 0;

		m_jobs[job_info.job_id] = job_info;
		// 新增/更新都打对应日志，方便排查
		if (is_update)
		{
			LOG_INFO("Job updated: job_id=[%s], name=[%s]",
				job_info.job_id.c_str(), job_info.job_name.c_str());
		}
		else
		{
			LOG_INFO("Job added: job_id=[%s], name=[%s]",
				job_info.job_id.c_str(), job_info.job_name.c_str());
			// 新任务默认进度0
			m_job_progress[job_info.job_id] = { 0, 0 };
		}
	}

	void JobManager::RemoveJob(const std::string& job_id)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		bool existed = m_jobs.count(job_id) > 0;
		// 先删任务定义，再删结果，再删进度
		m_jobs.erase(job_id);
		m_job_results.erase(job_id);
		m_job_progress.erase(job_id);

		if (existed) {
			LOG_INFO("Job removed completely: job_id=[%s]", job_id.c_str());
		}
		else {
			LOG_WARN("Try to remove non-exist job: job_id=[%s]", job_id.c_str());
		}
	}

	JobInfo JobManager::GetJob(const std::string& job_id)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (m_jobs.count(job_id) > 0)
		{
			LOG_DEBUG("Job found: job_id=[%s]", job_id.c_str());
			return m_jobs[job_id];
		}
		// 找不到返回空JobInfo，不抛异常，更健壮
		LOG_WARN("Job not found: job_id=[%s]", job_id.c_str());
		return JobInfo{};
	}

	std::vector<JobInfo> JobManager::GetAllEnableJobs()
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		std::vector<JobInfo> result;
		result.reserve(m_jobs.size());

		// 只返回启用的任务，对齐改后的枚举名
		for (auto& pair : m_jobs)
		{
			if (pair.second.status == JobDefineStatus::Enabled)
			{
				result.push_back(pair.second);
			}
		}
		return result;
	}

	void JobManager::SaveJobResult(const JobResult& job_result)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		auto& results = m_job_results[job_result.job_id];
		results.push_back(job_result);

		// 控制单任务最大结果数，避免内存无限增长，只保留最新1000条
		constexpr size_t max_results_per_job = 1000;
		if (results.size() > max_results_per_job) {
			results.erase(results.begin(), results.begin() + (results.size() - max_results_per_job));
		}

		LOG_INFO("Job result added: job_id=[%s], node_id=[%s], slice_id=[%d], success=[%d]",
			job_result.job_id.c_str(), job_result.node_id.c_str(), job_result.slice_id, job_result.success);
	}

	std::vector<JobResult> JobManager::GetJobResults(const std::string& job_id, int max_count)
	{
		std::lock_guard<std::mutex> lock(m_mtx);

		// 边界：找不到这个job的任何结果，直接返回空
		if (m_job_results.find(job_id) == m_job_results.end())
		{
			LOG_DEBUG("No job results found: job_id=[%s]", job_id.c_str());
			return {};
		}

		auto& all_results = m_job_results.at(job_id);
		// 边界：结果总数<=max_count 或 max_count<=0，直接返回全部
		if (all_results.size() <= (size_t)max_count || max_count <= 0)
		{
			return all_results;
		}

		// 只返回最新的max_count条（追加在尾部，所以取最后max_count个）
		return std::vector<JobResult>(
			all_results.end() - max_count,
			all_results.end()
		);
	}

	void JobManager::UpdateTaskProgress(const std::string& job_id, int32_t finished_slices, int32_t total_slices)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (!m_jobs.count(job_id)) {
			LOG_WARN("Update progress for non-exist job: job_id=[%s]", job_id.c_str());
			return;
		}
		m_job_progress[job_id] = { finished_slices, total_slices };
		LOG_DEBUG("Job progress updated: job_id=[%s], finished=[%d/%d]",
			job_id.c_str(), finished_slices, total_slices);
	}

	std::pair<int32_t, int32_t> JobManager::GetTaskProgress(const std::string& job_id)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (m_job_progress.count(job_id)) {
			return m_job_progress[job_id];
		}
		// 不存在返回0进度
		return { 0, 0 };
	}

	std::vector<std::pair<std::string, int32_t>> JobManager::GetUnfinishedSlicesForNode(const std::string& node_id)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		std::vector<std::pair<std::string, int32_t>> result;
		// 遍历所有结果，找该节点上未完成的切片
		for (auto& [job_id, results] : m_job_results) {
			for (auto& result_item : results) {
				if (result_item.node_id == node_id && !result_item.success) {
					result.emplace_back(job_id, result_item.slice_id);
				}
			}
		}
		LOG_DEBUG("Get unfinished slices for node: %s, total: %zu", node_id.c_str(), result.size());
		return result;
	}
}
