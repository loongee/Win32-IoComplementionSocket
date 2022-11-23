#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <string>

#include "SockIoServer.h"

// Need to link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

constexpr ULONG_PTR kExitKey = -1;

SockIoServer::SockIoServer()
{
  WSADATA wsaData;
  auto result = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (result != NO_ERROR) {
      wprintf(L"Error at WSAStartup\n");
      return;
  }

  needCleanup = true;
}

SockIoServer::~SockIoServer()
{
  exiting = true;

  if (needCleanup) {
    WSACleanup();
  }

  if (hCompPort != NULL) {
    PostQueuedCompletionStatus(hCompPort, 0, kExitKey, NULL);
    while (!loopExited) {
      Sleep(0);
    }
    CloseHandle(hCompPort);
    hCompPort = NULL;
  }

  if (socket != NULL) {
    closesocket(socket);
    socket = NULL;
  }
}

void SockIoServer::Start(uint16_t port)
{
  hCompPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  if (hCompPort == NULL) {
      wprintf(L"CreateIoCompletionPort failed with error: %u\n", GetLastError() );
      return;
  }

  // Create a listening socket
  socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (socket == INVALID_SOCKET) {
      wprintf(L"Create of ListenSocket socket failed with error: %u\n", WSAGetLastError() );
      return;
  }

  // Associate the listening socket with the completion port
  CreateIoCompletionPort((HANDLE)socket, hCompPort, socket, 0);
  
  sockaddr_in service;
  service.sin_family = AF_INET;
  service.sin_addr.s_addr = htonl(INADDR_ANY);
  service.sin_port = htons(port);

  if (bind(socket, (SOCKADDR *) & service, sizeof (service)) == SOCKET_ERROR) {
      wprintf(L"bind failed with error: %u\n", WSAGetLastError());
      closesocket(socket);
      socket = INVALID_SOCKET;
      return;
  }

  //----------------------------------------
  // Start listening on the listening socket
  if (listen(socket, 100) == SOCKET_ERROR) {
      wprintf(L"listen failed with error: %u\n", WSAGetLastError());
      closesocket(socket);
      socket = INVALID_SOCKET;
      return;
  }

  printf("Listening on address: %d\n", port);
  _beginthread(SocketProcessLoop, 0, this);
  LoadAcceptFn();
  Accept();  // accept one incoming socket
}

void SockIoServer::SendData(SOCKET socket, void* buf, size_t size)
{
  SockIoOperContext* pContext = new SockIoOperContext();
  pContext->oper = SockIoOperation::kSend;
  pContext->socket = socket;
  pContext->PrepareBuf(size);
  memcpy(pContext->realBuf.get(), buf, size);

  DWORD dwBytes = 0;
  DWORD dwFlag = 0;
  if (WSASend(socket, &pContext->buf, 1, NULL, dwFlag, &pContext->overlapped, nullptr)
    && WSAGetLastError() != ERROR_IO_PENDING) {
    printf("Send fail:%d\n", WSAGetLastError());
  }
}

void SockIoServer::RecvData(SOCKET socket, size_t size)
{
  SockIoOperContext* pContext = new SockIoOperContext();
  pContext->oper = SockIoOperation::kReceive;
  pContext->socket = socket;
  pContext->PrepareBuf(size);

  DWORD dwBytes = 0;
  DWORD dwFlag = 0;
  if (WSARecv(socket, &pContext->buf, 1, &dwBytes, &dwFlag, &pContext->overlapped, nullptr)
    && WSAGetLastError() != ERROR_IO_PENDING) {
    printf("Recv fail:%d\n", WSAGetLastError());
  }
}

void SockIoServer::OnNewSocketAccepted(SOCKET socket)
{
  printf("accept new socket:%p\n", socket);
  RecvData(socket, 1024);
  Accept();  // prepare next available socket
}

void SockIoServer::OnNewDataReceived(SockIoOperContext& ctx, DWORD dwSize)
{
  std::string str(ctx.realBuf.get(), dwSize);
  printf("Recv:%s\n", str.c_str());

  str += " echo";
  SendData(ctx.socket, (void*)str.c_str(), str.size());
  RecvData(ctx.socket, 1024);
}

void SockIoServer::OnDataSend(SockIoOperContext& ctx, DWORD dwSize)
{
  std::string str(ctx.realBuf.get(), dwSize);
  printf("Send:%s\n", str.c_str());
}

void SockIoServer::Accept()
{
  // Create an accepting socket
  auto acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (acceptSocket == INVALID_SOCKET) {
      wprintf(L"Create accept socket failed with error: %u\n", WSAGetLastError());
      closesocket(acceptSocket);
      return;
  }

  SockIoOperContext* pContext = new SockIoOperContext();
  pContext->oper = SockIoOperation::kAccept;
  pContext->socket = acceptSocket;
  pContext->PrepareBuf((sizeof(sockaddr_in) + 16) * 2);

  DWORD dwBytes = 0;
  auto bRetVal = lpfnAcceptEx(socket, acceptSocket, pContext->realBuf.get(),
                0,
                sizeof (sockaddr_in) + 16, sizeof (sockaddr_in) + 16, 
                &dwBytes, &pContext->overlapped);
  if (bRetVal == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
      wprintf(L"AcceptEx failed with error: %u\n", WSAGetLastError());
      closesocket(acceptSocket);
      return;
  }

  CreateIoCompletionPort((HANDLE)acceptSocket, hCompPort, 0, 0);
}

void SockIoServer::SocketProcessLoop(void* param)
{
  ((SockIoServer*)param)->SocketProcessLoopInner();
}

void SockIoServer::SocketProcessLoopInner()
{
  while (!exiting) {
    DWORD nTransfered = 0;
    LPOVERLAPPED pOverlapped = NULL;
    ULONG_PTR compKey;
    if (!GetQueuedCompletionStatus(hCompPort, &nTransfered, &compKey, &pOverlapped, INFINITE)) {
      if (ERROR_OPERATION_ABORTED != WSAGetLastError()) {
        printf("GetQueuedCompletionStatus fail:%d\n", WSAGetLastError());
      }
      break;
    }

    if (compKey == kExitKey) {
      continue;
    }

    std::unique_ptr<SockIoOperContext> pContext;
    pContext.reset(CONTAINING_RECORD(pOverlapped, SockIoOperContext, overlapped));

    if (nTransfered == 0 && pContext->oper != SockIoOperation::kAccept) {
      // socket is closed
      printf("socket closed:%p\n", pContext->socket);
      closesocket(pContext->socket);
      continue;
    }

    switch (pContext->oper) {
    case SockIoOperation::kAccept:
      OnNewSocketAccepted(pContext->socket);
      break;
    case SockIoOperation::kReceive:
      OnNewDataReceived(*pContext, nTransfered);
      break;
    case SockIoOperation::kSend:
      OnDataSend(*pContext, nTransfered);
      break;
    }
  }

  loopExited = true;
  printf("loop exited");
}

void SockIoServer::LoadAcceptFn()
{
  // Load the AcceptEx function into memory using WSAIoctl.
  // The WSAIoctl function is an extension of the ioctlsocket()
  // function that can use overlapped I/O. The function's 3rd
  // through 6th parameters are input and output buffers where
  // we pass the pointer to our AcceptEx function. This is used
  // so that we can call the AcceptEx function directly, rather
  // than refer to the Mswsock.lib library.
  GUID GuidAcceptEx = WSAID_ACCEPTEX;
  DWORD dwBytes = 0;
  auto iResult = WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &GuidAcceptEx, sizeof (GuidAcceptEx), 
            &lpfnAcceptEx, sizeof (lpfnAcceptEx), 
            &dwBytes, NULL, NULL);
  if (iResult != SOCKET_ERROR) {
    return;
  }

  wprintf(L"WSAIoctl failed with error: %u\n", WSAGetLastError());
  closesocket(socket);
  socket = INVALID_SOCKET;
  return;
}
