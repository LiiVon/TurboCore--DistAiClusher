#include "threadpool.h"
#include "logger.h"
#include "global.h"

namespace TcFrame
{
	ThreadPool::ThreadPool(size_t thread_num)
		: m_is_running(true), m_target_thread_num(thread_num)
	{
		LOG_INFO("create thread pool, initial thread num: " + std::to_string(thread_num));
		for (size_t i = 0; i < thread_num; ++i)
		{
			// emplace_back直接构造，比push_back更高效
			m_workers.emplace_back(&ThreadPool::WorkerLoop, this);
		}
	}

	ThreadPool::~ThreadPool()
	{
		Shutdown();
	}

	void ThreadPool::WorkerLoop()
	{
		// 打印线程标识，方便调试
		size_t thread_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
		LOG_DEBUG("thread " + std::to_string(thread_hash) + " start");

		// 循环：线程池运行 || 还有任务没执行 || 当前线程数没超过目标 → 继续工作
		while (m_is_running.load() || !m_tasks.empty())
		{
			// 如果当前线程数已经超过目标，当前线程可以主动退出（实现缩容）
			if (m_workers.size() > m_target_thread_num.load())
			{
				LOG_DEBUG("thread " + std::to_string(thread_hash) + " exit for resize shrink");
				break;
			}

			std::unique_lock<std::mutex> lock(m_queue_mutex);
			// 等待：线程池停止 或者 有任务来了 → 唤醒
			m_cv.wait(lock, [this]() {
				return !m_is_running.load() || !m_tasks.empty();
				});

			// 如果线程池停止且没有任务了，退出
			if (!m_is_running.load() && m_tasks.empty())
			{
				break;
			}

			// 取出任务，移动拷贝，减少拷贝
			std::function<void()> task;
			task = std::move(m_tasks.front());
			m_tasks.pop();
			// 取出后立刻释放锁，让其他线程可以继续放任务，减少锁竞争
			lock.unlock();

			LOG_DEBUG("thread " + std::to_string(thread_hash) + " process task");
			task();
		}

		LOG_DEBUG("thread " + std::to_string(thread_hash) + " exit");
	}

	void ThreadPool::CleanupExitedThreads()
	{
		std::lock_guard<std::mutex> lock(m_queue_mutex);
		// 移除已经join完成的退出线程，只保留正在运行的线程
		m_workers.erase(
			std::remove_if(m_workers.begin(), m_workers.end(),
				[](std::thread& t) {
					// 如果线程已经可以join，说明已经退出，移除它
					if (t.joinable())
					{
						t.join();
						return true;
					}
					return false;
				}),
			m_workers.end()
		);
	}

	size_t ThreadPool::GetThreadNum() const
	{
		// 返回当前实际正在运行的线程数
		std::lock_guard<std::mutex> lock(m_queue_mutex);
		return m_workers.size();
	}

	size_t ThreadPool::GetPendingTaskNum() const
	{
		std::lock_guard<std::mutex> lock(m_queue_mutex);
		return m_tasks.size();
	}

	bool ThreadPool::IsRunning() const
	{
		return m_is_running.load();
	}

	void ThreadPool::Resize(size_t new_thread_num)
	{
		if (!m_is_running.load())
		{
			LOG_WARN("cannot resize thread pool: thread pool is not running");
			return;
		}

		size_t old_thread_num = m_target_thread_num.load();
		if (new_thread_num == old_thread_num)
		{
			return; // 没有变化，不用处理
		}

		LOG_INFO("resize thread pool from " + std::to_string(old_thread_num) + " to " + std::to_string(new_thread_num));
		m_target_thread_num = new_thread_num;

		if (new_thread_num > old_thread_num)
		{
			// 扩容：直接加新线程
			size_t add_num = new_thread_num - old_thread_num;
			for (size_t i = 0; i < add_num; ++i)
			{
				m_workers.emplace_back(&ThreadPool::WorkerLoop, this);
			}
		}
		else if (new_thread_num < old_thread_num)
		{
			// 缩容：唤醒多余线程，让它们主动退出
			size_t remove_num = old_thread_num - new_thread_num;
			for (size_t i = 0; i < remove_num; ++i)
			{
				m_cv.notify_one();
			}
		}

		// 清理已经退出的线程
		CleanupExitedThreads();
	}

	void ThreadPool::Shutdown()
	{
		LOG_INFO("shutting down thread pool");
		if (!m_is_running.load())
		{
			LOG_WARN("thread pool is already shutdown");
			return;
		}

		m_is_running = false;
		m_cv.notify_all(); // 唤醒所有线程，处理完剩余任务退出

		// 等待所有线程退出，清理
		for (auto& worker : m_workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}

		// 清空任务队列
		std::lock_guard<std::mutex> lock(m_queue_mutex);
		while (!m_tasks.empty())
		{
			m_tasks.pop();
		}

		// 清空线程列表
		m_workers.clear();

		LOG_INFO("thread pool shutdown complete, total tasks processed: unknown");
	}
}
