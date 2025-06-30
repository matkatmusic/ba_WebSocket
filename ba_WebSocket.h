/***********************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:               ba_WebSocket
  vendor:           Benedikt Adams
  version:          1.0.0
  name:             BA Web Socket
  description:      A WebSocket protocol implementation based on juce::StreamingSocket
  website:
  license:

  dependencies:     juce_core juce_events juce_data_structures
  OSXFrameworks:
  iOSFrameworks:
  linuxLibs:
  mingwLibs:

 END_JUCE_MODULE_DECLARATION
**********************************************************************************/
#pragma once

#include "JuceIncludes.h"
#include "RingBuffer.h"

#define WEBSOCKET_ECHO_INCOMING_MESSAGES 0
 
namespace BA
{

class WebSocketServer;
class WebSocketQueueSender : private Thread 
{
public:
    WebSocketQueueSender (WebSocketServer& server);
    ~WebSocketQueueSender() override;

private:
    WebSocketServer& server;

    void run() override;

    JUCE_DECLARE_WEAK_REFERENCEABLE (WebSocketQueueSender)
};

class WebSocketServer : private juce::Thread
{
public:
    using ServerStartupCallback = std::function<void(bool openedOk, const int port)>;
    using ServerFunction = std::pair<String, std::function<void (const Array<var>&)>>;
    struct ServerFunctionProvider : public Array<ServerFunction> {
        ServerFunctionProvider operator+ (const ServerFunction& f) { add (f); return *this; }
        ServerFunctionProvider operator+ (const ServerFunctionProvider& f) {  addArray (f); return *this;  }
        ServerFunctionProvider operator<< (const ServerFunction& f) {  add (f); return *this;  }
        ServerFunctionProvider operator<< (const ServerFunctionProvider& f) {  addArray (f); return *this;  }
    };
    
    explicit WebSocketServer(ServerStartupCallback onServerStarted = nullptr, const ServerFunctionProvider& ServerFunctionProvider = {});
    ~WebSocketServer() override;
    
    static String MessageFromBufferSafe (const MemoryBlock& b);

    void startServer();
    void stopServer();
    bool isRunning() const;

    void registerFunction (ServerFunction callback);
    void deregisterFunction (const String& identifier);

    //async, no completion callback right now
    bool sendMessage(const juce::MemoryBlock& data);
    bool sendMessage(const juce::String& data);
    bool sendMessageCompressed (const juce::MemoryBlock& data);

    struct Listener {
        virtual ~Listener() = default;
        virtual void messageReceived(const var& message) {};
    };
    void addListener(Listener* listener);
    void removeListener(Listener* listener);

private:
    friend class WebSocketQueueSender;

    ServerStartupCallback onServerStarted;
    void run() override;
    void handleClient(juce::StreamingSocket* clientSocket);

    std::unique_ptr<juce::StreamingSocket> serverSocket;
    std::unique_ptr<juce::StreamingSocket> clientSocket;
    CriticalSection clientSocketLock;
    
    int port = -1;
    const Range<int> legalPortRange { 8001, 65535 };
    std::atomic<bool> running { false };

    bool handleHandshake(juce::StreamingSocket* clientSocket);
    juce::String computeWebSocketAcceptKey(const juce::String& key);
    juce::MemoryBlock buffer;

    Atomic<bool> currentlyWaitingForConnection = false;

    bool sendWebSocketMessage(const juce::MemoryBlock& data);

    RingBuffer incomingMessages;
    RingBuffer outgoingMessages;

    void processIncomingMessages();
    bool processOutgoingMessages(MemoryBlock& processingBuffer);

    MemoryBlock formatWebSocketFrame (const MemoryBlock& input, bool isTextFrame);
    void appendLengthBytes (size_t length, MemoryBlock& frame);
    bool sendFrame (const MemoryBlock& frame);

    struct Heartbeat {
        Atomic<bool> receivedResponse = false;
        Time lastHeartbeat;
        const int interval = 1000;
    };
    std::unique_ptr<Heartbeat> heartbeat;
    const int maxPongsMissedBeforeDisconnect = 3;
    int pongsMissed = 0;

    bool processHeartbeat (const MemoryBlock& message);
    bool checkHeartbeat ();

    const int findOpenAndFreePort () noexcept;

    const bool bufferContainsOnlyUtf8 (const MemoryBlock& buffer) const;

    struct ForceRestart {
        int count = 0;
        const int maxCount = 5;
    };
    std::unique_ptr<ForceRestart> forceRestart;

    WebSocketQueueSender queueSender { *this}; 

    ListenerList<Listener, Array<Listener*, CriticalSection>> listeners;

    bool processFunctionCall (const var& message);

    CriticalSection serverFunctionProviderLock;
    std::map<String, ServerFunction> serverFunctionProvider;

    JUCE_DECLARE_WEAK_REFERENCEABLE (WebSocketServer)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebSocketServer)
};


}; // namespace BA
