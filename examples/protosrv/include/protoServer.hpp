#ifndef __PROTO_SERVER_HPP__
#define __PROTO_SERVER_HPP__

#include "epollServer.hpp"
#include "protoCommon.hpp"
#include <google/protobuf/message.h>
#include <cstring>
#include <sstream>
#include <string.h>

namespace gen {

class ProtoServer : public EpollServer
{
public:
    ProtoServer(int threadPoolSize) : EpollServer(threadPoolSize) {}
    ~ProtoServer() override = default;

    // Set the max send/receive message size
    void SetMaxSendMessageSize(size_t size) { SetOutboundBufferSize(size); }
    void SetMaxReceiveMessageSize(size_t size) { SetInboundBufferSize(size); }

protected:
    struct Context
    {
        Context(const std::map<std::string, std::string>& _metadata) : metadata(_metadata) {}
        void SetError(const std::string& err) const { errMsg = err; }
        const std::string& GetError() const { return errMsg; }

        std::string GetMetadata(const char* key) const
        {
            if(auto itr = metadata.find(key); itr != metadata.end()) 
                return std::string(itr->second.data(), itr->second.size());
            return "";
        }

    private:
        const std::map<std::string, std::string>& metadata;
        mutable std::string errMsg;
    };

    template<class SERVER, class REQ, class RESP>
    bool Bind(void (SERVER::*fptr)(const Context& ctx, const REQ&, RESP&))
    {
        std::string reqName = REQ().GetTypeName();
        if(mHandlerMap.find(reqName) != mHandlerMap.end())
        {
            OnError(__FNAME__, __LINE__, "Failed to bind request " + reqName + ": it's already bound");
            return false;
        }
        mHandlerMap[reqName] = std::make_unique<HandlerImpl<SERVER, REQ, RESP>>((SERVER*)this, fptr);
        return true;
    }

private:
    struct Handler
    {
        virtual ~Handler() = default;
        virtual bool Call(const Context& ctx, const std::string& reqData, std::string& respData) = 0;
    };

    template<class SERVER, class REQ, class RESP>
    struct HandlerImpl : public Handler
    {
        typedef void (SERVER::*HANDLER_FPTR)(const Context& ctx, const REQ&, RESP&);
        HandlerImpl(SERVER* _srv, HANDLER_FPTR _fptr) : srv(_srv), fptr(_fptr) {}
        bool Call(const Context& ctx, const std::string& reqData, std::string& respData) override;
        SERVER* srv;
        HANDLER_FPTR fptr;
    };

    struct ClientContextImpl : public ClientContext
    {
        ClientContextImpl(ProtoServer& srv) : ClientContext(srv) {}

        enum class MessageState { READING_CODE=1, READING_LEN, READING_DATA };
        MessageState messageState{MessageState::READING_CODE};
        
        PROTO_CODE currentCode{ACK};
        uint32_t expectedLen{0};

        Handler* handler{nullptr};
        std::string reqName;
        std::string reqData;

        void Reset()
        {
            messageState = MessageState::READING_CODE;
            handler = nullptr;
            reqName.clear();
            reqData.clear();
        }
    };

    std::shared_ptr<ClientContext> MakeClientContext() override final { return std::make_shared<ClientContextImpl>(*this); }
    bool OnDataReceived(std::shared_ptr<ClientContext>& clientIn) override final;

    bool ReceiveUint32(std::shared_ptr<ClientContextImpl>& client, uint32_t& val);
    bool ReceiveString(std::shared_ptr<ClientContextImpl>& client, std::string& str);

    void SendProtoData(std::shared_ptr<ClientContextImpl>& client, PROTO_CODE code, const std::string& data);
    void SendProtoCode(std::shared_ptr<ClientContextImpl>& client, PROTO_CODE code);

    bool HandleFinishedFrame(std::shared_ptr<ClientContextImpl>& client, PROTO_CODE code);
    bool ParseMetadata(const char* buffer, size_t bufferSize,
                       std::map<std::string, std::string>& data, std::string& errMsg);

private:
    // Note: Using std::less<> (Heterogeneous Lookup) allows comparing
    // the view to the string without creating a temporary std::string copy
    std::map<std::string, std::unique_ptr<Handler>, std::less<>> mHandlerMap;
};

inline bool ProtoServer::OnDataReceived(std::shared_ptr<EpollServer::ClientContext>& clientIn)
{
    auto client = std::static_pointer_cast<ClientContextImpl>(clientIn);
    bool result = true;

    while(true)
    {
        if(client->messageState == ClientContextImpl::MessageState::READING_CODE)
        {
            uint32_t code{0};
            if(!ReceiveUint32(client, code))
                break;  // Wait for more data to receive
            client->currentCode = static_cast<PROTO_CODE>(code);
            client->messageState = ClientContextImpl::MessageState::READING_LEN;
        }
        else if(client->messageState == ClientContextImpl::MessageState::READING_LEN)
        {
            uint32_t len{0};
            if(!ReceiveUint32(client, len))
                break; // Wait for more data to receive
            client->expectedLen = len;
            client->messageState = ClientContextImpl::MessageState::READING_DATA;
        }
        else if(client->messageState == ClientContextImpl::MessageState::READING_DATA)
        {
            if(client->GetInboundBuffer().Size() < client->expectedLen)
                break; // Wait for more data to receive

            // Process the frame
            if(!HandleFinishedFrame(client, client->currentCode))
            {
                // Fatal error (e.g. malformed metadata):
                // We return false, EpollServer kills the connection.
                // No need to reset state here; the context is dying anyway.
                result = false;
                break;
            }

            client->messageState = ClientContextImpl::MessageState::READING_CODE;
        }
    }

    return result;
}

inline bool ProtoServer::ReceiveUint32(std::shared_ptr<ClientContextImpl>& client, uint32_t& val)
{
    uint32_t data = 0;
    if(!client->GetInboundBuffer().Read(data))
        return false;

    val = ntohl(data);
    return true;
}

inline bool ProtoServer::ReceiveString(std::shared_ptr<ClientContextImpl>& client, std::string& str)
{
    return client->GetInboundBuffer().Read(str, client->expectedLen);
}

inline bool ProtoServer::HandleFinishedFrame(std::shared_ptr<ClientContextImpl>& client, PROTO_CODE code)
{
    auto views = client->GetInboundBuffer().PeekAsViews(client->expectedLen);
    if(views.TotalSize() < client->expectedLen)
        return false; // This function should only be called when data is available

    std::string_view data;
    std::string tmp;

    if(views.IsContiguous())
    {
        // The single string_view has all the data
        data = views.first;
    } 
    else
    {
        // Fallback: The data is split - copy into a temporary buffer/string to have contiguity.
        tmp.reserve(views.first.size() + views.second.size()); // Pre-allocate memory
        tmp.append(views.first);
        tmp.append(views.second);
        data = tmp;
    }

    // Remove the data from the RingBuffer once processed
    client->GetInboundBuffer().Consume(views.TotalSize());

    bool result = true;

    if(code == PROTO_CODE::REQ_NAME)
    {
        auto itr = mHandlerMap.find(data);
        if(itr != mHandlerMap.end())
        {
            client->reqName = std::move(data);
            client->handler = itr->second.get();
            SendProtoCode(client, PROTO_CODE::ACK);
        }
        else
        {
            SendProtoCode(client, PROTO_CODE::NACK);
            SendProtoData(client, PROTO_CODE::ERR, "Unknown request: " + std::string(data));
            client->Reset();
        }
    }
    else if(code == PROTO_CODE::REQ)
    {
        client->reqData = std::move(data);
    }
    else if(code == PROTO_CODE::METADATA)
    {
        std::map<std::string, std::string> metadata;
        std::string errMsg;
        if(ParseMetadata(data.data(), data.size(), metadata, errMsg))
        {
            if(client->handler)
            {
                std::string respData;
                Context ctx(metadata);
                client->handler->Call(ctx, client->reqData, respData);
                SendProtoData(client, PROTO_CODE::RESP, respData);
                SendProtoData(client, PROTO_CODE::ERR, ctx.GetError());
            }
        }
        else
        {
            // If parsing fails, the client sent garbage.
            OnError(__FNAME__, __LINE__, "Failed parsing metadata for request '" + client->reqName + "'");
            result = false;
        }
        client->Reset();
    }
    else
    {
        OnError(__FNAME__, __LINE__, "Invalid PROTO_CODE " + std::to_string(static_cast<long>(code)));
        result = false;
    }

    //data.clear(); // It could be already empty after std::move
    return result;
}

inline void ProtoServer::SendProtoCode(std::shared_ptr<ClientContextImpl>& client, PROTO_CODE code)
{
    uint32_t val = htonl(static_cast<uint32_t>(code));
    client->Send(&val, sizeof(val));
}

inline void ProtoServer::SendProtoData(std::shared_ptr<ClientContextImpl>& client, PROTO_CODE code, const std::string& data)
{
    SendProtoCode(client, code);
    uint32_t len = htonl(static_cast<uint32_t>(data.length()));
    client->Send(&len, sizeof(len));
    if(data.length() > 0)
        client->Send(data.data(), data.length());
}

template<class SERVER, class REQ, class RESP>
bool ProtoServer::HandlerImpl<SERVER, REQ, RESP>::Call(const ProtoServer::Context& ctx, const std::string& reqData, std::string& respData)
{
    REQ req;
    if(!req.ParseFromString(reqData)) 
        return false;
    RESP resp;
    (srv->*fptr)(ctx, req, resp);
    return resp.SerializeToString(&respData);
}

inline bool ProtoServer::ParseMetadata(const char* buffer, size_t bufferSize,
                                       std::map<std::string, std::string>& metadata, 
                                       std::string& errMsg)
{
    size_t offset = 0;
    auto CheckBuffer = [&](size_t needed) -> bool
    {
        if(offset + needed > bufferSize)
        {
            std::stringstream ss;
            ss << __FNAME__ << ":" << __LINE__ << " Unexpected end of buffer";
            errMsg = ss.str();
            return false;
        }
        return true;
    };

    if(!CheckBuffer(sizeof(uint32_t))) 
        return false;
    
    uint32_t sizeNetwork;
    std::memcpy(&sizeNetwork, buffer + offset, sizeof(sizeNetwork));
    uint32_t sizeHost = ntohl(sizeNetwork);
    offset += sizeof(sizeNetwork);

    std::map<std::string, std::string> strmap;
    for(uint32_t i = 0; i < sizeHost; ++i)
    {
        if(!CheckBuffer(sizeof(uint32_t))) 
            return false;
        
        uint32_t kLen;
        std::memcpy(&kLen, buffer + offset, sizeof(uint32_t));
        kLen = ntohl(kLen);
        offset += sizeof(uint32_t);

        if(!CheckBuffer(kLen)) 
            return false;
        
        std::string key(buffer + offset, kLen);
        offset += kLen;

        if(!CheckBuffer(sizeof(uint32_t))) 
            return false;
        
        uint32_t vLen;
        std::memcpy(&vLen, buffer + offset, sizeof(uint32_t));
        vLen = ntohl(vLen);
        offset += sizeof(uint32_t);

        if(!CheckBuffer(vLen)) 
            return false;
        
        std::string value(buffer + offset, vLen);
        offset += vLen;

        strmap[key] = value;
    }

    metadata = std::move(strmap);
    return true;
}

} // namespace gen

#endif // __PROTO_SERVER_HPP__