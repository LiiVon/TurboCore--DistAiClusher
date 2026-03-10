#pragma once

#include "global.h"
#include "registry_nodeinfo.h"
#include "json.hpp"

//负责存任务（推理任务）的基本信息，先做内存版，后续再加数据库持久化就行：
namespace AiSchedule
{
	// 任务定义状态：这个任务定义是否启用，会不会被调度，和「任务执行状态」分开
	enum class JobDefineStatus
	{
		Disabled = 0, // 禁用，不会被调度
		Enabled = 1,  // 启用，满足调度条件会被调度
	};

	enum class JobStatus
	{
		PENDING,        // 刚提交，排队等待查询节点
		SCHEDULING,     // 正在查询节点、切片分发
		RUNNING,        // 切片已经分发，正在执行
		FINISHED,       // 全部切片完成，任务结束成功
		FAILED          // 任务执行失败（超过重试次数）
	};

	struct JobInfo
	{
		// 默认构造，初始化合理默认值
		JobInfo()
			: status(JobDefineStatus::Enabled)
			, timeout_seconds(300)
			, max_retry_times(2)
			, create_time(0)
			, update_time(0)
			, expect_slice_count(0)
			, run_status(JobStatus::PENDING)
			, finish_time(0)
		{
		}

		std::string job_id;                 // 任务ID，全局唯一标识
		std::string job_name;               // 任务名称，方便界面展示
		std::string model_name;             // 推理目标模型名称，用来选支持该模型的Worker
		std::string cron_expr;              // cron表达式，定时调度用，空表示一次性任务
		std::string handler_name;           // worker端处理函数名称（推理就是固定inference，留着扩展）
		std::string job_params;             // 任务自定义参数，JSON字符串格式，灵活扩展
		std::string input_file_url;         // 推理输入文件地址（本地/OSS）
		std::string output_result_url;      // 推理结果存储地址

		JobDefineStatus status = JobDefineStatus::Enabled; // 任务定义是否启用
		int32_t timeout_seconds = 300;     // 任务超时时间，单位秒，默认5分钟
		int32_t max_retry_times = 2;        // 任务失败最大重试次数，默认重试2次
		int32_t expect_slice_count = 0;     // 期望切片数，0表示自动根据输入/节点数切片

		int64_t create_time;                // 任务创建时间戳（秒）
		int64_t update_time;                // 任务最后更新时间戳（秒）
		int64_t finish_time;                // 任务完成时间戳（秒），方便统计耗时
		JobStatus run_status;               // 当前任务执行状态
		std::vector<RegistryNodeInfo> available_nodes;
		std::vector<TaskSlice> slices;
	};

	// 任务执行结果：支持单个切片结果，也支持整个任务总结果
	struct JobResult
	{
		// 默认构造，初始化默认值
		JobResult()
			: success(false)
			, exit_code(-1)
			, slice_id(-1)
			, retry_count(0)
			, cost_ms(0)
			, execute_time(0)
		{
		}

		std::string result_id;       // 结果唯一ID：切片结果 = 任务ID_切片ID，总结果 = 任务ID
		std::string job_id;          // 所属总任务ID
		int32_t slice_id;            // 切片ID：-1表示整个任务的总结果，适配两种场景
		std::string node_id;         // 执行该结果的Worker节点ID
		bool success;                // 是否执行成功
		std::string output;          // 执行结果输出，JSON格式：切片结果存当前切片输出，总结果存汇总地址
		int32_t exit_code;           // 退出码：0成功，非零对应错误类型，方便排查
		int32_t retry_count;         // 当前重试次数，超过最大重试直接标记失败
		int64_t cost_ms;             // 执行耗时，单位毫秒，性能统计用
		int64_t execute_time;        // 执行开始时间戳，方便排查
	};

	// 单个切片的执行状态
	enum class SliceStatus
	{
		PENDING,    // 等待分配
		RUNNING,    // 正在执行
		FINISHED,   // 执行完成
		FAILED      // 执行失败
	};

	// 单个任务切片：大任务切分后的最小执行单位，一个切片分配给一个Worker
	struct TaskSlice
	{
		std::string job_id;          // 所属总任务ID
		int32_t slice_id;            // 切片ID，从0开始，结果汇总排序用
		std::string input_range;     // 切片对应输入范围：比如"0-1000"行，方便对应输入文件分片
		std::vector<std::string> inputs; // 切片内所有输入项，直接给Worker推理
		std::string assigned_node_id; // 分配到的Worker节点ID
		SliceStatus status;          // 当前切片执行状态
		std::string result_url;      // 切片结果存储地址
		std::string error_message;   // 失败时的错误信息
		int32_t retry_count;         // 当前重试次数

		//  配套to_json/from_json，
		nlohmann::json to_json() const
		{
			return
			{
				{"job_id", job_id},
				{"slice_id", slice_id},
				{"input_range", input_range},
				{"inputs", inputs},
				{"assigned_node_id", assigned_node_id},
				{"status", (int)status},
				{"result_url", result_url},
				{"error_message", error_message},
				{"retry_count", retry_count}
			};
		}

		static TaskSlice from_json(const nlohmann::json& j) {
			TaskSlice slice;
			slice.job_id = j.at("job_id").get<std::string>();
			slice.slice_id = j.at("slice_id").get<int32_t>();
			slice.input_range = j.at("input_range").get<std::string>();
			slice.inputs = j.at("inputs").get<std::vector<std::string>>();
			slice.assigned_node_id = j.at("assigned_node_id").get<std::string>();
			slice.status = static_cast<SliceStatus>(j.at("status").get<int>());
			slice.result_url = j.at("result_url").get<std::string>();
			slice.error_message = j.at("error_message").get<std::string>();
			slice.retry_count = j.at("retry_count").get<int32_t>();
			return slice;
		}
	};

	inline std::string JobStatusToStr(JobStatus status)
	{
		switch (status) {
		case JobStatus::PENDING: return "PENDING";
		case JobStatus::SCHEDULING: return "SCHEDULING";
		case JobStatus::RUNNING: return "RUNNING";
		case JobStatus::FINISHED: return "FINISHED";
		case JobStatus::FAILED: return "FAILED";
		default: return "UNKNOWN";
		}
	}
}
