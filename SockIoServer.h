#pragma once
#include <memory>
#include <winsock2.h>
#include <mswsock.h>

enum class SockIoOperation {
  kAccept,
  kSend,
  kReceive,
};

class SockIoOperContext {
public:
  WSAOVERLAPPED overlapped{ 0 };
  SOCKET socket{ 0 };
  SockIoOperation oper{ SockIoOperation::kAccept };
  WSABUF buf{ 0 };
  std::unique_ptr<char> realBuf;

  void PrepareBuf(size_t size) {
    realBuf.reset(new char[size]);
    buf.buf = realBuf.get();
    buf.len = size;
  }
};

class SockIoServer {
public:
  SockIoServer();
  ~SockIoServer();
  void Start(uint16_t port);
  void SendData(SOCKET socket, void* buf, size_t size);
  void RecvData(SOCKET socket, size_t size);

  virtual void OnNewSocketAccepted(SOCKET socket);
  virtual void OnNewDataReceived(SockIoOperContext& ctx, DWORD dwSize);
  virtual void OnDataSend(SockIoOperContext& ctx, DWORD dwSize);

private:
  void LoadAcceptFn();
  void Accept();
  static void SocketProcessLoop(void* param);
  void SocketProcessLoopInner();

private:
  SOCKET socket{ INVALID_SOCKET };
  bool needCleanup{ false };
  HANDLE hCompPort{ NULL };
  LPFN_ACCEPTEX lpfnAcceptEx{ NULL };
  volatile bool exiting{ false };
  volatile bool loopExited{ false };
};

