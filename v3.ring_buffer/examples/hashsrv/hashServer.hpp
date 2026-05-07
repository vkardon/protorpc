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
        ClientContextImpl(EpollServer& srv) : ClientContext(srv) {}
        Hasher hasher;
    };

    std::shared_ptr<gen::EpollServer::ClientContext> MakeClientContext() override 
    { 
        return std::make_shared<ClientContextImpl>(*this); 
    }
    
    // Triggered when new data is available in client->inboundBuffer
    bool OnDataReceived(std::shared_ptr<ClientContext>& clientIn) override;
};

inline bool HashServer::OnDataReceived(std::shared_ptr<ClientContext>& clientIn)
{
    auto client = std::static_pointer_cast<ClientContextImpl>(clientIn);
    gen::RingBuffer& inboundBuffer = client->GetInboundBuffer();

    while(inboundBuffer.Size() > 0)
    {
        int newlineIdx = -1;
        for(size_t i = 0; i < inboundBuffer.Size(); ++i)
        {
            if(inboundBuffer[i] == '\n')
            {
                newlineIdx = static_cast<int>(i);
                break;
            }
        }

        if(newlineIdx == -1)
            break;

        size_t totalBytesToConsume = newlineIdx + 1; // We need to consume '\n' too
        size_t remainingDataToHash = newlineIdx;     // We only want to hash up to '\n'

        // Handle CRLF
        if(remainingDataToHash > 0 && inboundBuffer[remainingDataToHash - 1] == '\r')
            remainingDataToHash--;

        // Processes text and removes text + \n
        client->ConsumeReceived(totalBytesToConsume, [&](const uint8_t* ptr, size_t len) 
            {
                if(remainingDataToHash > 0)
                {
                    // Only hash the portion of this segment that is actual data
                    size_t toHash = std::min(len, remainingDataToHash);
                    client->hasher.Update(ptr, toHash);
                    remainingDataToHash -= toHash;
                }
            });

        size_t resLen = 0;
        const char* hex = client->hasher.FinalizeHex(resLen);
        client->Send(hex, resLen);
    }

    return true;
}

#endif // __HASH_SERVER_HPP__

