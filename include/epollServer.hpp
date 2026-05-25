#ifndef __EPOLL_SERVER_HPP__
#define __EPOLL_SERVER_HPP__

#include <iostream>
#include <sys/epoll.h>
#include <sys/un.h> 
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cstring>
#include <unordered_map>
#include "ringBuffer.hpp"
#include "utils.hpp"

constexpr int DEFAULT_MAX_EVENTS = 1024;
constexpr unsigned long MAX_CONNECTION_IDLE_TIME = 60;
constexpr size_t DEFAULT_INBOUND_BUFFER_SIZE = 4 * 1024;            // 4 KB
constexpr size_t DEFAULT_OUTBOUND_BUFFER_SIZE = 16 * 1024 * 1024;   // 16 KB

namespace gen {

class EpollServer
{
public:
    // Note: We ignore SIGPIPE so the server doesn't crash on broken sockets
    EpollServer(unsigned int threadsCount, int backlog = SOMAXCONN)
        : mThreadsCount(threadsCount), 
          mMaxFds(GetMaxFiles()),
          mBacklog(backlog) { signal(SIGPIPE, SIG_IGN); }
    virtual ~EpollServer() { Stop(); }

    void AddListener(uint16_t port);
    void AddListener(const char* socketPath);

    bool Start();
    bool Start(unsigned short port);
    bool Start(const char* socketPath);

    void Stop();

    void SetVerbose(bool verbose) { mVerbose = verbose; }
    void SetInboundBufferSize(size_t size) { mInboundBufferSize = size; }
    void SetOutboundBufferSize(size_t size) { mOutboundBufferSize = size; }

protected:
    struct ClientContext
    {
        ClientContext(EpollServer& srvIn)
            : srv(srvIn),
              inboundBuffer(srvIn.mInboundBufferSize),
              outboundBuffer(srvIn.mOutboundBufferSize) {}
        virtual ~ClientContext() = default;

        RingBuffer& GetInboundBuffer() { return inboundBuffer ; }
        bool Send(const void* data, size_t len) { return srv.Send(this, data, len); }

    private: 
        EpollServer& srv;
        int fd{-1};
        std::chrono::time_point<std::chrono::steady_clock> lastActivityTime;
        uint64_t connectionId{0};
        std::atomic<bool> wantsWrite{false};
        RingBuffer inboundBuffer{DEFAULT_INBOUND_BUFFER_SIZE};
        RingBuffer outboundBuffer{DEFAULT_OUTBOUND_BUFFER_SIZE};

        friend class EpollServer;
    };

    virtual bool OnInit() { return true; }
    virtual std::shared_ptr<ClientContext> MakeClientContext() = 0;
    virtual bool OnDataReceived(std::shared_ptr<ClientContext>& client) = 0;
    virtual bool OnDataSent(std::shared_ptr<ClientContext>& client) { return true; }
    virtual void OnError(const char* fname, int lineNum, const std::string& err) const;
    virtual void OnInfo(const char* fname, int lineNum, const std::string& info) const;

private:
    struct Listener
    {
        int domain{0};          // Holds standard system defines: AF_INET, AF_INET6, or AF_UNIX
        uint16_t port{0};       // Used for TCP (0 for UNIX)
        std::string address;    // IP:Port or Path for debugging/logging
        int unixFd{-1};         // UNIX socket fd
        std::string unixPath;   // UNIX socket path
        bool isAbstract{false}; // UNIX socket in abstract namespace
    };

    int GetMaxFiles();
    int SetupTcpSocket(unsigned short port, int backlog, std::string& errMsg) const;
    int SetupUnixSocket(const std::string& unixPath, int backlog, bool isAbstract, std::string& errMsg) const;
    void ReactorLoop();
    bool FlushOutboundBuffer(std::shared_ptr<ClientContext>& client);
    bool Send(ClientContext* client, const void* data, size_t len);
    bool InitThreadListeners(std::vector<int>& localListenerFds);
    void CloseThreadTcpListeners(const std::vector<int>& localListenerFds);

    enum class RecvStatus
    {
        UNKNOWN = 0,
        OK,
        DISCONNECT,
        ERROR
    };

    RecvStatus Receive(std::shared_ptr<gen::EpollServer::ClientContext>& client);

private:
    unsigned int mThreadsCount{0};
    std::atomic<bool> mServerRunning{false};
    std::atomic<uint64_t> mNextConnectionId{1};
    std::vector<std::thread> mThreads;
    
    std::vector<Listener> mListeners; // The collection of active listeners. 

    // unsigned short mPort{0};
    // int mUnixDomainSocket{-1};
    // std::string mActiveUnixPath;
    int mMaxFds{0};
    int mBacklog{SOMAXCONN};

    constexpr static uint64_t EPOLLED_LISTEN_FLAG = static_cast<uint64_t>(1) << 63;
    constexpr static uint64_t EPOLLED_FD_MASK = 0xFFFFFFFFULL;

protected:
    bool mVerbose{false};
    size_t mInboundBufferSize{DEFAULT_INBOUND_BUFFER_SIZE};
    size_t mOutboundBufferSize{DEFAULT_OUTBOUND_BUFFER_SIZE};
};

inline int EpollServer::GetMaxFiles()
{
    struct rlimit rl;
    if(getrlimit(RLIMIT_NOFILE, &rl) == 0)
        return static_cast<int>(rl.rlim_cur);
    return 65535;
}

inline void EpollServer::OnError(const char* fname, int lineNum, const std::string& err) const
{
    std::cerr << "Error: " << fname << ":" << lineNum << " " << err << std::endl;
}

inline void EpollServer::OnInfo(const char* fname, int lineNum, const std::string& info) const
{
    std::cout << "Info: " << fname << ":" << lineNum << " " << info << std::endl;
}

inline void EpollServer::AddListener(uint16_t port)
{
    // Search the vector for any existing TCP listener matching this port
    auto it = std::find_if(mListeners.begin(), mListeners.end(), [port](const Listener& l)
    {
        return l.domain == AF_INET && l.port == port;
    });

    if(it != mListeners.end())
    {
        OnError(__FNAME__, __LINE__, "Configuration rejected: TCP port " + std::to_string(port) + " is already configured.");
        return;
    }

    Listener listener;
    listener.domain = AF_INET;
    listener.port = port;
    mListeners.push_back(listener);
}

inline void EpollServer::AddListener(const char* socketPath)
{
    bool isAbstract = false;
    std::string unixPath;
    if(socketPath[0] == '@' || socketPath[0] == '\0')
    {
        isAbstract = true;
        unixPath = socketPath + 1;
    }
    else
    {
        isAbstract = false;
        unixPath = socketPath;
    }

    // Search the vector for any existing UNIX listener matching this file path
    auto it = std::find_if(mListeners.begin(), mListeners.end(), [&unixPath](const Listener& l)
    {
        return l.domain == AF_UNIX && l.unixPath == unixPath;
    });

    if(it != mListeners.end())
    {
        OnError(__FNAME__, __LINE__, "Configuration rejected: UNIX socket path '" + unixPath + "' is already configured.");
        return;
    }

    Listener listener;
    listener.domain = AF_UNIX;
    listener.unixPath = unixPath;
    listener.isAbstract = isAbstract;
    mListeners.push_back(listener);
}

inline bool EpollServer::Start(unsigned short port)
{
    AddListener(port);
    return Start();
}

inline bool EpollServer::Start(const char* socketPath)
{
    AddListener(socketPath);
    return Start();
}

inline bool EpollServer::Start()
{
    if(!OnInit())
    {
        OnError(__FNAME__, __LINE__, "Initialization failed: OnInit() returned false");
        return false;
    }

    if(mListeners.empty())
    {
        OnError(__FNAME__, __LINE__, "Cannot start server: No listeners have been added.");
        return false;
    }

    // Process and initialize global listeners (like UNIX domain sockets)
    for(auto& listener : mListeners)
    {
        if(listener.domain == AF_UNIX)
        {
            // Call your original UNIX socket initialization method here on the main thread.
            // Note: Update your SetupUnixSocket signature to accept your backlog configuration!
            std::string errMsg;
            listener.unixFd = SetupUnixSocket(listener.unixPath.c_str(), mBacklog, listener.isAbstract, errMsg);

            if(listener.unixFd == -1)
            {
                OnError(__FNAME__, __LINE__, errMsg);

                // Clean up any previously opened UNIX sockets before aborting
                for(auto& clean : mListeners)
                {
                    if(clean.unixFd != -1)
                        ::close(clean.unixFd);
                }
                return false;
            }
        }
    }

    // Sockets are validated and ready
    mServerRunning = true;

    for(auto& listener : mListeners)
    {
        std::stringstream ss;
        if(listener.domain == AF_UNIX)
        {
            ss << "Starting Server on Unix Domain Socket '" << listener.unixPath << "' with " << mThreadsCount << " threads...";
        }
        else
        {
            ss << "Starting Server on port " << listener.port << " with " << mThreadsCount << " threads...";
        }
        OnInfo(__FNAME__, __LINE__, ss.str());
    }

    // AI Review Note: This sequential consistency memory fence is added
    // to eliminate a subtle multithreaded initialization race condition. 
    // While 'mServerRunning' is atomic, 'mListeners' is a plain std::vector.
    // Without this fence, highly optimizing compilers or relaxed CPU architectures
    // (like ARM64) can theoretically reorder memory writes, spawning worker threads
    // before all local configurations (such as the main thread's updates to listener.unixFd)
    // are fully flushed down from the local CPU cache rows into globally shared memory. 
    // This fence establishes a strict data-synchronization barrier, guaranteeing total 
    // cross-thread visibility of the initialization state before the workers boot.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    for(unsigned int i = 0; i < mThreadsCount; ++i)
        mThreads.emplace_back([this]() { ReactorLoop(); });

    return true;
}

inline void EpollServer::Stop()
{
    if(!mServerRunning)
        return;

    mServerRunning = false;

    for(auto& t : mThreads)
    {
        if(t.joinable())
            t.join();
    }
    mThreads.clear();

    // Main thread safely cleans up global shared assets now that threads are dead
    for(auto& listener : mListeners)
    {
        if(listener.domain == AF_UNIX && listener.unixFd != -1)
        {
            ::close(listener.unixFd);

            // Unlink the file socket from the file system namespace
            if(!listener.unixPath.empty())
            {
                ::unlink(listener.unixPath.c_str());
            }
            listener.unixFd = -1;
        }
    }
}

inline void EpollServer::ReactorLoop()
{
    int threadEpollFd = epoll_create1(EPOLL_CLOEXEC);
    if(threadEpollFd == -1)
    {
        OnError(__FNAME__, __LINE__, "epoll_create1() failed: " + std::string(strerror(errno)));
        return;
    }

    // Inialize all listeneres
    std::vector<int> localListenerFds;
    if(!InitThreadListeners(localListenerFds))
    {
        OnError(__FNAME__, __LINE__, "Thread failed to initialize all requested listeners.");
        ::close(threadEpollFd);
        return; // Exit the thread
    }

    // Add listeners to to the epool
    for(int listenFd : localListenerFds)
    {
        struct epoll_event ev = {};
        ev.events = EPOLLIN | EPOLLET;
        
        // Use event.data.u64 field to pack both the socket and listener boollean flag.
        // Set the least significant bits for the socket file descriptor and 
        // the highest bit for the boolean flag.
        // Note: Mask with 0xFFFFFFFFULL ensures we only take the 32 bits of the FD
        // and don't accidentally pollute the 63rd bit via sign extension.
        ev.data.u64 = (static_cast<uint64_t>(listenFd) & EPOLLED_FD_MASK) | EPOLLED_LISTEN_FLAG;

        if(epoll_ctl(threadEpollFd, EPOLL_CTL_ADD, listenFd, &ev) == -1)
        {
            OnError(__FNAME__, __LINE__, "epoll_ctl ADD failed for listener FD " + std::to_string(listenFd) + ": " + std::string(strerror(errno)));
            CloseThreadTcpListeners(localListenerFds);
            ::close(threadEpollFd);
            return; // Exit the thread
        }
    }

    std::unordered_map<int, std::shared_ptr<ClientContext>> localClients;
    localClients.reserve(1024); // Start with a reasonable baseline
    
    // Epool event processing loop
    struct epoll_event evs[DEFAULT_MAX_EVENTS];
    auto lastCleanupTime = std::chrono::steady_clock::now();

    while(mServerRunning)
    {
        int numEvents = epoll_wait(threadEpollFd, evs, DEFAULT_MAX_EVENTS, 100);
        if(numEvents < 0)
        {
            if(errno == EINTR)
                continue;
            break;
        }

        for(int i = 0; i < numEvents && mServerRunning; ++i)
        {
            uint32_t events = evs[i].events;

            // Unpack event.data.u64 field to the socket file descriptor and listener boollean flag:
            // - If the highest bit is set, it's a listener
            // - Erase the highest bit (boolean flag bit) to unpack the socket
            bool isListener = (evs[i].data.u64 & EPOLLED_LISTEN_FLAG) != 0;
            int eventFd = static_cast<int>(evs[i].data.u64 & EPOLLED_FD_MASK); // We only need the bottom 32 bits

            if(isListener)
            {
                while(true)
                {
                    // Use accept4 to set non-blocking and cloexec immediately
                    int clientFd = accept4(eventFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

                    if(clientFd == -1)
                    {
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break; // Queue is empty, we are done for this wakeup
                        }
                        else if(errno == EINTR)
                        {
                            continue; // Interrupted by signal, try again
                        }
                        else
                        {
                            OnError(__FNAME__, __LINE__, "accept4() failed: " + std::string(strerror(errno)));
                            break;
                        }
                    }

                    // Safety check for vector bounds
                    if(clientFd >= mMaxFds)
                    {
                        OnError(__FNAME__, __LINE__, "Max FDs reached, closing connection");
                        close(clientFd);
                        continue;
                    }

                    // Register the new client with ONESHOT and ET (Edge Triggering)
                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
                    cev.data.u64 = (static_cast<uint64_t>(clientFd) & EPOLLED_FD_MASK);

                    if(epoll_ctl(threadEpollFd, EPOLL_CTL_ADD, clientFd, &cev) == -1)
                    {
                        OnError(__FNAME__, __LINE__, "epoll_ctl(clientFd) failed: " + std::string(strerror(errno)));
                        close(clientFd);
                        continue;
                    }

                    // Initialize context
                    auto ctx = MakeClientContext();
                    ctx->fd = clientFd;
                    ctx->connectionId = mNextConnectionId.fetch_add(1, std::memory_order_relaxed);
                    ctx->lastActivityTime = std::chrono::steady_clock::now();
                    localClients[clientFd] = ctx;

                    if(mVerbose)
                        OnInfo(__FNAME__, __LINE__, "Accepted connection ID: " + std::to_string(ctx->connectionId));
                }
            }
            else
            {
                int clientFd = eventFd;
                auto it = localClients.find(clientFd);
                if(it == localClients.end())
                    continue;
                auto& client = it->second;

                bool keepAlive = true;
                client->lastActivityTime = std::chrono::steady_clock::now();

                // Outbound processing block (standard EPOLLOUT)
                if(events & EPOLLOUT)
                {
                    keepAlive = FlushOutboundBuffer(client);
                    if(keepAlive && !client->wantsWrite)
                        keepAlive = OnDataSent(client);
                }

                // Inbound processing block
                if(keepAlive && (events & EPOLLIN))
                {
                    auto status = Receive(client);

                    // Always process data if we have it, regardless of status
                    if(!client->inboundBuffer.Empty())
                    {
                        keepAlive = OnDataReceived(client);
                    }

                    // If Receive signaled a disconnect or termination state, mark for closure
                    if(status == RecvStatus::DISCONNECT)
                    {
                        keepAlive = false;
                    }
                    else if(status == RecvStatus::ERROR)
                    {
                        keepAlive = false;
                    }

                    // Flush any responses before the loop potentially closes the FD
                    if(client->wantsWrite)
                    {
                        if(!FlushOutboundBuffer(client))
                            keepAlive = false;
                    }
                }

                // Final cleanup for this iteration
                if(!keepAlive || (events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)))
                {
                    epoll_ctl(threadEpollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                    close(clientFd);
                    localClients.erase(clientFd);
                }
                else
                {
                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
                    if(client->wantsWrite)
                        cev.events |= EPOLLOUT;
                    cev.data.u64 = (static_cast<uint64_t>(clientFd) & EPOLLED_FD_MASK);

                    if(epoll_ctl(threadEpollFd, EPOLL_CTL_MOD, clientFd, &cev) == -1)
                    {
                        OnError(__FNAME__, __LINE__, "epoll_ctl(clientFd) failed: " + std::string(strerror(errno)));
                        close(clientFd);
                        localClients.erase(clientFd);
                    }
                }
            }
        }

        // Idle connections cleanup
        auto now = std::chrono::steady_clock::now();
        if(now - lastCleanupTime > std::chrono::seconds(5))
        {
            for(auto it = localClients.begin(); it != localClients.end();)
            {
                auto& client = it->second;
                if(now - client->lastActivityTime > std::chrono::seconds(MAX_CONNECTION_IDLE_TIME))
                {
                    if(mVerbose)
                        OnInfo(__FNAME__, __LINE__, "Closing idle connection ID: " + std::to_string(client->connectionId));

                    // Cleanup: Close and remove from epoll
                    epoll_ctl(threadEpollFd, EPOLL_CTL_DEL, it->first, nullptr);
                    close(it->first);

                    it = localClients.erase(it); // go to the next element
                }
                else
                {
                    ++it;
                }
            }
            lastCleanupTime = now;
        }
    }

    for(auto& [fd, client] : localClients)
    {
        close(fd);
    }
    localClients.clear();

    // Clean up 
    CloseThreadTcpListeners(localListenerFds);
    close(threadEpollFd);
}

inline bool EpollServer::InitThreadListeners(std::vector<int>& listenerFds)
{
    bool isInitialized = true;
    
    for(const auto& listener : mListeners)
    {
        int listenFd = -1;

        if(listener.domain == AF_INET)
        {
            std::string errMsg;
            listenFd = SetupTcpSocket(listener.port, mBacklog, errMsg);
            if(listenFd < 0)
            {
                OnError(__FNAME__, __LINE__, errMsg);
                isInitialized = false;
                break;
            }
        }
        else if(listener.domain == AF_UNIX)
        {
            // Bind to the shared UNIX socket descriptor created in the Start phase
            listenFd = listener.unixFd;
            if(listenFd < 0)
            {
                OnError(__FNAME__, __LINE__, "Thread missing valid shared UNIX socket descriptor.");
                isInitialized = false;
                break;
            }
        }
        else
        {
            // Explicit fallback for corrupted or unsupported socket domains
            OnError(__FNAME__, __LINE__, "Encountered completely unsupported socket domain: " + std::to_string(listener.domain));
            isInitialized = false;
            break;
        }

        listenerFds.push_back(listenFd);
    }

    if(!isInitialized || listenerFds.size() != mListeners.size())
    {
        CloseThreadTcpListeners(listenerFds);
    }

    return isInitialized;
}

inline void EpollServer::CloseThreadTcpListeners(const std::vector<int>& localListenerFds)
{
    // Clean up all TCP listening sockets owned by this thread
    for(int fd : localListenerFds)
    {
        int domain = 0;
        socklen_t len = sizeof(domain);
        
        // Check the domain natively via the kernel
        if(getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0)
        {
            // Only close network sockets (TCP) created by this thread.
            // Leave the shared UNIX socket descriptor open for other threads/destructor.
            if(domain == AF_INET || domain == AF_INET6)
            {
                ::close(fd);
            }
        }
    }
}

inline int EpollServer::SetupTcpSocket(unsigned short port, int backlog, std::string& errMsg) const
{
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(sock == -1)
    {
        errMsg = "socket(AF_INET) failed: " + std::string(strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if(bind(sock, (sockaddr*)&addr, sizeof(addr)) == -1)
    {
        errMsg = "bind(AF_INET) failed: " + std::string(strerror(errno));
        close(sock);
        return -1;
    }

    if(listen(sock, mBacklog) == -1)
    {
        errMsg = "listen(AF_INET) failed: " + std::string(strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

inline int EpollServer::SetupUnixSocket(const std::string& unixPath, int backlog, bool isAbstract, std::string& errMsg) const
{
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    socklen_t addrLen = 0;

    if(isAbstract)
    {
        addr.sun_path[0] = '\0';    // Leading byte must be '\0
        std::memcpy(addr.sun_path + 1, unixPath.data(), unixPath.size());
        addrLen = offsetof(struct sockaddr_un, sun_path) + 1 + unixPath.size();
    }
    else
    {
        ::unlink(unixPath.c_str());
        std::memcpy(addr.sun_path, unixPath.data(), unixPath.size());
        addrLen = sizeof(struct sockaddr_un);
    }

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(sock == -1)
    {
        errMsg = "socket(AF_UNIX) failed: " + std::string(strerror(errno));
        return -1;
    }

    if(bind(sock, (struct sockaddr*)&addr, addrLen) == -1)
    {
        errMsg = "bind(AF_UNIX) failed: " + std::string(strerror(errno));
        close(sock);
        return -1;
    }

    if(listen(sock, backlog) == -1)
    {
        errMsg = "listen(AF_UNIX) failed: " + std::string(strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

inline bool EpollServer::FlushOutboundBuffer(std::shared_ptr<ClientContext>& client)
{
    auto rr = client->outboundBuffer.GetReadRegions(0, client->outboundBuffer.Size());
    if(rr.count == 0)
    {
        client->wantsWrite = false;
        return true;
    }

    // Note: We need t write until we hit EAGAIN, otherwise the kernel
    // will never drop below the "unwriteable" threshold, meaning epoll
    // will never send you another EPOLLOUT edge trigger. The socket hangs.

    struct iovec iov[2];
    for(int i = 0; i < rr.count; ++i)
    {
        iov[i].iov_base = rr.regions[i].ptr;
        iov[i].iov_len = rr.regions[i].len;
    }

    int iovIdx = 0;
    int iovCount = rr.count;

    while(iovCount > 0)
    {
        struct msghdr msg = {};
        msg.msg_iov = &iov[iovIdx];
        msg.msg_iovlen = iovCount;

        ssize_t sent = sendmsg(client->fd, &msg, MSG_NOSIGNAL);

        if(sent > 0)
        {
            client->outboundBuffer.Consume(sent);

            // Advance our local iovec pointers so we don't recalculate from the RingBuffer
            size_t remaining = static_cast<size_t>(sent);
            while(remaining > 0 && iovCount > 0)
            {
                if(remaining >= iov[iovIdx].iov_len)
                {
                    remaining -= iov[iovIdx].iov_len;
                    iovIdx++;
                    iovCount--;
                }
                else
                {
                    iov[iovIdx].iov_base = static_cast<char*>(iov[iovIdx].iov_base) + remaining;
                    iov[iovIdx].iov_len -= remaining;
                    remaining = 0;
                }
            }
        }
        else if(sent < 0)
        {
            if(errno == EINTR)
            {
                continue; // Interrupted by a system signal; try sending again immediately
            }

            // Check if data is actually left before returning
            client->wantsWrite = (client->outboundBuffer.Size() > 0);

            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return true; // We've consumed what we could, kernel buffer full
            }
            
            return false; // Terminal socket error (EPIPE, ECONNRESET, etc.)
        }
        else
        {
            // sent == 0: Treat as a temporary block or disconnect condition depending on context.
            // Check if data is actually left before returning
            client->wantsWrite = (client->outboundBuffer.Size() > 0);
            return true;
        }
    }

    client->wantsWrite = false; // Successfully emptied the buffer completely!
    return true;
}

inline bool EpollServer::Send(gen::EpollServer::ClientContext* client, const void* data, size_t len)
{
    if(!client || client->fd == -1)
        return false;

    RingBuffer& outboundBuffer = client->outboundBuffer;

    // Check if the RingBuffer has enough contiguous or wrapped space
    if(outboundBuffer.FreeSpace() < len)
        return false;

    outboundBuffer.Write(static_cast<const uint8_t*>(data), len);

    // Signal the epoll loop that this socket has data pending
    client->wantsWrite = true;
    return true;
}

inline EpollServer::RecvStatus EpollServer::Receive(std::shared_ptr<gen::EpollServer::ClientContext>& client)
{
    RingBuffer& inboundBuffer = client->inboundBuffer;
    int fd = client->fd;

    while(true)
    {
        auto wr = inboundBuffer.GetWriteRegions();
        if(wr.count == 0)
        {
            OnError(__FNAME__, __LINE__, "Receive error: Inbound buffer full");
            return RecvStatus::ERROR;
        }

        struct iovec iov[2];
        for(int i = 0; i < wr.count; ++i)
        {
            iov[i].iov_base = wr.regions[i].ptr;
            iov[i].iov_len = wr.regions[i].len;
        }

        ssize_t n = readv(fd, iov, wr.count);

        if(n > 0)
        {
            inboundBuffer.CommitWrite(n);
            // If we filled the requested space, there might be more in the kernel
            if(static_cast<size_t>(n) < (iov[0].iov_len + (wr.count > 1 ? iov[1].iov_len : 0)))
                return RecvStatus::OK;
            continue;
        }
        else if(n == 0)
            return RecvStatus::DISCONNECT;
        else if(errno == EAGAIN || errno == EWOULDBLOCK)
            return RecvStatus::OK;
        else if(errno == EINTR)
            continue;

        OnError(__FNAME__, __LINE__, "Receive error: " + std::string(strerror(errno)));
        return RecvStatus::ERROR;
    }
}

} // namespace gen

#endif  // __EPOLL_SERVER_HPP__
