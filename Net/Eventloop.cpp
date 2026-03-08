#include "eventloop.h"
#include "channel.h"
#include "socket_utils.h"
#include "poller.h"
#include "logger.h"
#include "global.h"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR

#else
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#endif

namespace TcFrame
{
	// 线程本地存储定义，每个线程一个实例
	thread_local EventLoop* t_currentThreadLoop = nullptr;

#ifdef _WIN32

	void EventLoop::CreateWakeupPair(SocketType& readFd, SocketType& writeFd)
	{
		// Windows下用本地回环socket pair实现唤醒
		SocketType listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener == INVALID_SOCKET_VALUE)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair create listener failed: " + SocketUtils::GetLastErrorStr(err));
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0; // 系统自动分配空闲端口

		int ret = bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
		if (ret != 0)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair bind listener failed: " + SocketUtils::GetLastErrorStr(err));
		}

		listen(listener, 1);

		// 获取系统分配的实际端口
		sockaddr_in local_addr{};
		int len = sizeof(local_addr);
		getsockname(listener, reinterpret_cast<sockaddr*>(&local_addr), &len);

		writeFd = socket(AF_INET, SOCK_STREAM, 0);
		if (writeFd == INVALID_SOCKET_VALUE)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair create writeFd failed: " + SocketUtils::GetLastErrorStr(err));
		}

		ret = connect(writeFd, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr));
		if (ret != 0)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair connect writeFd failed: " + SocketUtils::GetLastErrorStr(err));
		}

		readFd = accept(listener, nullptr, nullptr);
		if (readFd == INVALID_SOCKET_VALUE)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair accept failed: " + SocketUtils::GetLastErrorStr(err));
		}

		SocketUtils::CloseSocket(listener);
		SocketUtils::SetNonBlocking(readFd);
		SocketUtils::SetNonBlocking(writeFd);

		LOG_DEBUG("CreateWakeupPair done, readFd: " + std::to_string(static_cast<int>(readFd)) + ", writeFd: " + std::to_string(static_cast<int>(writeFd)));
	}
#else

	void EventLoop::CreateWakeupPair(SocketType& readFd, SocketType& writeFd)
	{
		// Linux下用eventfd，更简单高效，读写都是同一个fd，大连接下开销更小
		int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (efd < 0)
		{
			LOG_FATAL("CreateWakeupPair eventfd failed: " + std::string(strerror(errno)));
		}
		readFd = efd;
		writeFd = efd;
		LOG_DEBUG("CreateWakeupPair (eventfd) done, fd: " + std::to_string(static_cast<int>(readFd)));
	}
#endif

	EventLoop::EventLoop(bool auto_start)
		: m_quit(false)
		, m_callingPendingFunctors(false)
		, m_wakeupFd(INVALID_SOCKET_VALUE)
		, m_wakeupWriteFd(INVALID_SOCKET_VALUE)
	{
		m_threadId = std::this_thread::get_id();

		// 创建平台对应的Poller
		m_poller = std::unique_ptr<Poller>(Poller::CreatePoller(this));

		// 创建跨线程唤醒pair
		CreateWakeupPair(m_wakeupFd, m_wakeupWriteFd);

		// 创建wakeup channel，注册读回调
		m_wakeupChannel = std::make_unique<Channel>(this, m_wakeupFd);
		m_wakeupChannel->SetReadCallback([this]() { HandleWakeupRead(); });
		m_wakeupChannel->EnableReading();

		// 设置线程本地存储
		t_currentThreadLoop = this;

		// 自动启动：开新线程运行Loop
		if (auto_start)
		{
			m_thread = std::thread([this]() {
				// 新线程重新设置线程ID和本地存储，覆盖原来的主线程ID
				m_threadId = std::this_thread::get_id();
				t_currentThreadLoop = this;
				LOG_INFO("EventLoop thread started, thread ID: " + thread_id_to_str(m_threadId));
				this->Loop();
				});
		}

		std::stringstream ss;
		ss << "EventLoop created (large connection optimized), thread ID: " << m_threadId;
		LOG_INFO(ss.str());
	}

	EventLoop::~EventLoop()
	{
		m_wakeupChannel->DisableAll();
		RemoveChannel(m_wakeupChannel.get());

		// 关闭唤醒fd
#ifdef _WIN32

		closesocket(m_wakeupFd);
		if (m_wakeupWriteFd != m_wakeupFd)
		{
			closesocket(m_wakeupWriteFd);
		}
#else

		close(m_wakeupFd);
		if (m_wakeupWriteFd != m_wakeupFd)
		{
			close(m_wakeupWriteFd);
		}
#endif

		// join线程，等待Loop退出
		if (m_thread.joinable())
		{
			m_thread.join();
		}

		t_currentThreadLoop = nullptr;
		LOG_INFO("EventLoop destroyed, thread ID: " + thread_id_to_str(m_threadId));
	}

	void EventLoop::Loop()
	{
		AssertInLoopThread();
		m_quit = false;
		LOG_INFO("EventLoop start looping, thread ID: " + thread_id_to_str(m_threadId));

		while (!m_quit)
		{
			// 清空上一次的就绪Channel
			m_activeChannels.clear();
			// Poller阻塞等待事件，超时10ms，保证定时任务和跨线程任务及时处理
			int numEvents = m_poller->Poll(10, m_activeChannels);

			// 处理所有就绪IO事件
			for (Channel* channel : m_activeChannels)
			{
				channel->HandleEvent();
			}

			// 执行跨线程排队的任务
			DoPendingFunctors();
			// 处理过期的定时任务
			ProcessDelayedTasks();
		}

		LOG_INFO("EventLoop stop looping, thread ID: " + thread_id_to_str(m_threadId));
	}

	void EventLoop::Quit()
	{
		m_quit = true;
		// 如果不在Loop线程，唤醒阻塞的Poll
		if (!IsInLoopThread())
		{
			Wakeup();
		}
		LOG_INFO("EventLoop quit requested, thread ID: " + thread_id_to_str(m_threadId));
	}

	void EventLoop::UpdateChannel(Channel* channel)
	{
		AssertInLoopThread();
		m_poller->UpdateChannel(channel);
	}

	void EventLoop::RemoveChannel(Channel* channel)
	{
		AssertInLoopThread();
		m_poller->RemoveChannel(channel);
	}

	void EventLoop::RunInLoop(Functor cb)
	{
		if (IsInLoopThread())
		{
			// 已经在Loop线程，直接执行，无锁
			cb();
		}
		else
		{
			// 不在，排队唤醒
			QueueInLoop(std::move(cb));
		}
	}

	void EventLoop::QueueInLoop(Functor cb)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_pendingFunctors.push_back(std::move(cb));
		}

		// 需要唤醒的两种情况：
		// 1. 调用者不在Loop线程 → Loop阻塞在Poll，需要唤醒处理新任务
		// 2. 调用者在Loop线程，但是当前正在执行pending任务 → 新任务加入，需要唤醒下次循环处理
		if (!IsInLoopThread() || m_callingPendingFunctors)
		{
			Wakeup();
		}
	}

	void EventLoop::Wakeup()
	{
		// 写一个8字节数据，唤醒读端，多次唤醒也只会触发一次读，读完就干净
		uint64_t one = 1;
		ssize_t n;
#ifdef _WIN32

		n = send(m_wakeupWriteFd, reinterpret_cast<const char*>(&one), sizeof(one), 0);
#else

		n = write(m_wakeupWriteFd, &one, sizeof(one));
#endif

		if (n != sizeof(one))
		{
			int err = SocketUtils::GetLastError();
			LOG_WARN("Wakeup write partial data: " + SocketUtils::GetLastErrorStr(err));
		}
		LOG_DEBUG("EventLoop wakeup sent");
	}

	bool EventLoop::IsInLoopThread() const
	{
		return m_threadId == std::this_thread::get_id();
	}

	void EventLoop::AssertInLoopThread()
	{
		if (!IsInLoopThread())
		{
			std::stringstream ss;
			ss << "AssertInLoopThread failed: EventLoop created in thread " << m_threadId
				<< ", current thread is " << std::this_thread::get_id();
			LOG_FATAL(ss.str());
			std::abort();
		}
	}

	std::thread& EventLoop::GetThread()
	{
		return m_thread;
	}
	const std::thread& EventLoop::GetThread() const
	{
		return m_thread;
	}

	EventLoop* EventLoop::GetCurrentThreadEventLoop()
	{
		return t_currentThreadLoop;
	}

	void EventLoop::HandleWakeupRead()
	{
		uint64_t one;
		ssize_t n;
		// 循环读完所有唤醒数据，保证不会残留数据触发多余读事件
		do
		{
#ifdef _WIN32

			n = recv(m_wakeupFd, reinterpret_cast<char*>(&one), sizeof(one), 0);
#else

			n = read(m_wakeupFd, &one, sizeof(one));
#endif

			if (n <= 0)
			{
				if (n == 0)
				{
					LOG_DEBUG("Wakeup fd closed by peer");
				}
				else
				{
					int err = SocketUtils::GetLastError();
#ifdef _WIN32

					if (err != WSAEWOULDBLOCK)
#else

					if (err != EAGAIN && err != EWOULDBLOCK)
#endif

					{
						LOG_ERROR("Wakeup read error: " + SocketUtils::GetLastErrorStr(err));
					}
				}
				break;
			}
		} while (true);

		LOG_DEBUG("EventLoop waked up");
	}

	void EventLoop::DoPendingFunctors()
	{
		std::vector<Functor> functors;
		m_callingPendingFunctors = true;

		// 缩小锁范围：swap到局部变量，释放锁再执行，减少锁持有时间，大并发下不阻塞
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			functors.swap(m_pendingFunctors);
		}

		// 无锁执行所有任务，非常快
		for (const Functor& functor : functors)
		{
			functor();
		}

		m_callingPendingFunctors = false;
	}

	void EventLoop::RunAfter(double delay_seconds, std::function<void()> cb)
	{
		DelayedTask task;
		task.seconds = delay_seconds;
		task.expire_time = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(static_cast<int64_t>(delay_seconds * 1000));
		task.cb = std::move(cb);

		// 加锁插入优先队列
		std::lock_guard<std::mutex> lock(m_mutex);
		m_delayed_tasks.push(std::move(task));

		// 不在Loop线程或者正在执行pending任务，需要唤醒处理
		if (!IsInLoopThread() || m_callingPendingFunctors)
		{
			Wakeup();
		}
	}

	void EventLoop::ProcessDelayedTasks()
	{
		AssertInLoopThread();
		std::vector<DelayedTask> ready_tasks;

		// 加锁取出所有过期任务，因为是小顶堆，top就是最早过期的
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			auto now = std::chrono::steady_clock::now();
			// 一直取到第一个没过期的就停止，O(k log n)，k是本次过期的任务数，非常高效
			while (!m_delayed_tasks.empty() && m_delayed_tasks.top().expire_time <= now)
			{
				ready_tasks.push_back(m_delayed_tasks.top());
				m_delayed_tasks.pop();
			}
		}

		// 无锁执行回调，快
		for (auto& task : ready_tasks)
		{
			if (task.cb)
			{
				task.cb();
			}
		}
	}
}
