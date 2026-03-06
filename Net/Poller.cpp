#include "poller.h"
#include "eventloop.h"
#include "logger.h"
#include "channel.h"


// 根据平台引入对应具体实现头文件
#ifdef _WIN32

#include "wspollpoller.h"

#elif defined(__linux__)

#include "epollpoller.h"

#elif defined(__APPLE__) && defined(__MACH__)

#include "kqueuepoller.h"

#endif


namespace TcFrame
{
    Poller* Poller::CreatePoller(EventLoop* loop)
    {
#ifdef _WIN32

        LOG_INFO("create WSPollPoller for Windows platform");
        return new WSPollPoller(loop);
#elif defined(__linux__)

        LOG_INFO("create EpollPoller for Linux platform");
        return new EpollPoller(loop);
#elif defined(__APPLE__) && defined(__MACH__)

        LOG_INFO("create KqueuePoller for macOS/BSD platform");
        return new KqueuePoller(loop);
#else

        LOG_FATAL("unsupported platform: no Poller implementation available");
        return nullptr;
#endif

    }
}
