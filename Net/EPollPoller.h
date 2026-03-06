#pragma once
#ifdef __linux__
#include "poller.h"

#include <sys/epoll.h>

#include <vector>



namespace TcFrame
{
    class EventLoop;
    class Channel;

    /*
    @brief: Linux平台epoll实现的Poller，大连接优化版，O(1)查找Channel
    */
    class EpollPoller : public Poller
    {
    public:
        explicit EpollPoller(EventLoop* loop);
        ~EpollPoller() override;

        // 实现基类接口
        int Poll(int timeout_ms, std::vector<Channel*>& active_channels) override;
        void UpdateChannel(Channel* channel) override;
        void RemoveChannel(Channel* channel) override;

    private:
        // 将epoll事件转换为Channel的revents
        int TransRevents(uint32_t events);
        // 更新epoll的监听事件
        void UpdateEpoll(int op, Channel* channel);

    private:
        EventLoop* m_ownerLoop;
        int m_epoll_fd; // epoll实例的fd
        std::vector<epoll_event> m_events; // epoll等待事件返回的缓冲区
        std::unordered_map<SocketType, Channel*> m_fd_to_channel; // 大连接优化：fd -> Channel*，O(1)查找
    };
}
#endif // __linux__
