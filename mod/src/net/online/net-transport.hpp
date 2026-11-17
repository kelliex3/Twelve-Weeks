#pragma once

#include <cstdint>
#include <winsock2.h>

namespace net {

	int EnsureWinsock();

}

namespace net::transport {

	bool Open(uint16_t bindPort);
	void Close();
	bool IsOpen();

	int  Send(const sockaddr_in& to, const void* data, int len);
	int  Recv(void* buf, int cap, sockaddr_in* from);

	int  LastError();

	bool OpenRelay(const char* relayIp, uint16_t relayPort, uint32_t sessionId, uint8_t mySlot);
	bool IsRelay();

	int  DatagramOverhead();

	sockaddr_in RelayVirtualAddr(uint8_t slot);
	bool RelaySlotForAddr(const sockaddr_in& a, uint8_t* slot);

	bool SameAddr(const sockaddr_in& a, const sockaddr_in& b);
	bool MakeAddr(const char* ip, uint16_t port, sockaddr_in* out); // false = bad IP

}