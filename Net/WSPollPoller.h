#pragma once

#include "poller.h"


#ifdef _WIN32

#include <winsock2.h>

#undef ERROR


namespace TcFrame
{
    class EventLoop;

    /*
    @brief: Windows平台WSAPoll实现的Poller，适配大连接数场景
    用unordered_map做fd→下标的映射，O(1)查找Channel
    */
    class WSPollPoller :public Poller
    {
    public:
        explicit WSPollPoller(EventLoop* loop);
        ~WSPollPoller() override;

        // 实现基类Poller接口
        int Poll(int timeout_ms, std::vector<Channel*>& active_channels) override;
        void UpdateChannel(Channel* channel) override;
        void RemoveChannel(Channel* channel) override;

    private:
        // 将WSAPoll的事件位转换成Channel需要的revents，完成事件映射
        int TransRevents(short events);

    private:
        EventLoop* m_ownerLoop;	// 所属的EventLoop
        std::vector<WSAPOLLFD> m_pollfds;    // WSAPoll需要的pollfd列表，所有监听的socket都在这里
        std::vector<Channel*> m_channels;    // Channel列表，和m_pollfds一一对应，索引直接找到Channel
        std::unordered_map<SocketType, size_t> m_fd_to_idx; // 大连接优化：fd → 列表下标，O(1)查找，不用遍历
    };
}
#endif // _WIN32
