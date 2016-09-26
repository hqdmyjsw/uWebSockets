#include "../src/uWS.h"

#include <node.h>
#include <node_buffer.h>
#include <cstring>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <uv.h>

using namespace std;
using namespace v8;

Persistent<Object> persistentTicket;

enum {
    HUB_PTR,
    SERVER_CONNECTION_CALLBACK,
    CLIENT_CONNECTION_CALLBACK,

    SERVER_MESSAGE_CALLBACK,
    CLIENT_MESSAGE_CALLBACK,

    SERVER_PING_CALLBACK,
    CLIENT_PING_CALLBACK,

    SERVER_PONG_CALLBACK,
    CLIENT_PONG_CALLBACK,

    SERVER_DISCONNECTION_CALLBACK,
    CLIENT_DISCONNECTION_CALLBACK,

    SIZE_OF_HUB
};

union SocketUnion {
    double number;
    uv_poll_t *pollHandle;
};

class NativeString {
    char *data;
    size_t length;
    char utf8ValueMemory[sizeof(String::Utf8Value)];
    String::Utf8Value *utf8Value = nullptr;
public:
    NativeString(const Local<Value> &value) {
        if (value->IsUndefined()) {
            data = nullptr;
            length = 0;
        } else if (value->IsString()) {
            utf8Value = new (utf8ValueMemory) String::Utf8Value(value);
            data = (**utf8Value);
            length = utf8Value->length();
        } else if (node::Buffer::HasInstance(value)) {
            data = node::Buffer::Data(value);
            length = node::Buffer::Length(value);
        } else if (value->IsTypedArray()) {
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            ArrayBuffer::Contents contents = arrayBufferView->Buffer()->GetContents();
            length = contents.ByteLength();
            data = (char *) contents.Data();
        } else if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            ArrayBuffer::Contents contents = arrayBuffer->GetContents();
            length = contents.ByteLength();
            data = (char *) contents.Data();
        } else {
            static char empty[] = "";
            data = empty;
            length = 0;
        }
    }

    char *getData() {return data;}
    size_t getLength() {return length;}
    ~NativeString() {
        if (utf8Value) {
            utf8Value->~Utf8Value();
        }
    }
};

template <bool isServer>
inline Local<Number> wrapSocket(uWS::WebSocket<isServer> webSocket, Isolate *isolate) {
    SocketUnion socketUnion;
    socketUnion.pollHandle = webSocket.getPollHandle();
    return Number::New(isolate, socketUnion.number);
}

template <bool isServer>
inline uWS::WebSocket<isServer> unwrapSocket(Local<Number> number) {
    SocketUnion socketUnion;
    socketUnion.number = number->Value();
    return uWS::WebSocket<isServer>(socketUnion.pollHandle);
}

inline Local<Value> wrapMessage(const char *message, size_t length, uWS::OpCode opCode, Isolate *isolate) {
    return opCode == uWS::OpCode::BINARY ? (Local<Value>) ArrayBuffer::New(isolate, (char *) message, length) : (Local<Value>) String::NewFromUtf8(isolate, message, String::kNormalString, length);
}

template <bool isServer>
inline Local<Value> getDataV8(uWS::WebSocket<isServer> webSocket, Isolate *isolate) {
    return webSocket.getUserData() ? Local<Value>::New(isolate, *(Persistent<Value> *) webSocket.getUserData()) : Local<Value>::Cast(Undefined(isolate));
}

template <bool isServer>
void getUserData(const FunctionCallbackInfo<Value> &args) {
    args.GetReturnValue().Set(getDataV8(unwrapSocket<isServer>(args[0]->ToNumber()), args.GetIsolate()));
}

template <bool isServer>
void clearUserData(const FunctionCallbackInfo<Value> &args) {
    uWS::WebSocket<isServer> webSocket = unwrapSocket<isServer>(args[0]->ToNumber());
    ((Persistent<Value> *) webSocket.getUserData())->Reset();
    delete (Persistent<Value> *) webSocket.getUserData();
}

template <bool isServer>
void setUserData(const FunctionCallbackInfo<Value> &args) {
    uWS::WebSocket<isServer> webSocket = unwrapSocket<isServer>(args[0]->ToNumber());
    if (webSocket.getUserData()) {
        ((Persistent<Value> *) webSocket.getUserData())->Reset(args.GetIsolate(), args[1]);
    } else {
        webSocket.setUserData(new Persistent<Value>(args.GetIsolate(), args[1]));
    }
}

template <bool isServer>
void getAddress(const FunctionCallbackInfo<Value> &args)
{
    typename uWS::WebSocket<isServer>::Address address = unwrapSocket<isServer>(args[0]->ToNumber()).getAddress();
    Local<Array> array = Array::New(args.GetIsolate(), 3);
    array->Set(0, Integer::New(args.GetIsolate(), address.port));
    array->Set(1, String::NewFromUtf8(args.GetIsolate(), address.address));
    array->Set(2, String::NewFromUtf8(args.GetIsolate(), address.family));
    args.GetReturnValue().Set(array);
}

uv_handle_t *getTcpHandle(void *handleWrap) {
    volatile char *memory = (volatile char *) handleWrap;
    for (volatile uv_handle_t *tcpHandle = (volatile uv_handle_t *) memory; tcpHandle->type != UV_TCP
         || tcpHandle->data != handleWrap || tcpHandle->loop != uv_default_loop(); tcpHandle = (volatile uv_handle_t *) memory) {
        memory++;
    }
    return (uv_handle_t *) memory;
}

struct SendCallbackData {
    Persistent<Function> jsCallback;
    Isolate *isolate;
};

void sendCallback(void *webSocket, void *data, bool cancelled)
{
    SendCallbackData *sc = (SendCallbackData *) data;
    if (!cancelled) {
        HandleScope hs(sc->isolate);
        node::MakeCallback(sc->isolate, sc->isolate->GetCurrentContext()->Global(), Local<Function>::New(sc->isolate, sc->jsCallback), 0, nullptr);
    }
    sc->jsCallback.Reset();
    delete sc;
}

template <bool isServer>
void send(const FunctionCallbackInfo<Value> &args)
{
    uWS::OpCode opCode = (uWS::OpCode) args[2]->IntegerValue();
    NativeString nativeString(args[1]);

    SendCallbackData *sc = nullptr;
    void (*callback)(void *, void *, bool) = nullptr;

    if (args[3]->IsFunction()) {
        callback = sendCallback;
        sc = new SendCallbackData;
        sc->jsCallback.Reset(args.GetIsolate(), Local<Function>::Cast(args[3]));
        sc->isolate = args.GetIsolate();
    }

    unwrapSocket<isServer>(args[0]->ToNumber()).send(nativeString.getData(),
                           nativeString.getLength(), opCode, callback, sc);
}

void connect(const FunctionCallbackInfo<Value> &args) {
    uWS::Hub *hub = (uWS::Hub *) args.Holder()->GetAlignedPointerFromInternalField(HUB_PTR);
    NativeString uri(args[0]);
    hub->connect(std::string(uri.getData(), uri.getLength()), new Persistent<Value>(args.GetIsolate(), args[1]));
}

void upgrade(const FunctionCallbackInfo<Value> &args) {
    uWS::Hub *hub = (uWS::Hub *) args.Holder()->GetAlignedPointerFromInternalField(HUB_PTR);
    Local<Object> ticket = args[0]->ToObject();
    NativeString secKey(args[1]);
    NativeString extensions(args[2]);

    uv_os_sock_t *fd = (uv_os_sock_t *) ticket->GetAlignedPointerFromInternalField(0);
    SSL *ssl = (SSL *) ticket->GetAlignedPointerFromInternalField(1);

    // todo: move this check into core!
    if (*fd != INVALID_SOCKET) {
        hub->upgrade(*fd, secKey.getData(), ssl, extensions.getData(), extensions.getLength());
    } else {
        if (ssl) {
            SSL_free(ssl);
        }
    }
    delete fd;
}

void transfer(const FunctionCallbackInfo<Value> &args) {
    // (_handle.fd OR _handle), SSL
    uv_os_sock_t *fd = new uv_os_sock_t;
    if (args[0]->IsObject()) {
        uv_fileno(getTcpHandle(args[0]->ToObject()->GetAlignedPointerFromInternalField(0)), (uv_os_fd_t *) fd);
    } else {
        *fd = args[0]->IntegerValue();
    }

    *fd = dup(*fd);
    SSL *ssl = nullptr;
    if (args[1]->IsExternal()) {
        ssl = (SSL *) args[1].As<External>()->Value();
        SSL_up_ref(ssl);
    }

    Local<Object> ticket = Local<Object>::New(args.GetIsolate(), persistentTicket)->Clone();
    ticket->SetAlignedPointerInInternalField(0, fd);
    ticket->SetAlignedPointerInInternalField(1, ssl);
    args.GetReturnValue().Set(ticket);
}

void Hub(const FunctionCallbackInfo<Value> &args) {
    if (args.IsConstructCall()) {
        // todo: these needs to be removed on destruction
        args.This()->SetAlignedPointerInInternalField(HUB_PTR, new uWS::Hub);
        for (int i = 1; i < SIZE_OF_HUB; i++) {
            args.This()->SetAlignedPointerInInternalField(i, new Persistent<Function>);
        }
        args.GetReturnValue().Set(args.This());
    }
}

template <bool isServer>
void onConnection(const FunctionCallbackInfo<Value> &args) {
    uWS::Hub *hub = (uWS::Hub *) args.Holder()->GetAlignedPointerFromInternalField(HUB_PTR);
    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *connectionCallback = (Persistent<Function> *) args.Holder()->GetAlignedPointerFromInternalField(CLIENT_CONNECTION_CALLBACK - isServer);
    connectionCallback->Reset(isolate, Local<Function>::Cast(args[0]));
    hub->onConnection([isolate, connectionCallback](uWS::WebSocket<isServer> webSocket) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *connectionCallback), 1, argv);
    });
}

template <bool isServer>
void onMessage(const FunctionCallbackInfo<Value> &args) {
    uWS::Hub *hub = (uWS::Hub *) args.Holder()->GetAlignedPointerFromInternalField(HUB_PTR);
    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *messageCallback = (Persistent<Function> *) args.Holder()->GetAlignedPointerFromInternalField(CLIENT_MESSAGE_CALLBACK - isServer);
    messageCallback->Reset(isolate, Local<Function>::Cast(args[0]));
    hub->onMessage([isolate, messageCallback](uWS::WebSocket<isServer> webSocket, const char *message, size_t length, uWS::OpCode opCode) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapMessage(message, length, opCode, isolate),
                               getDataV8(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *messageCallback), 2, argv);
    });
}

template <bool isServer>
void onPing(const FunctionCallbackInfo<Value> &args) {
    uWS::Hub *hub = (uWS::Hub *) args.Holder()->GetAlignedPointerFromInternalField(HUB_PTR);
    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *pingCallback = (Persistent<Function> *) args.Holder()->GetAlignedPointerFromInternalField(CLIENT_PING_CALLBACK - isServer);
    pingCallback->Reset(isolate, Local<Function>::Cast(args[0]));
    hub->onPing([isolate, pingCallback](uWS::WebSocket<isServer> webSocket, const char *message, size_t length) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapMessage(message, length, uWS::OpCode::PING, isolate),
                               getDataV8(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *pingCallback), 2, argv);
    });
}

template <bool isServer>
void onPong(const FunctionCallbackInfo<Value> &args) {
    uWS::Hub *hub = (uWS::Hub *) args.Holder()->GetAlignedPointerFromInternalField(HUB_PTR);
    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *pongCallback = (Persistent<Function> *) args.Holder()->GetAlignedPointerFromInternalField(CLIENT_PONG_CALLBACK - isServer);
    pongCallback->Reset(isolate, Local<Function>::Cast(args[0]));
    hub->onPong([isolate, pongCallback](uWS::WebSocket<isServer> webSocket, const char *message, size_t length) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapMessage(message, length, uWS::OpCode::PONG, isolate),
                               getDataV8(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *pongCallback), 2, argv);
    });
}

template <bool isServer>
void onDisconnection(const FunctionCallbackInfo<Value> &args) {
    uWS::Hub *hub = (uWS::Hub *) args.Holder()->GetAlignedPointerFromInternalField(HUB_PTR);
    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *disconnectionCallback = (Persistent<Function> *) args.Holder()->GetAlignedPointerFromInternalField(CLIENT_DISCONNECTION_CALLBACK - isServer);
    disconnectionCallback->Reset(isolate, Local<Function>::Cast(args[0]));
    hub->onDisconnection([isolate, disconnectionCallback](uWS::WebSocket<isServer> webSocket, int code, char *message, size_t length) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate),
                               Integer::New(isolate, code),
                               wrapMessage(message, length, uWS::OpCode::CLOSE, isolate),
                               getDataV8(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *disconnectionCallback), 4, argv);
    });
}

/*
void close(const FunctionCallbackInfo<Value> &args)
{
    uWS::Server *server = (uWS::Server *) args.Holder()->GetAlignedPointerFromInternalField(0);
    if (args.Length()) {
        // socket, code, data
        uWS::WebSocket socket = unwrapSocket(args[0]->ToNumber());
        NativeString nativeString(args[2]);
        socket.close(false, args[1]->IntegerValue(), nativeString.getData(), nativeString.getLength());
    } else {
        server->close(false);
    }
}

void broadcast(const FunctionCallbackInfo<Value> &args)
{
    uWS::Server *server = (uWS::Server *) args.Holder()->GetAlignedPointerFromInternalField(0);
    OpCode opCode = args[1]->BooleanValue() ? BINARY : TEXT;
    NativeString nativeString(args[0]);
    server->broadcast(nativeString.getData(), nativeString.getLength(), opCode);
}

void prepareMessage(const FunctionCallbackInfo<Value> &args)
{
    OpCode opCode = (uWS::OpCode) args[1]->IntegerValue();
    NativeString nativeString(args[0]);
    Local<Object> preparedMessage = Local<Object>::New(args.GetIsolate(), persistentTicket)->Clone();
    preparedMessage->SetAlignedPointerInInternalField(0, WebSocket::prepareMessage(nativeString.getData(), nativeString.getLength(), opCode, false));
    args.GetReturnValue().Set(preparedMessage);
}

void sendPrepared(const FunctionCallbackInfo<Value> &args)
{
    unwrapSocket(args[0]->ToNumber())
                 .sendPrepared((WebSocket::PreparedMessage *) args[1]->ToObject()->GetAlignedPointerFromInternalField(0));
}

void finalizeMessage(const FunctionCallbackInfo<Value> &args)
{
    WebSocket::finalizeMessage((WebSocket::PreparedMessage *) args[0]->ToObject()->GetAlignedPointerFromInternalField(0));
}*/

void Main(Local<Object> exports) {
    Isolate *isolate = exports->GetIsolate();
    Local<FunctionTemplate> hubTemplate = FunctionTemplate::New(isolate, Hub);
    hubTemplate->InstanceTemplate()->SetInternalFieldCount(SIZE_OF_HUB);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onConnectionServer", onConnection<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onConnectionClient", onConnection<uWS::CLIENT>);
    
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onMessageServer", onMessage<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onMessageClient", onMessage<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onPingServer", onPing<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onPingClient", onPing<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onPongServer", onPong<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onPongClient", onPong<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onDisconnectionServer", onDisconnection<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "onDisconnectionClient", onDisconnection<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "setUserDataServer", setUserData<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "setUserDataClient", setUserData<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "getUserDataServer", getUserData<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "getUserDataClient", getUserData<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "clearUserDataServer", clearUserData<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "clearUserDataClient", clearUserData<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "getAddressServer", getAddress<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "getAddressClient", getAddress<uWS::CLIENT>);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "sendServer", send<uWS::SERVER>);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "sendClient", send<uWS::CLIENT>);

    // subject to change!
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "transfer", transfer);
    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "upgrade", upgrade);

    NODE_SET_PROTOTYPE_METHOD(hubTemplate, "connect", connect);


    /*
    NODE_SET_PROTOTYPE_METHOD(tpl, "close", close);
    NODE_SET_PROTOTYPE_METHOD(tpl, "broadcast", broadcast);

    NODE_SET_PROTOTYPE_METHOD(tpl, "prepareMessage", prepareMessage);
    NODE_SET_PROTOTYPE_METHOD(tpl, "sendPrepared", sendPrepared);
    NODE_SET_PROTOTYPE_METHOD(tpl, "finalizeMessage", finalizeMessage);*/

    exports->Set(String::NewFromUtf8(isolate, "Hub"), hubTemplate->GetFunction());

    Local<ObjectTemplate> ticketTemplate = ObjectTemplate::New(isolate);
    ticketTemplate->SetInternalFieldCount(2);
    persistentTicket.Reset(isolate, ticketTemplate->NewInstance());
}

NODE_MODULE(uws, Main)
