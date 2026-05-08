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

    while(true)
    {
        // Search for '\n'
        ssize_t newlineIdx = inboundBuffer.Find('\n');
        if(newlineIdx == -1)
            break;

        size_t totalBytesToConsume = newlineIdx + 1;
        size_t dataToHash = newlineIdx;

        // Handle CRLF check using operator[] (it's fine for single checks)
        if(dataToHash > 0 && inboundBuffer[dataToHash - 1] == '\r')
        {
            dataToHash--;
        }

        // Use GetReadRegions to feed the hasher zero-copy chunks
        auto rr = inboundBuffer.GetReadRegions(0, dataToHash);
        for(int i = 0; i < rr.count; ++i)
        {
            client->hasher.Update(rr.regions[i].ptr, rr.regions[i].len);
        }

        // Clean up the buffer (including the \n)
        inboundBuffer.Consume(totalBytesToConsume);

        // Finalize and Send
        size_t resLen = 0;
        const char* hex = client->hasher.FinalizeHex(resLen);
        client->Send(hex, resLen);
    }

    return true;
}

#endif // __HASH_SERVER_HPP__

