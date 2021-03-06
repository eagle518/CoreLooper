﻿#include "stdafx.h"
#include "tcpclient_windows.h"
#include "net/tcpclient.h"
#include "net/tcpserver.h"
#include "looper/looper.h"
#include "net/dnslooper.h"
#include "../../core/looper/handlerinternaldata.h"
#include "../../core/looper/message.inl"
#include <string>
#include <sstream>
#ifdef _MSC_VER
#include <mstcpip.h>
#endif

#include <WinSock2.h>  
#include <MSWSock.h>  
#include <Windows.h>  
#include <process.h>  

#ifdef _MSC_VER_DEBUG
	#define new DEBUG_NEW
#endif

using namespace std;

namespace Bear {
namespace Core
{
namespace Net {
enum
{
	BM_DNS_ACK,
};

TcpClient_Windows::TcpClient_Windows()
{
	mInternalData->SetActiveObject();

	SetObjectName("TcpClient_Windows");
	SockTool::InitSockTool();

	//DV("%s,this=0x%x", __func__, this);
	mIocp = (HANDLE)Looper::CurrentLooper()->GetLooperHandle();
	mSock = INVALID_SOCKET;
	m_lpfnConnectEx = nullptr;

	mSingalClosePending = false;
	mSignalCloseHasFired = false;
}

TcpClient_Windows::~TcpClient_Windows()
{
	//DV("%s,this=0x%x", __func__, this);
	ASSERT(mSock == INVALID_SOCKET);

	//ASSERT(IsMyselfThread());//确保在iocp线程中析构(是为了能直接调用TcpServer接口)
	//由于TcpClient_Windows.mBaseServer是强引用，所以运行到此时能保证TcpServer和Looper都存在并正常运行
}

int TcpClient_Windows::OnRecv(IoContext *context, DWORD bytes)
{
	ASSERT(context == &this->mIoContextRecv);

	context->mBusying = false;
	bool repost = false;
	if (bytes > 0)
	{
		UpdateRecvTick();

		int ret = mInbox.Write((LPBYTE)context->mByteBuffer.GetDataPointer(), bytes);
		if (ret == bytes)
		{
			int freeBytes = mInbox.GetTailFreeSize();
			if (freeBytes > 0)
			{
				ret = context->PostRecv(freeBytes);
				if (ret == 0)
				{
					repost = true;
				}
				else
				{
					Close();
				}
			}
		}
		else
		{
			ASSERT(FALSE);//todo
		}
	}
	else if (bytes == 0)
	{
		Close();
	}

	/*
	if (!repost)
	{
		Close();
		return -1;
	}
	*/

	OnReceive();

	return 0;
}

int TcpClient_Windows::OnSendDone(IoContext *context, DWORD bytes)
{
	context->mBusying = false;
	context->mByteBuffer.clear();

	UpdateSendTick();

	if (mSock == INVALID_SOCKET)
	{
		context->mSock = INVALID_SOCKET;
		PostDispose(context->mBaseClient);
		return 0;
	}
	else
	{
		bool full = (mOutbox.GetFreeSize() == 0);
		if (!full)
		{
			OnSend();
		}

		if (mMarkEndOfSend && mOutbox.IsEmpty())
		{
			shutdown(mSock, SD_SEND);
		}

		if (!mOutbox.IsEmpty() && !mIoContextSend.mBusying)
		{
			int ret = SendOutBox();
			if (ret)
			{
				return -1;
			}
		}
	}

	return 0;
}

int TcpClient_Windows::DispatchIoContext(IoContext *context, DWORD bytes)
{
	auto objThis = shared_from_this();//确保在DispatchIoContext执行期间不被删除

	//DW("%s,this=0x%x,threadId=0x%x", __func__, this, GetCurrentThreadId());


	//shared_ptr<TcpClient_Windows> ptr = dynamic_pointer_cast<TcpClient_Windows>(shared_from_this());
	//2016.03.22,现在采用PostDispose来保证不会删除调用栈上的对象
	//所以不再需要在这里用shared_ptr保护TcpClient_Windows

	switch (context->mType)
	{
	case IoContextType_Recv:
	{
		int ret = OnRecv(context, bytes);
		break;
	}
	case IoContextType_Send:
	{
		OnSendDone(context, bytes);
		break;
	}
	case IoContextType_Connect:
	{
		mIoContextConnect.mBusying = false;
		//PostDispose(mIoContextConnect.mBaseClient);
		mIoContextConnect.mBaseClient = nullptr;
		OnConnectAck();
		break;
	}
	default:
	{
		ASSERT(FALSE);
		break;
	}
	}

	if (mSingalClosePending)
	{
		OnClose();
	}

	return 0;
}

void TcpClient_Windows::OnReceive()
{
	SignalOnReceive(this);
}

//当可写时会调用本接口
void TcpClient_Windows::OnSend()
{
	SignalOnSend(this);
}

void TcpClient_Windows::OnClose()
{
	if (mSignalCloseHasFired)
	{
		return;
	}

	bool busying = (mIoContextConnect.mBusying || mIoContextRecv.mBusying || mIoContextSend.mBusying);
	if (busying)
	{
		mSingalClosePending = true;
		return;
	}

	mSingalClosePending = false;
	mSignalCloseHasFired = true;
	SignalOnClose(this);
}

//返回接收到的字节数
int TcpClient_Windows::Receive(LPVOID buf, int bufLen)
{
	ASSERT(buf);

	int bytes = MIN(mInbox.GetActualDataLength(), bufLen);
	if (bytes > 0)
	{
		memcpy(buf, mInbox.GetDataPointer(), bytes);
		mInbox.Eat(bytes);

		if (!mIoContextRecv.mBusying)
		{
			int freeBytes = mInbox.GetTailFreeSize();
			if (freeBytes == 0)
			{
				mInbox.MoveToHead();
			}

			freeBytes = mInbox.GetTailFreeSize();
			ASSERT(freeBytes > 0);
			if (mIoContextRecv.PostRecv(freeBytes))
			{
				Close();
			}
		}
		return bytes;
	}
	else if (mSock == INVALID_SOCKET)
	{
		return 0;
	}

	return -1;
}

//返回成功提交的字节数
int TcpClient_Windows::Send(LPVOID data, int dataLen)
{
	ASSERT(IsMyselfThread());

	int freeSize = mOutbox.GetFreeSize();
	int bytes = MIN(dataLen, freeSize);
	int ret = mOutbox.Write(data, bytes);

	if (ret > 0 && !mIoContextSend.mBusying)
	{
		int ack = SendOutBox();
		if (ack)
		{
			Close();
			return 0;
		}
	}

	return ret;
}

int TcpClient_Windows::SendOutBox()
{
	ASSERT(!mIoContextSend.mBusying);
	ASSERT(mIoContextSend.mByteBuffer.IsEmpty());
	ASSERT(!mOutbox.IsEmpty());

	int ret = mIoContextSend.mByteBuffer.Write(mOutbox.GetDataPointer(), mOutbox.GetActualDataLength());
	if (ret > 0)
	{
		int ack = mIoContextSend.PostSend();
		if (ack)
		{
			Close();
			return -1;
		}

		mOutbox.Eat(ret);
		return 0;
	}

	return -1;
}

int TcpClient_Windows::ConnectHelper(string ip)
{
	//ASSERT(SockTool::IsValidIP(ip));
	ASSERT(mSock == INVALID_SOCKET);

	int port = mBundle.GetInt("port");

	mSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	HANDLE handle = CreateIoCompletionPort((HANDLE)mSock, mIocp, (ULONG_PTR)this, 0);
	ASSERT(handle == mIocp);

	{
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = INADDR_ANY;
		sin.sin_port = 0;
		int ret = ::bind(mSock, (sockaddr*)&sin, sizeof(sin));
		//DT("mSock=%d,bind ret=%d", mSock,ret);
	}

	{
		DWORD ulBytesReturn = 0;
		struct tcp_keepalive stKeepIn = { 0 }, stKeepOut = { 0 };
		stKeepIn.keepalivetime = 1000 * 10;		// 超过20秒没接收数据就发送探测包
		stKeepIn.keepaliveinterval = 1000 * 2;	// 探测包ack超时后每隔2秒发送一次探测包
		stKeepIn.onoff = 1;						// 启用KEEPALIVE
		int ret = WSAIoctl(mSock, SIO_KEEPALIVE_VALS, (LPVOID)&stKeepIn, sizeof(tcp_keepalive), (LPVOID)&stKeepOut,
			sizeof(tcp_keepalive), &ulBytesReturn, NULL, NULL);
		if (ret == SOCKET_ERROR)
		{
			DW("CTCPNetwork::AddSession - WSAIoctl failed. errno(%d)", WSAGetLastError());
		}
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(ip.c_str());
	sin.sin_port = htons(port);

	mIoContextConnect.mType = IoContextType_Connect;
	mIoContextConnect.mBaseClient = dynamic_pointer_cast<TcpClient>(shared_from_this());

	//采用ConnectEx之前要绑定本地sock,否则调用会失败
	//ConnectEx默认超时为20秒
	if (!m_lpfnConnectEx)
	{
		GUID guidConnectEx = WSAID_CONNECTEX;
		DWORD dwBytes = 0;
		int ret = WSAIoctl(mSock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidConnectEx, sizeof(guidConnectEx),
			&m_lpfnConnectEx, sizeof(m_lpfnConnectEx), &dwBytes, NULL, NULL);
		ASSERT(ret == 0);
	}

	BOOL ok = m_lpfnConnectEx(mSock, (sockaddr*)&sin, sizeof(sockaddr), NULL, 0, 0, &(mIoContextConnect.mOV));
	if (!ok)
	{
		int error = WSAGetLastError();
		if (error == ERROR_IO_PENDING)
		{
			mIoContextConnect.mBusying = true;
			//DT("ERROR_IO_PENDING");
			ok = true;
		}
		else
		{
			DW("error = %d", error);
		}
	}

	return 0;
}

int TcpClient_Windows::Connect(Bundle& info)
{
	if (mSock != INVALID_SOCKET)
	{
		ASSERT(FALSE);
		return -1;
	}

	//BindProcData(mAddress, "address");

	mBundle = info;
	string addr = info.GetString("address");
	mAddress = addr.c_str();
	if (SockTool::IsValidIP(addr.c_str()))
	{
		return ConnectHelper(addr);
	}

	auto mainLooper = Looper::GetMainLooper();
	ASSERT(mainLooper);

	if (mainLooper)
	{
		auto dnsLooper = dynamic_pointer_cast<DnsLooper>(mainLooper->FindObject("DnsLooper"));
		if (!dnsLooper)
		{
			auto looper = make_shared<DnsLooper>();
			//looper->SetExitEvent(mainLooper->CreateExitEvent());
			mainLooper->AddChild(looper);
			looper->Start();

			dnsLooper = dynamic_pointer_cast<DnsLooper>(mainLooper->FindObject("DnsLooper"));
		}

		if (dnsLooper)
		{
			dnsLooper->AddRequest(addr, shared_from_this(), BM_DNS_ACK);
		}
	}

	return 0;
}

void TcpClient_Windows::Close()
{
	ASSERT(IsMyselfThread());

	bool needFireEvent = false;
	if (mSock != INVALID_SOCKET)
	{
		shutdown(mSock, SD_BOTH);
		closesocket(mSock);
		mSock = INVALID_SOCKET;
		needFireEvent = true;
	}

	if (!mIoContextConnect.mBusying)
	{
		PostDispose(mIoContextConnect.mBaseClient);
	}
	if (!mIoContextSend.mBusying)
	{
		PostDispose(mIoContextSend.mBaseClient);
	}
	if (!mIoContextRecv.mBusying)
	{
		PostDispose(mIoContextRecv.mBaseClient);
	}

	if (needFireEvent)
	{
		OnClose();
	}
}

int TcpClient_Windows::OnConnect(long handle, Bundle* extraInfo)
{
	ASSERT(IsMyselfThread());
	SOCKET s = (SOCKET)handle;

	if (s != INVALID_SOCKET)
	{
		ASSERT(mSock == INVALID_SOCKET);
		mSock = s;

		{
			mPeerDesc = StringTool::Format("%s:%d", SockTool::GetPeerIP(s).c_str(), SockTool::GetPeerPort(s));
			mLocalDesc =StringTool::Format("%s:%d", SockTool::GetLocalIP(s).c_str(), SockTool::GetLocalPort(s));
		}
		{
			// 设置连接的KEEPALIVE参数
			DWORD ulBytesReturn = 0;
			struct tcp_keepalive stKeepIn = { 0 }, stKeepOut = { 0 };
			stKeepIn.keepalivetime = 10 * 1000;		// 超过此秒数没接收数据就发送探测包
			stKeepIn.keepaliveinterval = 2 * 1000;	// 每隔几秒发送一次探测包
			stKeepIn.onoff = 1;						// 启用KEEPALIVE
			int ret = WSAIoctl(s, SIO_KEEPALIVE_VALS, (LPVOID)&stKeepIn, sizeof(tcp_keepalive), (LPVOID)&stKeepOut,
				sizeof(tcp_keepalive), &ulBytesReturn, NULL, NULL);
			if (ret == SOCKET_ERROR)
			{
				int x = 0;
			}
		}

#ifdef _MSC_VER
		HANDLE handle = CreateIoCompletionPort((HANDLE)mSock, mIocp, (ULONG_PTR)(IocpObject*)this, 0);
		ASSERT(handle == mIocp);
		if (!handle)
		{
			DW("fail CreateIoCompletionPort,error=%d", GetLastError());
		}

		ConfigCacheBox();
		mIoContextRecv.PostRecv();
#else
		ASSERT(FALSE);//todo
#endif
	}
	else
	{
		//crazy!
		ASSERT(FALSE);
		return -1;
	}

	SignalOnConnect(this, 0, nullptr, extraInfo);
	return 0;
}

void TcpClient_Windows::OnConnectAck()
{
	int ret = -1;

	if (mSock != INVALID_SOCKET)
	{
#ifdef _MSC_VER
		ret = GetLastError();

		setsockopt(mSock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
		int seconds;
		int bytes = sizeof(seconds);
		int iResult = 0;

		iResult = getsockopt(mSock, SOL_SOCKET, SO_CONNECT_TIME,
			(char *)&seconds, (PINT)&bytes);
		if (iResult != NO_ERROR)
		{
			DW("getsockopt(SO_CONNECT_TIME) failed with error: %u", WSAGetLastError());
		}
		else
		{
			if (seconds == 0xFFFFFFFF)
			{
				//DW("Connection not established yet\n");
			}
			else
			{
				//DT("Connection has been established %ld seconds\n", seconds);
				ret = 0;
			}
		}
#else
		ASSERT(FALSE);//todo
#endif

		}

	if (ret)
	{
		SignalOnConnect(this, ret, nullptr, nullptr);
	}
	else
	{
		ConfigCacheBox();
		mIoContextRecv.PostRecv();

		SignalOnConnect(this, 0, nullptr, nullptr);
	}
	}

void TcpClient_Windows::ConfigCacheBox()
{
	{
		mOutbox.SetBufferSize(4 * 1024, 8 * 1024);
		mOutbox.PrepareBuf(4 * 1024);

		IoContext& context = mIoContextSend;
		context.mType = IoContextType_Send;
		context.mSock = mSock;
		context.mByteBuffer.PrepareBuf(8 * 1024);
		context.mBaseClient = dynamic_pointer_cast<TcpClient>(shared_from_this());
	}
	{
		IoContext& context = mIoContextRecv;
		context.mType = IoContextType_Recv;
		context.mSock = mSock;
		context.mByteBuffer.PrepareBuf(8 * 1024);
		context.mBaseClient = dynamic_pointer_cast<TcpClient>(shared_from_this());
	}
}

LRESULT TcpClient_Windows::OnMessage(UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case BM_DESTROY:
	{
		Close();
		break;
	}
	case BM_DUMP:
	{
		int x = 0;
		break;
	}
	case BM_DNS_ACK:
	{
		string dns = (const char*)wp;
		string ip = (const char *)lp;
		//DV("%s=[%s]", dns.c_str(), ip.c_str());
		if (dns == mBundle.GetString("address") && mSock == INVALID_SOCKET)
		{
			ConnectHelper(ip);
		}
		else
		{
			//已取消连接
		}

		return 0;
	}
	}

	return __super::OnMessage(msg, wp, lp);
}

void TcpClient_Windows::MarkEndOfRecv()
{
	shutdown(mSock, SD_RECEIVE);
}

void TcpClient_Windows::MarkEndOfSend()
{
	mMarkEndOfSend = true;

	if (mOutbox.IsEmpty())
	{
		shutdown(mSock, SD_SEND);
	}
}

}
}
}