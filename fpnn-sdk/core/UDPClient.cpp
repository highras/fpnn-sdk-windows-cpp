#include <exception>
#include <winsock2.h>
//#include <ws2tcpip.h>
#include <sys/types.h>
#include <io.h>
#include "ClientEngine.h"
#include "UDPClient.h"
#include "FPWriter.h"
#include "AutoRelease.h"
#include "FileSystemUtil.h"

using namespace fpnn;

UDPClient::UDPClient(const std::string& host, int port, bool autoReconnect): Client(host, port, autoReconnect),
	_MTU(Config::UDP::_internet_MTU), _keepAlive(false), _untransmittedSeconds(0)
	{
		if (_connectionInfo->isPrivateIP())
			_MTU = Config::UDP::_LAN_MTU;
		else
			_MTU = Config::UDP::_internet_MTU;
	}

class UDPQuestTask: public ITaskThreadPool::ITask
{
	FPQuestPtr _quest;
	ConnectionInfoPtr _connectionInfo;
	UDPClientPtr _client;

public:
	UDPQuestTask(UDPClientPtr client, FPQuestPtr quest, ConnectionInfoPtr connectionInfo):
		_quest(quest), _connectionInfo(connectionInfo), _client(client) {}

	virtual ~UDPQuestTask() {}

	virtual void run()
	{
		try
		{
			_client->processQuest(_quest, _connectionInfo);
		}
		catch (const FpnnError& ex){
			LOG_ERROR("UDP client processQuest() error:(%d)%s. %s", ex.code(), ex.what(), _connectionInfo->str().c_str());
		}
		catch (const std::exception& ex)
		{
			LOG_ERROR("UDP client processQuest() error: %s. %s", ex.what(), _connectionInfo->str().c_str());
		}
		catch (...)
		{
			LOG_ERROR("Fatal error occurred when UDP client processQuest() function. %s", _connectionInfo->str().c_str());
		}
	}
};

void UDPClient::dealQuest(FPQuestPtr quest, ConnectionInfoPtr connectionInfo)		//-- must done in thread pool or other thread.
{
	if (!_questProcessor)
	{
		LOG_ERROR("Recv a quest but UDP client without quest processor. %s", connectionInfo->str().c_str());
		return;
	}

	std::shared_ptr<UDPQuestTask> task(new UDPQuestTask(shared_from_this(), quest, connectionInfo));
	if (ClientEngine::runTask(task) == false)
	{
		LOG_ERROR("wake up thread pool to process UDP quest failed. Quest pool limitation is caught. Quest task havn't be executed. %s",
			connectionInfo->str().c_str());

		if (quest->isTwoWay())
		{
			try
			{
				FPAnswerPtr answer = FPAWriter::errorAnswer(quest, FPNN_EC_CORE_WORK_QUEUE_FULL, std::string("worker queue full, ") + connectionInfo->str().c_str());
				std::string *raw = answer->raw();
				//_engine->sendData(connectionInfo->socket, connectionInfo->token, raw);
				_engine->sendUDPData(connectionInfo->socket, connectionInfo->token, raw, slack_real_msec() + _timeoutQuest, quest->isOneWay());
			}
			catch (const FpnnError& ex)
			{
				LOG_ERROR("Generate error answer for UDP duplex client worker queue full failed. No answer returned, peer need to wait timeout. %s, exception:(%d)%s",
					connectionInfo->str().c_str(), ex.code(), ex.what());
			}
			catch (const std::exception& ex)
			{
				LOG_ERROR("Generate error answer for UDP duplex client worker queue full failed. No answer returned, peer need to wait timeout. %s, exception: %s",
					connectionInfo->str().c_str(), ex.what());
			}
			catch (...)
			{
				LOG_ERROR("Generate error answer for UDP duplex client worker queue full failed. No answer returned, peer need to wait timeout. %s",
					connectionInfo->str().c_str());
			}
		}
	}
}

bool UDPClient::perpareConnection(ConnectionInfoPtr currConnInfo)
{
	UDPClientConnection* connection = new UDPClientConnection(shared_from_this(), currConnInfo, _questProcessor, _MTU);
	if (_keepAlive)
		connection->enableKeepAlive();
	
	if (_untransmittedSeconds != 0)
		connection->setUntransmittedSeconds(_untransmittedSeconds);

	if (_embedRecvNotifyDeleagte)
		connection->embed_configRecvNotifyDelegate(_embedRecvNotifyDeleagte);

	callConnectedEvent(connection, true);

	bool joined = ClientEngine::instance()->join(connection);
	if (!joined)
	{
		LOG_ERROR("Join epoll failed after UDP connected event. %s", currConnInfo->str().c_str());
		willClose(connection, true);
		return false;
	}

	bool isClosed = false;
	bool beginReceived = connection->beginReceiving(isClosed);
	if (isClosed || !beginReceived)
	{
		LOG_ERROR("Begin receiving failed after UDP connected event. %s", currConnInfo->str().c_str());
		willClose(connection, true);
		return false;
	}
	
	return true;
}

SOCKET UDPClient::connectIPv4Address(ConnectionInfoPtr currConnInfo)
{
	SOCKET socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (socket == INVALID_SOCKET)
		return INVALID_SOCKET;

	size_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in* serverAddr = (struct sockaddr_in*)malloc(addrlen);

	memset(serverAddr, 0, addrlen);
	serverAddr->sin_family = AF_INET;
	serverAddr->sin_port = htons(currConnInfo->port);

	if (inet_pton(AF_INET, currConnInfo->ip.c_str(), &serverAddr->sin_addr) != 1)
	{
		closesocket(socket);
		free(serverAddr);
		return INVALID_SOCKET;
	}

	if (serverAddr->sin_addr.s_addr == INADDR_NONE)
	{
		closesocket(socket);
		free(serverAddr);
		return INVALID_SOCKET;
	}

	if (::connect(socket, (struct sockaddr *)serverAddr, addrlen) != 0)
	{
		closesocket(socket);
		free(serverAddr);
		return INVALID_SOCKET;
	}

	currConnInfo->changeToUDP(socket, (uint8_t*)serverAddr);

	return socket;
}
SOCKET UDPClient::connectIPv6Address(ConnectionInfoPtr currConnInfo)
{
	SOCKET socket = WSASocket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (socket == INVALID_SOCKET)
		return INVALID_SOCKET;

	size_t addrlen = sizeof(struct sockaddr_in6);
	struct sockaddr_in6* serverAddr = (struct sockaddr_in6*)malloc(addrlen);

	memset(serverAddr, 0, addrlen);
	serverAddr->sin6_family = AF_INET6;  
	serverAddr->sin6_port = htons(currConnInfo->port);

	if (inet_pton(AF_INET6, currConnInfo->ip.c_str(), &serverAddr->sin6_addr) != 1)
	{
		closesocket(socket);
		free(serverAddr);
		return INVALID_SOCKET;
	}

	if (::connect(socket, (struct sockaddr *)serverAddr, addrlen) != 0)
	{
		closesocket(socket);
		free(serverAddr);
		return INVALID_SOCKET;
	}

	currConnInfo->changeToUDP(socket, (uint8_t*)serverAddr);

	return socket;
}

bool UDPClient::connect()
{
	if (_connected)
		return true;

	ConnectionInfoPtr currConnInfo;
	{
		std::unique_lock<std::mutex> lck(_mutex);
		while (_connStatus == ConnStatus::Connecting)
			_condition.wait(lck);

		if (_connStatus == ConnStatus::Connected)
			return true;

		currConnInfo = _connectionInfo;

		_connected = false;
		_connStatus = ConnStatus::Connecting;
	}

	UDPClient* self = this;

	CannelableFinallyGuard errorGuard([self, currConnInfo](){
		std::unique_lock<std::mutex> lck(self->_mutex);
		if (currConnInfo.get() == self->_connectionInfo.get())
		{
			if (self->_connectionInfo->socket)
			{
				ConnectionInfoPtr newConnectionInfo(new ConnectionInfo(INVALID_SOCKET, self->_connectionInfo->port, self->_connectionInfo->ip, self->_isIPv4));
				newConnectionInfo->changeToUDP();
				self->_connectionInfo = newConnectionInfo;
			}

			self->_connected = false;
			self->_connStatus = ConnStatus::NoConnected;
		}
		self->_condition.notify_all();
	});

	SOCKET socket = INVALID_SOCKET;
	if (_isIPv4)
		socket = connectIPv4Address(currConnInfo);
	else
		socket = connectIPv6Address(currConnInfo);

	if (socket == INVALID_SOCKET)
	{
		LOG_ERROR("UDP client connect remote server %s failed.", currConnInfo->str().c_str());
		return false;
	}

	/*if (!nonblockedFd(socket))
	{
		closesocket(socket);
		LOG_ERROR("UDP client change socket to nonblocking for remote server %s failed.", currConnInfo->str().c_str());
		return false;
	}*/

	if (!perpareConnection(currConnInfo))
		return false;

	errorGuard.cancel();
	{
		std::unique_lock<std::mutex> lck(_mutex);
		if (_connectionInfo.get() == currConnInfo.get())
		{
			_connected = true;
			_connStatus = ConnStatus::Connected;
			_condition.notify_all();

			return true;
		}
	}

	LOG_ERROR("This codes (UDPClient::connect dupled) is impossible touched. This is just a safety inspection. If this ERROR triggered, please tell swxlion to fix it.");

	//-- dupled
	UDPClientConnection* conn = (UDPClientConnection*)_engine->takeConnection(currConnInfo.get());
	if (conn)
	{
		_engine->quit(conn);
		clearConnectionQuestCallbacks(conn, FPNN_EC_CORE_CONNECTION_CLOSED);
		willClose(conn, false);
	}

	std::unique_lock<std::mutex> lck(_mutex);

	while (_connStatus == ConnStatus::Connecting)
		_condition.wait(lck);

	_condition.notify_all();
	if (_connStatus == ConnStatus::Connected)
		return true;

	return false;
}

UDPClientPtr Client::createUDPClient(const std::string& host, int port, bool autoReconnect)
{
	return UDPClient::createClient(host, port, autoReconnect);
}
UDPClientPtr Client::createUDPClient(const std::string& endpoint, bool autoReconnect)
{
	return UDPClient::createClient(endpoint, autoReconnect);
}

void UDPClient::close()
{
	if (!_connected)
		return;

	ConnectionInfoPtr oldConnInfo;
	IQuestProcessorPtr questProcessor;
	{
		std::unique_lock<std::mutex> lck(_mutex);
		while (_connStatus == ConnStatus::Connecting)
			_condition.wait(lck);

		if (_connStatus == ConnStatus::NoConnected)
			return;

		oldConnInfo = _connectionInfo;

		ConnectionInfoPtr newConnectionInfo(new ConnectionInfo(INVALID_SOCKET, _connectionInfo->port, _connectionInfo->ip, _isIPv4));
		newConnectionInfo->changeToUDP();
		_connectionInfo = newConnectionInfo;
		_connected = false;
		_connStatus = ConnStatus::NoConnected;

		questProcessor = _questProcessor;
	}

	_engine->executeConnectionAction(oldConnInfo->socket, [questProcessor](BasicConnection* connection){
		UDPClientConnection* conn = (UDPClientConnection*)connection;
		conn->sendCloseSignal();
	});
}

FPAnswerPtr UDPClient::sendQuestEx(FPQuestPtr quest, bool discardable, int timeoutMsec)
{
	if (!_connected)
	{
		if (!_autoReconnect)
		{
			if (quest->isTwoWay())
				return FPAWriter::errorAnswer(quest, FPNN_EC_CORE_CONNECTION_CLOSED, "Client is not allowed auto-connected.");
			else
				return NULL;
		}

		if (!reconnect())
		{
			if (quest->isTwoWay())
				return FPAWriter::errorAnswer(quest, FPNN_EC_CORE_CONNECTION_CLOSED, "Reconnection failed.");
			else
				return NULL;
		}
	}

	ConnectionInfoPtr connInfo;
	{
		std::unique_lock<std::mutex> lck(_mutex);
		connInfo = _connectionInfo;
	}
	Config::ClientQuestLog(quest, connInfo->ip, connInfo->port);

	if (timeoutMsec == 0)
		return ClientEngine::instance()->sendQuest(connInfo->socket, connInfo->token, &_mutex, quest, (int)_timeoutQuest, discardable);
	else
		return ClientEngine::instance()->sendQuest(connInfo->socket, connInfo->token, &_mutex, quest, timeoutMsec, discardable);
}
bool UDPClient::sendQuestEx(FPQuestPtr quest, AnswerCallback* callback, bool discardable, int timeoutMsec)
{
	if (!_connected)
	{
		if (!_autoReconnect)
			return false;

		if (!reconnect())
			return false;
	}

	ConnectionInfoPtr connInfo;
	{
		std::unique_lock<std::mutex> lck(_mutex);
		connInfo = _connectionInfo;
	}
	Config::ClientQuestLog(quest, connInfo->ip, connInfo->port);

	if (timeoutMsec == 0)
		return ClientEngine::instance()->sendQuest(connInfo->socket, connInfo->token, quest, callback, (int)_timeoutQuest, discardable);
	else
		return ClientEngine::instance()->sendQuest(connInfo->socket, connInfo->token, quest, callback, timeoutMsec, discardable);
}
bool UDPClient::sendQuestEx(FPQuestPtr quest, std::function<void (FPAnswerPtr answer, int errorCode)> task, bool discardable, int timeoutMsec)
{
	if (!_connected)
	{
		if (!_autoReconnect)
			return false;

		if (!reconnect())
			return false;
	}

	ConnectionInfoPtr connInfo;
	{
		std::unique_lock<std::mutex> lck(_mutex);
		connInfo = _connectionInfo;
	}
	Config::ClientQuestLog(quest, connInfo->ip, connInfo->port);

	bool res;
	if (timeoutMsec == 0)
		res = ClientEngine::instance()->sendQuest(connInfo->socket, connInfo->token, quest, std::move(task), (int)_timeoutQuest, discardable);
	else
		res = ClientEngine::instance()->sendQuest(connInfo->socket, connInfo->token, quest, std::move(task), timeoutMsec, discardable);

	return res;
}

	/*===============================================================================
	  Interfaces for embed mode.
	=============================================================================== */
bool UDPClient::embed_sendData(std::string* rawData, bool discardable, int timeoutMsec)
{
	if (!_connected)
	{
		if (!_autoReconnect)
			return false;

		if (!reconnect())
			return false;
	}

	ConnectionInfoPtr connInfo;
	{
		std::unique_lock<std::mutex> lck(_mutex);
		connInfo = _connectionInfo;
	}
	//-- Config::ClientQuestLog(quest, connInfo->ip, connInfo->port);
	int64_t expiredMS = (timeoutMsec == 0) ? _timeoutQuest : timeoutMsec;
	if (expiredMS == 0)
		expiredMS = ClientEngine::getQuestTimeout() * 1000;

	expiredMS += slack_real_msec();

	ClientEngine::instance()->sendUDPData(connInfo->socket, connInfo->token, rawData, expiredMS, discardable);
	return true;
}

void UDPClient::keepAlive()
{
	_keepAlive = true;
	SOCKET connSocket = socket();
	if (connSocket)
		_engine->keepAlive(connSocket, true);
}

void UDPClient::setUntransmittedSeconds(int untransmittedSeconds)
{
	_untransmittedSeconds = untransmittedSeconds;
	SOCKET connSocket = socket();
	if (connSocket)
		_engine->setUDPUntransmittedSeconds(connSocket, untransmittedSeconds);
}
