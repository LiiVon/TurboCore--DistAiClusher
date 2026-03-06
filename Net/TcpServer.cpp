#include "tcpserver.h"
#include "logger.h"
#include <cassert>
#include <utility>


namespace TcFrame
{
    TcpServer::TcpServer(EventLoop* main_loop, const Address& listen_addr, size_t thread_num)
        : m_main_loop(main_loop)
        , m_listen_addr(listen_addr)
        , m_thread_num(thread_num)
        , m_next_loop_idx(0)
        , m_started(false)
    {
        LOG_INFO("TcpServer create, main loop address: " +
            std::to_string(reinterpret_cast<uintptr_t>(main_loop)) +
            ", work thread num: " + std::to_string(thread_num));
        m_acceptor = std::make_unique<Acceptor>(main_loop, listen_addr);
        // 设置新连接回调，accept完回调到HandleNewConnection
        m_acceptor->SetNewConnectionCallback([this](Socket&& client_socket, const Address& client_addr) {
            this->HandleNewConnection(std::move(client_socket), client_addr);
            });

        if (m_thread_num > 0)
        {
            m_sub_loops.reserve(m_thread_num);
            for (size_t i = 0; i < m_thread_num; ++i)
            {
                // ✅ 修复：EventLoop传true，自动启动线程，sub loop开始跑事件循环
                auto loop = std::make_unique<EventLoop>(true);
                LOG_INFO("TcpServer create sub loop " + std::to_string(i) +
                    ", address: " + std::to_string(reinterpret_cast<uintptr_t>(loop.get())));
                m_sub_loops.push_back(std::move(loop));
            }
        }
    }

    TcpServer::~TcpServer()
    {
        LOG_DEBUG("TcpServer destructor begin");
        // 先关闭所有活跃连接
        for (auto& item : m_connections)
        {
            TcpConnectionPtr conn = item.second;
            conn->ForceClose();
        }
        // ✅ 修复：退出所有sub loop，等待线程结束，析构干净
        for (auto& sub_loop : m_sub_loops)
        {
            sub_loop->Quit();
            if (sub_loop->GetThread().joinable())
            {
                sub_loop->GetThread().join();
            }
        }
        m_connections.clear();
        LOG_DEBUG("TcpServer destructor done");
    }

    void TcpServer::Start()
    {
        // 原子交换，保证只启动一次，线程安全
        if (m_started.exchange(true))
        {
            LOG_WARN("TcpServer already started, ignored Start()");
            return;
        }
        // 必须在main loop线程调用，符合线程模型
        m_main_loop->AssertInLoopThread();
        // Acceptor开始监听，main loop开始处理accept
        m_acceptor->Listen();
        LOG_INFO("TcpServer started successfully, listening on " + m_listen_addr.ToString());
    }

    EventLoop* TcpServer::NextLoop()
    {
        if (m_thread_num == 0)
        {
            return m_main_loop; // 单线程模式，直接用main loop
        }

        // 轮询选择，原子递增，线程安全，平均分配
        size_t idx = m_next_loop_idx.fetch_add(1) % m_thread_num;
        EventLoop* selected = m_sub_loops[idx].get();
        LOG_DEBUG("TcpServer NextLoop selected loop index: " + std::to_string(idx) +
            ", address: " + std::to_string(reinterpret_cast<uintptr_t>(selected)));
        return selected;
    }

    void TcpServer::HandleNewConnection(Socket&& client_socket, const Address& client_addr)
    {
        m_main_loop->AssertInLoopThread();
        EventLoop* io_loop = NextLoop(); // 轮询选一个sub loop
        std::string conn_name = "Conn-" + client_addr.ToString() + "-" + std::to_string(static_cast<int>(client_socket.GetFd()));

        //  shared_ptr管理连接，生命周期自动管理，跨线程安全
        TcpConnectionPtr conn = std::make_shared<TcpConnection>(io_loop, conn_name, std::move(client_socket), client_addr);

        //  用户设置的回调转给连接，一次设置所有连接复用
        conn->SetConnectionCallback(m_connection_callback);
        conn->SetMessageCallback(m_message_callback);
        conn->SetWriteCompleteCallback(m_write_complete_callback);

        // 关闭回调，连接关闭自动回调到HandleRemoveConnection
        conn->SetCloseCallback([this](const TcpConnectionPtr& conn) { this->HandleRemoveConnection(conn); });

        // ✅ 修复：key是SocketType，不截断，64位Windows也不会冲突
        m_connections[conn->GetFd()] = conn;

        // 抛给io loop线程，执行连接建立，注册事件
        io_loop->RunInLoop([conn]() {
            conn->ConnectEstablished();
            });

        LOG_INFO("TcpServer new connection [" + conn_name + "] from " + client_addr.ToString() +
            ", assigned to loop " + std::to_string(reinterpret_cast<uintptr_t>(io_loop)));
    }

    void TcpServer::HandleRemoveConnection(const TcpConnectionPtr& conn)
    {
        // 必须抛给main loop执行，只有main loop能修改m_connections，线程安全，断言通过
        m_main_loop->RunInLoop([this, conn]() {
            m_main_loop->AssertInLoopThread();
            SocketType fd = conn->GetFd();
            LOG_INFO("TcpServer remove connection [" + conn->GetName() + "] fd: " + std::to_string(static_cast<int>(fd)));

            // 从活跃列表移除，key正确，不会删错
            size_t erased = m_connections.erase(fd);
            assert(erased == 1); // 断言一定能删掉，有问题直接暴露
            (void)erased; // 消除未使用变量警告，Release模式不会报错

            // 再抛给io loop执行连接销毁，移除channel，关闭socket
            EventLoop* io_loop = conn->GetLoop();
            io_loop->RunInLoop([conn]() {
                conn->ConnectDestroyed();
                });

            LOG_DEBUG("TcpServer remove connection done for fd " + std::to_string(static_cast<int>(fd)));
            });
    }

    EventLoop* TcpServer::GetMainLoop() const
    {
        return m_main_loop;
    }

    // 设置回调函数，由用户调用
    void TcpServer::SetConnectionCallback(const ConnectionCallback& cb)
    {
        m_connection_callback = cb;
    }
    void TcpServer::SetMessageCallback(const MessageCallback& cb)
    {
        m_message_callback = cb;
    }
    void TcpServer::SetWriteCompleteCallback(const WriteCompleteCallback& cb)
    {
        m_write_complete_callback = cb;
    }

    size_t TcpServer::GetThreadNum() const
    {
        return m_thread_num;
    }
}
