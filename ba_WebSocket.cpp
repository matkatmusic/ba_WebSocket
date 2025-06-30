#include "RingBuffer.cpp"

#include "ba_WebSocket.h"
#include "TinySHA1/TinySHA1.hpp"
#if JUCE_WINDOWS
#include <WinSock2.h>
#endif

using namespace BA;

WebSocketServer::WebSocketServer (ServerStartupCallback startupCb, const ServerFunctionProvider& frontendCallbacks)
: 
Thread("WebSocketServerWorker"),
onServerStarted (startupCb)
{
    for (auto& cb : frontendCallbacks)
        registerFunction (cb);

    startServer();
}

WebSocketServer::~WebSocketServer()
{
    stopServer();
}

void WebSocketServer::startServer()
{
    running = true;
    startThread();
}

void WebSocketServer::stopServer()
{
    running = false;
    stopThread(4000);

    heartbeat.reset ();
    clientSocket.reset();
    serverSocket.reset();
}

bool WebSocketServer::isRunning() const
{
    return running;
}

const int WebSocketServer::findOpenAndFreePort() noexcept
{
    juce::StreamingSocket s;
    for (int port = legalPortRange.getStart (); port <= legalPortRange.getEnd (); ++port) // Iterate over a range of ports
    {
        if (s.createListener(port)) // Try to create a listener on the port
        {
            s.close(); // Close the socket
            return port; // If successful, return the port
        }
    }

    return 0; // If no free port is found, return 0
}

void WebSocketServer::run() {

    auto portIsLegal = legalPortRange.contains (port);
    if ( ! portIsLegal)
    {
        port = findOpenAndFreePort ();
        portIsLegal = legalPortRange.contains (port);
    }

    jassert (port > 0);

    MessageManager::callAsync ([weak = WeakReference<WebSocketServer> (this), portIsLegal] () {
        if (weak.get () != nullptr && weak->onServerStarted != nullptr)
            weak->onServerStarted (portIsLegal, weak->port);
    });

    if (!portIsLegal) {
        DBG("Failed to find an open port.");
        running = false;
        return;
    }

    while (!threadShouldExit ())
    {
        serverSocket.reset(new juce::StreamingSocket());
        if (serverSocket->createListener(port)) {
            DBG("Server is listening on port " + juce::String(port));
            break;
        } else {
            DBG("Failed to open socket on port " + juce::String(port));
            
            do {
                port = findOpenAndFreePort ();
            } while (!legalPortRange.contains (port));

            DBG("Switching to port " + juce::String(port) + "...");
            running = false;
            currentlyWaitingForConnection.set (true);
            Thread::sleep (400);
            continue;
        }
    }

    while (!threadShouldExit()) {
        clientSocket.reset ();
        heartbeat.reset ();
        DBG("Waiting for connection...");
        currentlyWaitingForConnection.set (true);
        std::unique_ptr<juce::StreamingSocket> cs (serverSocket->waitForNextConnection());
        if (cs != nullptr && !threadShouldExit()) 
        {
            DBG("Client connected");
            if (handleHandshake(cs.get())) 
            {
                clientSocket.reset (cs.release());
                heartbeat = std::make_unique<Heartbeat> ();

                currentlyWaitingForConnection.set (false);  
                handleClient (clientSocket.get ());  // Handle client communication after successful handshake
            }
            else 
            {
                DBG("Failed to handshake with client");
                cs->close();
            }
        }

        Thread::sleep(500);
    }
   
    if (!threadShouldExit())
        jassertfalse; // Should never reach this point
}



void WebSocketServer::handleClient(juce::StreamingSocket* clientSocket) {

    MemoryBlock buffer;
    while (clientSocket->isConnected() && !threadShouldExit()) {
        if (clientSocket->waitUntilReady(true, 100) > 0) {
            // Read the first two bytes to get basic frame headers
            unsigned char header[2];
            if (clientSocket->read(header, 2, true) != 2) {
                DBG("Error reading header.");
                break;
            }

            bool fin = (header[0] & 0x80) != 0;
            unsigned int opcode = header[0] & 0x0F;
            bool mask = (header[1] & 0x80) != 0;
            unsigned int payloadLen = header[1] & 0x7F;
            unsigned long long fullPayloadLength = payloadLen;

            if (payloadLen == 126) {
                unsigned char extendedPayload[2];
                if (clientSocket->read(extendedPayload, 2, true) != 2) {
                    DBG("Error reading extended payload length.");
                    break;
                }
                fullPayloadLength = (extendedPayload[0] << 8) + extendedPayload[1];
            } else if (payloadLen == 127) {
                unsigned char extendedPayload[8];
                if (clientSocket->read(extendedPayload, 8, true) != 8) {
                    DBG("Error reading extended payload length.");
                    break;
                }
                fullPayloadLength = 0;
                for (int i = 0; i < 8; ++i) {
                    fullPayloadLength = (fullPayloadLength << 8) + extendedPayload[i];
                }
            }

            // Read mask key if mask is set
            unsigned char maskKey[4];
            if (mask) {
                if (clientSocket->read(maskKey, 4, true) != 4) {
                    DBG("Error reading mask key.");
                    break;
                }
            }

            // Read the actual payload data
            juce::MemoryBlock payloadData;
            payloadData.setSize(static_cast<size_t>(fullPayloadLength), true);
            if (clientSocket->read(payloadData.getData(), static_cast<int>(fullPayloadLength), true) != static_cast<int>(fullPayloadLength)) {
                DBG("Error reading payload data.");
                break;
            }

            // Unmask data if necessary
            if (mask) {
                unsigned char* payloadBytes = static_cast<unsigned char*>(payloadData.getData());
                for (size_t i = 0; i < fullPayloadLength; ++i) {
                    payloadBytes[i] ^= maskKey[i % 4];
                }
            }

           
            // Handle different types of frames
            if (opcode == 0x8) {  // Close frame
                DBG("Connection closed by client.");
                break;  // Optionally send a close frame back
            } else if (!fin) {  // Handle fragmentation
                buffer.append(payloadData.getData(), payloadData.getSize());
                continue; 
            } else {
                buffer.append(payloadData.getData(), payloadData.getSize());
                // DBG("Received data: " + buffer.toString());
                incomingMessages.push(buffer);

                buffer.setSize(0);  // Clear the buffer for the next message
            }
      
        } else if (!clientSocket->isConnected()) {
            DBG("Client disconnected");
            break;
        }
    
        processIncomingMessages ();
    }

    const ScopedLock sl (clientSocketLock);
    if (clientSocket != nullptr && clientSocket->isConnected ()) clientSocket->close();  // Ensure the socket is closed on exit
}

static var deserializeMessage(const MemoryBlock& message) {
    DynamicObject* obj = new DynamicObject();
    const char* data = static_cast<const char*>(message.getData());
    size_t offset = 0;
    int argIndex = 0;  

    // Read the length of the function name
    uint16 functionNameLength = *reinterpret_cast<const uint16*>(data + offset);
    offset += sizeof(uint16);

    // Read the function name
    String functionName(data + offset, functionNameLength);
    offset += functionNameLength;
    obj->setProperty("functionName", functionName);

    while (offset < message.getSize()) {
        uint8 dataType = *reinterpret_cast<const uint8*>(data + offset);
        offset += sizeof(uint8);
        String argKey = "arg" + String(argIndex++);  // Create argument key like arg1, arg2, ...

        switch (dataType) {
            case 0x01: { // 'number'
                double number = *reinterpret_cast<const double*>(data + offset);
                offset += sizeof(double);
                obj->setProperty(argKey, number);
                break;
            }
            case 0x02: { // 'boolean'
                bool boolean = *reinterpret_cast<const bool*>(data + offset);
                offset += sizeof(bool);
                obj->setProperty(argKey, boolean);
                break;
            }
            case 0x03:  // 'arraybuffer'
            case 0x04:  // 'uint8array'
            case 0x05: { // 'object'
                uint32 bufferLength = *reinterpret_cast<const uint32*>(data + offset);
                offset += sizeof(uint32);
                MemoryBlock buffer(data + offset, bufferLength);
                offset += bufferLength;
                obj->setProperty(argKey, var(buffer));
                break;
            }
            case 0x06: { // 'string'
                uint16 stringLength = *reinterpret_cast<const uint16*>(data + offset);
                offset += sizeof(uint16);
                String string(data + offset, stringLength);
                offset += stringLength;
                obj->setProperty(argKey, string);
                break;
            }
            case 0x07: { // 'null'
                obj->setProperty(argKey, var());  // Represent null
                break;
            }
            default: {
                std::cerr << "Unexpected data type code: " << static_cast<int>(dataType) << std::endl;
                jassertfalse; // Unknown data type
                return var();
            }
        }
    }

    return var(obj);
}

static String getTypeString(const var& value) {
    if (value.isVoid()) {
        return "Void";
    } else if (value.isUndefined()) {
        return "Undefined";
    } else if (value.isInt()) {
        return "Integer";
    } else if (value.isInt64()) {
        return "Integer64";
    } else if (value.isBool()) {
        return "Boolean";
    } else if (value.isDouble()) {
        return "Double";
    } else if (value.isString()) {
        return "String";
    } else if (value.isObject()) {
        return "Object";
    } else if (value.isArray()) {
        return "Array";
    } else if (value.isBinaryData()) {
        return "BinaryData";
    } else if (value.isMethod()) {
        return "Method";
    } else {
        return "Unknown";
    }
}

void WebSocketServer::processIncomingMessages ()
{
    processHeartbeat ({"generate", 8}); //places a heartbeat every second, 8byte message for 'generate' word in Memory Block

    MemoryBlock buffer;
    while (incomingMessages.pop (buffer) && !threadShouldExit ())
    {
        #if WEBSOCKET_ECHO_INCOMING_MESSAGES
            DBG ("Received message: " + MessageFromBufferSafe (buffer));
            Thread::launch ([weak = WeakReference<WebSocketServer> (this), b = buffer] {
                if (auto that = weak.get())
                    that->sendMessage (WebSocketServer::MessageFromBufferSafe (b));
            });
        #endif

        if (! processHeartbeat (buffer)) continue; //means we don't forward heartbeats

        const var message = deserializeMessage(buffer);
        
        if ( ! processFunctionCall (message) )
            listeners.call ([&] (Listener& l) { l.messageReceived (message); });
    }
}

void WebSocketServer::registerFunction (ServerFunction callback)
{
    const ScopedLock sl { serverFunctionProviderLock };
    serverFunctionProvider[callback.first] = callback;
}

void WebSocketServer::deregisterFunction (const String& identifier)
{
    const ScopedLock sl { serverFunctionProviderLock };
    serverFunctionProvider.erase (identifier);
}

bool WebSocketServer::processFunctionCall (const var& message)
{
    static constexpr auto functionNameKey = "functionName";
    
    if (message.hasProperty (functionNameKey))
    {
        static constexpr auto functionArgKey = "arg";

        const auto functionName = message.getProperty (functionNameKey, {}).toString ();

        const auto findFrontendFunction = [&]() -> const ServerFunction
        {
            const ScopedLock sl { serverFunctionProviderLock };
            auto it = serverFunctionProvider.find (functionName);
            if (it != serverFunctionProvider.end ())
            {
                const auto function = it->second; 
                return function;
            }

            return {};
        };
        

        if (const auto function = findFrontendFunction ();
            function.first.isNotEmpty () && function.second != nullptr)
        {
            jassert (function.first == functionName);

            Array<var> args;
            for (int i = 0; message.hasProperty (functionArgKey + String (i)); ++i)
            {
                args.add (message.getProperty (functionArgKey + String (i), {}));
            }
            const ScopedLock sl { serverFunctionProviderLock };
            function.second (args);
            return true;
        }
        else 
        {
            DBG("Websocket: frontend function not found: " + functionName + "... maybe not registered with this server yet?");
            jassertfalse;
        }
    }

    return false;
}

void WebSocketServer::addListener(Listener* listener)
{
    listeners.add(listener);
}

void WebSocketServer::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

bool WebSocketServer::processOutgoingMessages (MemoryBlock& processingBuffer)
{
    bool processedAny = false;
    while (outgoingMessages.pop (processingBuffer) && ! threadShouldExit ())
    {
        processedAny = true;
        if (processHeartbeat (processingBuffer)) // only going forward if the heartbeat is fine
        {
            sendWebSocketMessage (processingBuffer);
        }
    }

    return processedAny;
}

const bool WebSocketServer::bufferContainsOnlyUtf8 (const MemoryBlock& buffer) const
{
    for (int i = 0; i < jmin ((size_t)50, buffer.getSize ()); ++i)
    {
        if (buffer[i] == 0)
            return false;
    }

    return true;
}

bool WebSocketServer::checkHeartbeat ()
{
    jassert (heartbeat != nullptr);
    if (heartbeat != nullptr && ! heartbeat->receivedResponse.get ())
    {
        if (++pongsMissed >= maxPongsMissedBeforeDisconnect)
        {
            DBG ("Heartbeat failed, resetting client...");
            heartbeat.reset ();
            clientSocket.reset ();
            return false;
        }
    }

    jassert (heartbeat != nullptr);

    if (heartbeat != nullptr)
        heartbeat->receivedResponse = false;

    return true;
}

bool WebSocketServer::processHeartbeat (const MemoryBlock& buffer)
{
    const auto msg = MessageFromBufferSafe (buffer);

    if (msg == "ping") //outgoing
    {
        return checkHeartbeat ();
    }
    else if (msg == "pong") //incoming
    {
        jassert (heartbeat != nullptr);
        if (heartbeat != nullptr)
        {
            heartbeat->receivedResponse = true;
            pongsMissed = 0;
        }

        return false; //consume message
    }
    else if (msg == "generate")
    {
        jassert (heartbeat != nullptr);
        if (heartbeat != nullptr)
        {
            const auto lastHeartbeat = heartbeat->lastHeartbeat;
            const auto now = Time::getCurrentTime ();
            if (RelativeTime (now - lastHeartbeat).inMilliseconds () >= heartbeat->interval)
            {
                sendMessage ("ping");
                heartbeat->lastHeartbeat = Time::getCurrentTime ();
            }
        }
    }

    return true; //not a heartbeat message
}

String WebSocketServer::MessageFromBufferSafe (const MemoryBlock& b)
{
    return String::createStringFromData (b.getData(), b.getSize());   
}

bool WebSocketServer::sendMessage (const juce::MemoryBlock& data) {
    if (clientSocket == nullptr || !clientSocket->isConnected()) {

        if (currentlyWaitingForConnection.get ()) return false;

        DBG("Client socket is not connected or null.");
        if (forceRestart == nullptr)
            forceRestart = std::make_unique<ForceRestart> ();
        
        if (++forceRestart->count >= 3)
        {
            DBG ("Force restarting server...");
            stopThread (4000);
            startThread ();
            forceRestart.reset ();
        }
        return false;
    }

    outgoingMessages.push (data);
    return true;
}

bool WebSocketServer::sendMessageCompressed (const MemoryBlock& b)
{
    // const auto compressed = CompressionHelper::CompressUsingZlib (b);
    // jassert (compressed.getSize () > 0);

    jassertfalse; //compression not implemented yet
    return sendMessage (b);
}

bool WebSocketServer::sendMessage (const juce::String& data) {
    return sendMessage (juce::MemoryBlock (data.toRawUTF8(), data.getNumBytesAsUTF8()));
}



juce::String WebSocketServer::computeWebSocketAcceptKey(const juce::String& key) {
    static const juce::String magicGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"; //this is a gobally unique identifier, see RFC 6455 (https://www.rfc-editor.org/rfc/rfc6455)
    juce::String combined = key + magicGUID;

    // Assuming TinySHA1 is correctly implemented
    sha1::SHA1 sha;
    sha.processBytes(combined.toRawUTF8(), combined.getNumBytesAsUTF8());
    uint32_t digest[5];
    sha.getDigest(digest);

    // Convert to a byte array
    char sha1Buf[20];
    for (int i = 0; i < 5; ++i) {
        digest[i] = htonl(digest[i]);  // Ensure network byte order
        memcpy(sha1Buf + i * 4, &digest[i], 4);
    }

    // Base64 encode
    return juce::Base64::toBase64(sha1Buf, 20);
}

bool WebSocketServer::sendWebSocketMessage(const juce::MemoryBlock& data) {
    if (clientSocket == nullptr || !clientSocket->isConnected()) {
        if (currentlyWaitingForConnection.get()) return false;
        
        DBG("Client socket is not connected or null.");
        
        return false;
    }

    const auto isTextBuffer = bufferContainsOnlyUtf8 (data);
    juce::MemoryBlock framedData = formatWebSocketFrame(data, isTextBuffer);

    return sendFrame (framedData);
}

MemoryBlock WebSocketServer::formatWebSocketFrame(const MemoryBlock& data, bool isTextFrame) {
    juce::MemoryBlock frame;
    unsigned char firstByte = isTextFrame ? 0x81 : 0x82; // 0x81 for text, 0x82 for binary
    frame.append(&firstByte, 1);  // Start of frame: FIN bit set and appropriate opcode

    size_t dataLength = data.getSize();
    appendLengthBytes(dataLength, frame);

    frame.append(data.getData(), data.getSize());  // Append the actual data
    return frame;
}

void WebSocketServer::appendLengthBytes(size_t length, juce::MemoryBlock& frame) {
    if (length <= 125) {
        unsigned char lengthByte = static_cast<unsigned char>(length);
        frame.append(&lengthByte, 1);
    } else if (length <= 65535) {
        unsigned char lengthBytes[3] = {126, static_cast<unsigned char>(length >> 8), static_cast<unsigned char>(length)};
        frame.append(lengthBytes, 3);
    } else {
        unsigned char lengthBytes[9];
        lengthBytes[0] = 127;
        for (int i = 0; i < 8; ++i) {
            lengthBytes[i + 1] = (length >> (56 - i * 8)) & 0xFF;
        }
        frame.append(lengthBytes, 9);
    }
}

bool WebSocketServer::sendFrame(const juce::MemoryBlock& frame) {
    size_t totalBytesWritten = 0;
    const char* frameData = static_cast<const char*>(frame.getData());
    size_t frameSize = frame.getSize();

    while (totalBytesWritten < frameSize && clientSocket->isConnected() && !threadShouldExit()) {
        int bytesWritten = clientSocket->write(frameData + totalBytesWritten, static_cast<int>(frameSize - totalBytesWritten));
        if (bytesWritten > 0) {
            totalBytesWritten += bytesWritten;
        } else {
            DBG("Failed to write data. Is this a dropped frame?");
            break;
        }
    }

    return totalBytesWritten == frameSize;
}


bool WebSocketServer::handleHandshake(juce::StreamingSocket* clientSocket) {
    juce::String request;
    while (clientSocket->isConnected() && !threadShouldExit()) {
        if (clientSocket->waitUntilReady(true, 100) > 0) {  // Check if data is ready
            char buffer[512];
            int bytesRead = clientSocket->read(buffer, sizeof(buffer) - 1, false);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';  // Null terminate the string
                request += buffer;

                // Check if the end of the header section has been reached
                if (request.contains("\r\n\r\n")) {
                    DBG("Complete Handshake Buffer: " + request);
                    break;  // Exit the loop if the end of the header is found
                }
            } else {
                // No more data or read error, sleep a little then continue waiting or exit if needed
                Thread::sleep(100);
            }
        } else if (!clientSocket->isConnected()) {
            DBG("Client disconnected during handshake");
            return false;
        }
    }

    // Now process the full request
    if (request.contains("GET") && request.contains("Upgrade: websocket")) {
        int keyStart = request.indexOfIgnoreCase("Sec-WebSocket-Key:") + 18; // find the key
        if (keyStart == 17) {
            DBG("Sec-WebSocket-Key not found in the request.");
            return false;
        }
        
        int keyEnd = request.indexOf(keyStart, "\r\n");
        if (keyEnd == -1) {
            DBG("Malformed WebSocket key header.");
            return false;
        }

        juce::String key = request.substring(keyStart, keyEnd).trim();
        juce::String acceptKey = computeWebSocketAcceptKey(key);
        
        juce::String response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
            "\r\n";
        
        DBG("Responding to handshake with: " + response);
        clientSocket->write(response.toRawUTF8(), response.getNumBytesAsUTF8());
        // clientSocket->write ("Hello from server!", 18);
        return true;
    }

    return false;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
WebSocketQueueSender::WebSocketQueueSender (WebSocketServer& s)
:
Thread ("WebSocketQueueSender"),
server (s)
{
    // startRealtimeThread (
    //     Thread::RealtimeOptions ()
    //     .withPriority (10)
    //     .withPeriodHz (90)
    // );
    startThread (Thread::Priority::high);
}

WebSocketQueueSender::~WebSocketQueueSender()
{
    stopThread (2000);
}

void WebSocketQueueSender::run()
{
    MemoryBlock processingBuffer;
    while (!threadShouldExit ())
    {
        if (! server.processOutgoingMessages (processingBuffer))
            Thread::sleep (1);
    }
}
