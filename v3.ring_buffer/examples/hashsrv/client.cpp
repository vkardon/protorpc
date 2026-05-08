//
// client.cpp
//
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include "utils.hpp"

class EchoClient
{
public:
    EchoClient(const std::string& host, int port) : mHost(host), mPort(port), mSock(-1) {}
    ~EchoClient() 
    {
        if(mSock != -1) 
            close(mSock);
    }

    bool Connect() 
    {
        struct hostent* server = gethostbyname(mHost.c_str());
        if (server == nullptr) 
        {
            std::cerr << "Error: Could not resolve hostname " << mHost << std::endl;
            return false;
        }

        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(mPort);
        memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);

        int retries = 5;
        while(retries > 0) 
        {
            mSock = socket(AF_INET, SOCK_STREAM, 0);
            if(mSock < 0) 
            {
                std::cerr << "Failed to create socket: " << std::strerror(errno) << std::endl;
                return false;
            }

            if(connect(mSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == 0) 
            {
                return true; // Success!            
            }

            // If we failed, clean up and wait a bit
            close(mSock);
            mSock = -1;
            retries--;

            if(retries > 0) 
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            else
            {
                std::cerr << "Connection failed to " 
                          << inet_ntoa(serverAddr.sin_addr) << ":" << ntohs(serverAddr.sin_port) 
                          << ", Error: " << std::strerror(errno) << std::endl;            
            }
        }

        // If we get here, all retries failed
        return false;
    }

    bool Call(const std::string& msg, std::string& response) 
    {
        // Newline termination required by EchoServer::OnRead
        std::string request = msg + "\n";
        
        if(send(mSock, request.c_str(), request.length(), 0) < 0) 
        {
            std::cerr << "Failed to send data: " << std::strerror(errno) << std::endl;
            return false;
        }

        char buffer[4096]{0};
        ssize_t bytesRead = recv(mSock, buffer, sizeof(buffer) - 1, 0);

        if(bytesRead < 0)
        {
            // A true error occurred
            std::cerr << "Receive error: " << std::strerror(errno) << std::endl;
            return false;
        } 
        else if(bytesRead == 0)
        {
            // The peer closed the connection gracefully
            std::cout << "Connection closed by peer." << std::endl;
            return false;
        }

        if(buffer[bytesRead - 1] != '\n')
        {
            // The response is not newline-teminated.
            // Cast to int to see the ASCII value, especially if it's a non-printable character
            std::cerr << "Protocol error: Response not newline-terminated. "
                      << "Last byte received: 0x" << std::hex << static_cast<int>(static_cast<unsigned char>(buffer[bytesRead - 1]))
                      << std::dec << " ('" << buffer[bytesRead - 1] << "')" << std::endl;
            return false;
        }

        buffer[bytesRead-1] = '\0'; // Drop the newline
        response = std::string(buffer);
        return true;
    }

private:
    std::string mHost;
    unsigned int mPort{0};
    int mSock{0};
};

void RunTest(const std::string& host, unsigned short port, 
             int numThreads, int numOfCallsPerThread)
{
    // Create and start multiple threads
    std::vector<std::thread> threads;

    for(int i = 0; i < numThreads; ++i)
    {
        // A small delay every few threads or even every thread
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        threads.emplace_back([host, port, i, numOfCallsPerThread]()
        {
            EchoClient client(host, port);
            if(!client.Connect()) 
                return;

            for(int j = 0; j < numOfCallsPerThread; j++)
            {
                std::string req = "Thread " + std::to_string(i) + " call " + std::to_string(j);
                std::string resp;

                if(client.Call(req, resp)) 
                {
                    // Silence output in stress test to avoid console lag
                    //std::cout << "[" << i << "] Req: '" << req << "', Resp: '" << resp << "'" << std::endl;
                }
            }
        });
    }

    for(auto& thread : threads)
        thread.join();
}

void Usage(const char* progname)
{
    std::cerr << "Usage: " << progname << " <hostname/ip>:<port>" << std::endl;
    std::cerr << "Example: " << progname << " 127.0.0.1:8080" << std::endl;
}

int main(int argc, char* argv[])
{
    // Check if we have exactly 2 arguments (prog_name host port)
    if(argc < 2) 
    {
        Usage(argv[0]);
        return 1;
    }

    std::string errMsg;
    std::string host;
    unsigned short port = gen::ParseHostPort(argv[1], host, errMsg);
    if (port == 0)
    {
        std::cerr << "Error: " << errMsg << std::endl;
        Usage(argv[0]);
        return 1;
    }

    // Writing to an unconnected socket will cause a process to receive a SIGPIPE
    // signal. We don't want to die if this happens, so we ignore SIGPIPE.
    signal(SIGPIPE, SIG_IGN);

    int numOfThreadsPerRun = 500;
    int numOfCallsPerThread = 500;
    int numOfRuns = 10;

    // int numOfThreadsPerRun = 5;
    // int numOfCallsPerThread = 5;
    // int numOfRuns = 1;

    std::cout << "Running:\n"
            << "  Number of clients          : " << numOfThreadsPerRun << "\n"
            << "  Number of calls per client : " << numOfCallsPerThread << "\n"
            << "  Number of runs             : " << numOfRuns << std::endl;
    
    {
        gen::StopWatch sw;

        for(int i = 0; i < numOfRuns; i++)
        {
            std::cout << "Run " << i << std::endl;
            RunTest(host, port, numOfThreadsPerRun, numOfCallsPerThread);
        }
    }

    std::cout << "Done:\n"
            << "  Number of threads          : " << numOfThreadsPerRun << "\n"
            << "  Number of calls per thread : " << numOfCallsPerThread << "\n"
            << "  Number of runs             : " << numOfRuns << std::endl;

    return 0;
}
