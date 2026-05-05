#ifndef __HASH_SERVER_HPP__
#define __HASH_SERVER_HPP__

#include "epollServer.hpp"
#include "hasher.hpp"

class HashServer : public gen::EpollServer
{
public:
    HashServer(size_t threadsCount) : gen::EpollServer(threadsCount) {}
    ~HashServer() override = default;

private:
    struct ClientContextImpl : public ClientContext
    {
        Hasher hasher;
    };

    std::shared_ptr<gen::EpollServer::ClientContext> MakeClientContext() override { return std::make_shared<ClientContextImpl>(); }
    
    // Triggered when new data is available in client->inboundBuffer
    bool OnDataReceived(std::shared_ptr<ClientContext>& clientIn) override;
 };

inline bool HashServer::OnDataReceived(std::shared_ptr<ClientContext>& clientIn)
{
    auto client = std::static_pointer_cast<ClientContextImpl>(clientIn);
    size_t processedIdx = 0;

    // Scan the inbound buffer provided by the base class
    for(size_t i = 0; i < client->inboundBuffer.size(); ++i)
    {
        if(client->inboundBuffer[i] == '\n')
        {
            size_t dataLen = i - processedIdx;
            
            // Handle CRLF (Windows style line endings)
            if(dataLen > 0 && client->inboundBuffer[i - 1] == '\r')
                dataLen--;

            // Process the line content
            if(dataLen > 0)
                client->hasher.Update(&client->inboundBuffer[processedIdx], dataLen);

            // Get the response
            size_t len = 0;
            const char* hex = client->hasher.FinalizeHex(len);

            if(!client->Send(hex, len))
                return false;

            processedIdx = i + 1;
        }
    }

    // Clean up processed data so the buffer only contains partial lines
    if(processedIdx > 0)
        client->inboundBuffer.erase(client->inboundBuffer.begin(), client->inboundBuffer.begin() + processedIdx);

    return true;
}

#endif // __HASH_SERVER_HPP__

