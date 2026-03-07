#pragma once

#include "nlohmann/json.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace AiSchedule {
	// ==================== 基础常量定义 ====================
	// 传输层魔数：快速识别合法包，"ASCH" = Ai SCHedule，固定4字节
	constexpr uint32_t AI_SCHEDULE_MAGIC = 0x41534348;
	// 协议版本：后续升级协议兼容旧版本
	constexpr uint16_t AI_SCHEDULE_PROTOCOL_VERSION = 1;

	// ==================== 工具函数：CRC32数据校验（动态生成表，永远不会报初始值太多！） ====================
	inline uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
		// 运行时动态生成CRC表，只生成一次，不需要手动写256个初始值，彻底解决元素个数问题
		static bool table_inited = false;
		static uint32_t table[256];
		if (!table_inited)
		{
			for (int i = 0; i < 256; i++)
			{
				uint32_t c = static_cast<uint32_t>(i);
				for (int j = 0; j < 8; j++)
				{
					if (c & 1)
					{
						c = 0xedb88320 ^ (c >> 1);
					}
					else
					{
						c = c >> 1;
					}
				}
				table[i] = c;
			}
			table_inited = true;
		}

		crc = crc ^ 0xFFFFFFFF;
		while (len--) {
			crc = table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
		}
		return crc ^ 0xFFFFFFFF;
	}

	// ==================== 传输层协议头：解决粘包/校验/版本兼容 ====================
	// 固定16字节（之前算错了，加上checksum一共16字节，不是12字节），每个包最前面必须带这个头，工业级TCP标准设计
	struct TransportHeader
	{
		uint32_t magic;         // 魔数，快速过滤非法脏数据
		uint16_t version;       // 协议版本，兼容后续升级
		uint16_t reserve;       // 保留扩展位，当前填0
		uint32_t body_len;      // 后续业务body的总长度，解决粘包拆包
		uint32_t checksum;      // body的CRC32校验，验证数据完整性

		// 序列化为大端字节流（跨CPU架构兼容）
		void serialize_to_bytes(std::vector<char>& out) const
		{
			write_uint32(magic, out);
			write_uint16(version, out);
			write_uint16(reserve, out);
			write_uint32(body_len, out);
			write_uint32(checksum, out);
		}

		// 从字节流解析传输头，失败返回false表示非法包
		static bool deserialize_from_bytes(const char* buf, size_t len, TransportHeader& out)
		{
			// 先判断长度够不够，固定16字节传输头
			if (len < sizeof(TransportHeader)) return false;
			out.magic = read_uint32(buf);
			out.version = read_uint16(buf + 4);
			out.reserve = read_uint16(buf + 6);
			out.body_len = read_uint32(buf + 8);
			out.checksum = read_uint32(buf + 12);

			// 第一步校验魔数，不对直接丢弃
			if (out.magic != AI_SCHEDULE_MAGIC) return false;
			// 校验版本，不兼容直接拒绝
			if (out.version > AI_SCHEDULE_PROTOCOL_VERSION) return false;
			return true;
		}

		// 获取传输头固定大小，外部粘包解析用
		static constexpr size_t header_size() {
			return 16;
		}

	private:
		// 大端写工具函数
		static void write_uint32(uint32_t val, std::vector<char>& out) {
			out.push_back(static_cast<char>((val >> 24) & 0xFF));
			out.push_back(static_cast<char>((val >> 16) & 0xFF));
			out.push_back(static_cast<char>((val >> 8) & 0xFF));
			out.push_back(static_cast<char>(val & 0xFF));
		}
		static void write_uint16(uint16_t val, std::vector<char>& out) {
			out.push_back(static_cast<char>((val >> 8) & 0xFF));
			out.push_back(static_cast<char>(val & 0xFF));
		}
		// 大端读工具函数
		static uint32_t read_uint32(const char* buf) {
			return ((uint32_t)(uint8_t)buf[0] << 24)
				| ((uint32_t)(uint8_t)buf[1] << 16)
				| ((uint32_t)(uint8_t)buf[2] << 8)
				| (uint32_t)(uint8_t)buf[3];
		}
		static uint16_t read_uint16(const char* buf) {
			return ((uint16_t)(uint8_t)buf[0] << 8)
				| (uint16_t)(uint8_t)buf[1];
		}
	};

	// ==================== 公共包头：所有业务包都带的公共信息 ====================
	// 包类型：区分不同业务请求
	enum class PacketType {
		REGISTER_WORKER,   // Worker向注册中心发起注册
		REGISTER_RESP,     // 注册中心返回注册结果
		HEARTBEAT,         // Worker心跳上报
		SUBMIT_TASK,       // 用户向调度中心提交任务
		TASK_DISPATCH,     // 调度中心分发切片给Worker
		TASK_RESULT_REPORT,// Worker上报切片结果
		TASK_QUERY,        // 用户查询任务进度
		WORKER_REMOVE     // Worker主动请求下线
	};

	struct BasePacket {
		uint64_t seq;          // 序列号，用于幂等、去重、请求响应匹配
		PacketType type;       // 包类型，用于快速分流处理
		std::string sender_id; // 发送方节点ID，路由用

		// JSON序列化
		nlohmann::json to_json() const {
			return {
				{"seq", seq},
				{"type", (int)type},
				{"sender_id", sender_id}
			};
		}
		// JSON反序列化，带异常抛出，外部catch处理
		static BasePacket from_json(const nlohmann::json& j) {
			BasePacket p;
			p.seq = j.at("seq").get<uint64_t>();
			p.type = static_cast<PacketType>(j.at("type").get<int>());
			p.sender_id = j.at("sender_id").get<std::string>();
			return p;
		}
	};

	// ==================== 各个业务包定义 ====================
	// 1. Worker注册请求：Worker → 注册中心
	struct WorkerRegisterReq {
		std::string node_addr;         // 节点对外服务地址 IP:port
		std::vector<std::string> support_models; // 节点支持加载的模型列表
		int32_t total_memory_mb;       // 节点总GPU显存（单位MB）
		int32_t weight;                // 负载均衡权重

		nlohmann::json to_json() const {
			return {
				{"node_addr", node_addr},
				{"support_models", support_models},
				{"total_memory_mb", total_memory_mb},
				{"weight", weight}
			};
		}
		static WorkerRegisterReq from_json(const nlohmann::json& j) {
			WorkerRegisterReq req;
			req.node_addr = j.at("node_addr").get<std::string>();
			req.support_models = j.at("support_models").get<std::vector<std::string>>();
			req.total_memory_mb = j.at("total_memory_mb").get<int32_t>();
			req.weight = j.at("weight").get<int32_t>();
			return req;
		}
	};

	// 2. 注册响应：注册中心 → Worker
	struct WorkerRegisterResp {
		bool success;
		std::string node_id;           // 注册中心分配的全局唯一节点ID
		std::string message;            // 结果描述：失败原因/成功提示

		nlohmann::json to_json() const {
			return {
				{"success", success},
				{"node_id", node_id},
				{"message", message}
			};
		}
		static WorkerRegisterResp from_json(const nlohmann::json& j) {
			WorkerRegisterResp resp;
			resp.success = j.at("success").get<bool>();
			resp.node_id = j.at("node_id").get<std::string>();
			resp.message = j.at("message").get<std::string>();
			return resp;
		}
	};

	// 3. 心跳包：Worker → 注册中心
	struct Heartbeat {
		std::string node_id;
		int32_t available_memory_mb;   // 当前节点可用GPU显存
		std::vector<std::string> online_models; // 当前在线可调度模型

		nlohmann::json to_json() const {
			return {
				{"node_id", node_id},
				{"available_memory_mb", available_memory_mb},
				{"online_models", online_models}
			};
		}
		static Heartbeat from_json(const nlohmann::json& j) {
			Heartbeat hb;
			hb.node_id = j.at("node_id").get<std::string>();
			hb.available_memory_mb = j.at("available_memory_mb").get<int32_t>();
			hb.online_models = j.at("online_models").get<std::vector<std::string>>();
			return hb;
		}
	};

	// 4. 提交任务请求：用户 → 调度中心
	struct SubmitTaskReq {
		std::string model_name;         // 需要推理的目标模型
		int32_t slice_size;             // 自定义切片大小，0表示自动切片
		std::string input_file_url;     // 输入文件地址（本地/OSS）

		nlohmann::json to_json() const {
			return {
				{"model_name", model_name},
				{"slice_size", slice_size},
				{"input_file_url", input_file_url}
			};
		}
		static SubmitTaskReq from_json(const nlohmann::json& j) {
			SubmitTaskReq req;
			req.model_name = j.at("model_name").get<std::string>();
			req.slice_size = j.at("slice_size").get<int32_t>();
			req.input_file_url = j.at("input_file_url").get<std::string>();
			return req;
		}
	};

	// 5. 提交任务响应：调度中心 → 用户
	struct SubmitTaskResp {
		bool success;
		std::string task_id;            // 调度中心分配的任务ID
		std::string message;

		nlohmann::json to_json() const {
			return {
				{"success", success},
				{"task_id", task_id},
				{"message", message}
			};
		}
		static SubmitTaskResp from_json(const nlohmann::json& j) {
			SubmitTaskResp resp;
			resp.success = j.at("success").get<bool>();
			resp.task_id = j.at("task_id").get<std::string>();
			resp.message = j.at("message").get<std::string>();
			return resp;
		}
	};

	// 6. 任务分发包：调度中心 → Worker
	struct TaskDispatch {
		std::string task_id;           // 所属总任务ID
		int32_t slice_id;               // 切片ID，结果汇总排序用
		std::string model_name;         // 推理目标模型
		std::vector<std::string> inputs; // 切片内所有推理输入
		// 调度中心地址，Worker知道把结果往哪发
		std::string scheduler_ip;       // 调度中心IP，结果上报用
		int32_t scheduler_port;         // 调度中心端口，结果上报用

		nlohmann::json to_json() const {
			return {
				{"task_id", task_id},
				{"slice_id", slice_id},
				{"model_name", model_name},
				{"inputs", inputs},
				{"scheduler_ip", scheduler_ip},
				{"scheduler_port", scheduler_port}
			};
		}
		static TaskDispatch from_json(const nlohmann::json& j) {
			TaskDispatch dispatch;
			dispatch.task_id = j.at("task_id").get<std::string>();
			dispatch.slice_id = j.at("slice_id").get<int32_t>();
			dispatch.model_name = j.at("model_name").get<std::string>();
			dispatch.inputs = j.at("inputs").get<std::vector<std::string>>();

			dispatch.scheduler_ip = j.at("scheduler_ip").get<std::string>();
			dispatch.scheduler_port = j.at("scheduler_port").get<int32_t>();
			return dispatch;
		}
	};

	// 7. 切片结果上报：Worker → 调度中心
	struct TaskResultReport {
		std::string task_id;
		int32_t slice_id;
		bool success;
		std::string message;            // 改成message，对齐统一风格，原来error_msg太局限
		int32_t cost_ms;
		std::vector<std::string> outputs; // 对应每个输入的推理输出
		// 调度中心地址字段，和TaskDispatch对应，Worker已经用到了
		std::string scheduler_ip;
		int32_t scheduler_port;

		nlohmann::json to_json() const {
			return {
				{"task_id", task_id},
				{"slice_id", slice_id},
				{"success", success},
				{"message", message},
				{"cost_ms", cost_ms},
				{"outputs", outputs},
				{"scheduler_ip", scheduler_ip},
				{"scheduler_port", scheduler_port}
			};
		}
		static TaskResultReport from_json(const nlohmann::json& j) {
			TaskResultReport report;
			report.task_id = j.at("task_id").get<std::string>();
			report.slice_id = j.at("slice_id").get<int32_t>();
			report.success = j.at("success").get<bool>();
			report.message = j.at("message").get<std::string>();
			report.cost_ms = j.at("cost_ms").get<int32_t>();
			report.outputs = j.at("outputs").get<std::vector<std::string>>();
			report.scheduler_ip = j.at("scheduler_ip").get<std::string>();
			report.scheduler_port = j.at("scheduler_port").get<int32_t>();
			return report;
		}
	};

	// 8. 任务查询请求：用户 → 调度中心
	struct TaskQueryReq {
		std::string task_id;

		nlohmann::json to_json() const {
			return { {"task_id", task_id} };
		}
		static TaskQueryReq from_json(const nlohmann::json& j) {
			TaskQueryReq req;
			req.task_id = j.at("task_id").get<std::string>();
			return req;
		}
	};

	// 任务状态枚举
	enum class TaskStatus {
		QUEUING = 0,        // 排队等待调度
		DISPATCHING = 1,    // 正在分发切片
		RUNNING = 2,        // 正在执行
		FINISHED = 3,       // 全部完成
		FAILED = 4          // 执行失败
	};

	// 9. 任务查询响应：调度中心 → 用户
	struct TaskQueryResp {
		bool exists;
		TaskStatus status;
		int32_t total_slices;
		int32_t finished_slices;
		int32_t progress_percent;
		std::string result_file_url;
		std::string message;

		nlohmann::json to_json() const {
			return {
				{"exists", exists},
				{"status", (int)status},
				{"total_slices", total_slices},
				{"finished_slices", finished_slices},
				{"progress_percent", progress_percent},
				{"result_file_url", result_file_url},
				{"message", message}
			};
		}
		static TaskQueryResp from_json(const nlohmann::json& j) {
			TaskQueryResp resp;
			resp.exists = j.at("exists").get<bool>();
			resp.status = static_cast<TaskStatus>(j.at("status").get<int>());
			resp.total_slices = j.at("total_slices").get<int32_t>();
			resp.finished_slices = j.at("finished_slices").get<int32_t>();
			resp.progress_percent = j.at("progress_percent").get<int32_t>();
			resp.result_file_url = j.at("result_file_url").get<std::string>();
			resp.message = j.at("message").get<std::string>();
			return resp;
		}
	};

	struct WorkerRemoveReq
	{
		std::string node_id;  // 要下线的节点ID

		nlohmann::json to_json() const {
			return { {"node_id", node_id} };
		}
		static WorkerRemoveReq from_json(const nlohmann::json& j) {
			WorkerRemoveReq req;
			req.node_id = j.at("node_id").get<std::string>();
			return req;
		}
	};

	// ==================== 最外层统一包：统一序列化入口 ====================
	struct AiSchedulePacket {
		TransportHeader trans_header;
		BasePacket base_header;
		nlohmann::json body;           // 业务body，根据type对应不同业务包

		// 统一序列化：转为可发送的字节流，直接给TCP连接发送
		void serialize(std::vector<char>& out_bytes) const {
			// 1. 序列化公共头+业务body为JSON字符串
			nlohmann::json full_body = nlohmann::json::object();
			full_body["base"] = base_header.to_json();
			full_body["body"] = body;
			std::string body_str = full_body.dump();

			// 2.  计算body的CRC32校验和
			uint32_t checksum = crc32(0, reinterpret_cast<const uint8_t*>(body_str.data()), body_str.size());

			// 3. 填充传输头信息
			TransportHeader header;
			header.magic = AI_SCHEDULE_MAGIC;
			header.version = AI_SCHEDULE_PROTOCOL_VERSION;
			header.body_len = static_cast<uint32_t>(body_str.size());
			header.checksum = checksum; // 把计算好的校验和放进传输头
			header.reserve = 0;

			// 4. 先写传输头，再写body
			header.serialize_to_bytes(out_bytes);
			out_bytes.insert(out_bytes.end(), body_str.begin(), body_str.end());
		}

		// 统一反序列化：从字节流解析出完整包，失败返回false表示非法包
		static bool deserialize(const char* buf, size_t buf_len, AiSchedulePacket& out_packet) {
			// 1. 先解析传输头
			if (!TransportHeader::deserialize_from_bytes(buf, buf_len, out_packet.trans_header)) {
				return false;
			}
			// 2. 校验buf长度是否足够
			if (buf_len < TransportHeader::header_size() + out_packet.trans_header.body_len) {
				return false;
			}
			// 3. 校验CRC32，数据传输出错直接丢弃
			const char* body_buf = buf + TransportHeader::header_size();
			uint32_t actual_checksum = crc32(0, reinterpret_cast<const uint8_t*>(body_buf), out_packet.trans_header.body_len);
			if (actual_checksum != out_packet.trans_header.checksum) {
				return false;
			}
			// 4. 解析JSON，捕获JSON异常，不崩溃
			try
			{
				std::string body_str(body_buf, out_packet.trans_header.body_len);
				nlohmann::json full_json = nlohmann::json::parse(body_str);
				out_packet.base_header = BasePacket::from_json(full_json.at("base"));
				out_packet.body = full_json.at("body");
				return true;
			}
			catch (const nlohmann::json::exception& e)
			{
				// JSON解析失败打日志，返回false，不崩溃
				return false;
			}
		}

		//  快捷方法：把不同业务包转成body，方便序列化
		template<typename T>
		static AiSchedulePacket build(uint64_t seq, PacketType type, const std::string& sender_id, const T& body) {
			AiSchedulePacket packet;
			packet.base_header.seq = seq;
			packet.base_header.type = type;
			packet.base_header.sender_id = sender_id;
			packet.body = body.to_json();
			return packet;
		}
	};
} // namespace AiSchedule
