//
// server.cpp
//
#include "epollServer.hpp"
#include "protoServer.hpp"
#include "hello.pb.h"
#include <signal.h>
#include <future>

// Handler for SIGHUP, SIGINT, SIGQUIT and SIGTERM
volatile static sig_atomic_t gSignalNumber{0};

int Signal(int signum, void (*handler)(int))
{
    struct sigaction sa, old_sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // want interrupted system calls to be restarted
    return sigaction(signum, &sa, &old_sa);
}

extern "C"
void HandlerExitSignal(int signalNumber)
{
    // Once we are in this handler, block all the signals that trigger this handler
    sigset_t blockSignals;
    sigemptyset(&blockSignals);
    sigaddset(&blockSignals, SIGHUP);
    sigaddset(&blockSignals, SIGINT);
    sigaddset(&blockSignals, SIGQUIT);
    sigaddset(&blockSignals, SIGTERM);
    sigprocmask(SIG_BLOCK, &blockSignals, nullptr);

    const char* msg = "Got a signal\n";
    write(STDOUT_FILENO, msg, strlen(msg));

    gSignalNumber = signalNumber;
}

class MyServer : public gen::ProtoServer
{
public:
    MyServer(size_t threadsCount) : gen::ProtoServer(threadsCount) {}
    MyServer() = delete;
    virtual ~MyServer() = default;

    bool Start(unsigned short port)
    {
        // Run EpollServer::Start in a different thread (async)
        std::future<bool> future = std::async(std::launch::async,
                static_cast<bool (gen::EpollServer::*)(short unsigned int)>(&gen::EpollServer::Start),
                this, port);

        return StartImpl(std::move(future));   // Pass 'future' by value (move ownership)
    }

    bool Start(const char* sockName, bool isAbstract)
    {
        // Run EpollServer::Start in a different thread (async)
        std::future<bool> future = std::async(std::launch::async,
                static_cast<bool (gen::EpollServer::*)(const char*, bool)>(&gen::EpollServer::Start),
                this, sockName, isAbstract);

        return StartImpl(std::move(future));   // Pass 'future' by value (move ownership)
    }

private:
    virtual bool OnInit() override
    {
        Bind(&MyServer::OnPing);
        return true;
    }

    virtual void OnError(const char* fname, int lineNum, const std::string& err) const override
    {
        std::cerr << fname << ":" << lineNum << " " << err << std::endl;
//        ERRORMSG(fname << ":" << lineNum << " " << err);
    }

    virtual void OnInfo(const char* fname, int lineNum, const std::string& info) const override
    {
        std::cout << fname << ":" << lineNum << " " << info << std::endl;
//        OUTMSG(fname << ":" << lineNum << " " << info);
    }

    bool StartImpl(std::future<bool> future)
    {
        while(true)
        {
            if(gSignalNumber != 0)
            {
                // We got a signal. Exiting...
                std::cout << __FNAME__<< ":" << __LINE__ << " Got a signal " << gSignalNumber << ", exiting..." << std::endl;
                Stop();
                break;
            }
            else
            {
                // Check if gen::ProtoServer::Start() is still running
                auto status = future.wait_for(std::chrono::seconds(1));
                if(status == std::future_status::ready)
                    break; // gen::ProtoServer::Start() no longer running or failed to start
            }
        }

        return future.get();
    }

    void OnPing(const Context& ctx,
                const test::PingRequest& req,
                test::PingResponse& resp)
    {
//        const std::string sessionId = ctx.GetMetadata("sessionId");
//        const std::string requestId = ctx.GetMetadata("reportId");
//
//        std::cout << __func__
//                  << ": sessionId='" << sessionId << "'"
//                  << ", requestId='" << requestId << "'"
//                  << ", req=" << req.from() << std::endl;

        resp.set_msg("Pong");
    }
};

int main()
{
    // Writing to an unconnected socket will cause a process to receive a SIGPIPE
    // signal. We don't want to die if this happens, so we ignore SIGPIPE.
    Signal(SIGPIPE, SIG_IGN);

    // Let the kernel know that we want to handle exit signals
    Signal(SIGHUP,  HandlerExitSignal);
    Signal(SIGINT,  HandlerExitSignal);
    Signal(SIGQUIT, HandlerExitSignal);
    Signal(SIGTERM, HandlerExitSignal);

//    MyServer server(std::thread::hardware_concurrency());
    MyServer server(8);
//    server.SetVerbose(true);
//    if(!server.Start(8080))
    if(!server.Start("protoserver_domain_socket.sock", true))
    {
        std::cerr << "Failed to start the epoll server." << std::endl;
        return 1;
    }

    return 0;
}

