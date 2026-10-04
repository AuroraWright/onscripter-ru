/**
 *  TextWebSocket.hpp
 *  ONScripter-RU
 *
 *  Local WebSocket server for streaming rendered text.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include <cstdint>
#include <string>

class TextWebSocketServer {
	struct Implementation;
	Implementation *implementation{nullptr};

public:
	~TextWebSocketServer();

	bool start(const std::string &host, uint16_t port);
	void stop();
	void broadcast(const std::string &text);
};

extern TextWebSocketServer textWebSocketServer;
