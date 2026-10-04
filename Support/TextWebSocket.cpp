/**
 *  TextWebSocket.cpp
 *  ONScripter-RU
 *
 *  Local WebSocket server for streaming rendered text.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Support/TextWebSocket.hpp"
#include "Support/FileDefs.hpp"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifdef WIN32
using Socket = SOCKET;
using SocketLength = int;
static constexpr Socket InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
using SocketLength = socklen_t;
static constexpr Socket InvalidSocket = -1;
#endif

TextWebSocketServer textWebSocketServer;

namespace {

void closeSocket(Socket socket) {
	if (socket == InvalidSocket)
		return;
#ifdef WIN32
	closesocket(socket);
#else
	close(socket);
#endif
}

bool setNonBlocking(Socket socket) {
#ifdef WIN32
	u_long enabled = 1;
	return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
	int flags = fcntl(socket, F_GETFL, 0);
	return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool socketWouldBlock() {
#ifdef WIN32
	int error = WSAGetLastError();
	return error == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

uint32_t rotateLeft(uint32_t value, unsigned int amount) {
	return (value << amount) | (value >> (32 - amount));
}

std::array<uint8_t, 20> sha1(const std::string &input) {
	std::vector<uint8_t> message(input.begin(), input.end());
	uint64_t bitLength = static_cast<uint64_t>(message.size()) * 8;
	message.push_back(0x80);
	while ((message.size() % 64) != 56) message.push_back(0);
	for (int i = 7; i >= 0; --i)
		message.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));

	uint32_t h0 = 0x67452301;
	uint32_t h1 = 0xEFCDAB89;
	uint32_t h2 = 0x98BADCFE;
	uint32_t h3 = 0x10325476;
	uint32_t h4 = 0xC3D2E1F0;

	for (size_t offset = 0; offset < message.size(); offset += 64) {
		uint32_t words[80]{};
		for (int i = 0; i < 16; ++i) {
			size_t pos = offset + i * 4;
			words[i] = static_cast<uint32_t>(message[pos]) << 24 |
			           static_cast<uint32_t>(message[pos + 1]) << 16 |
			           static_cast<uint32_t>(message[pos + 2]) << 8 |
			           static_cast<uint32_t>(message[pos + 3]);
		}
		for (int i = 16; i < 80; ++i)
			words[i] = rotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);

		uint32_t a = h0;
		uint32_t b = h1;
		uint32_t c = h2;
		uint32_t d = h3;
		uint32_t e = h4;
		for (int i = 0; i < 80; ++i) {
			uint32_t f;
			uint32_t k;
			if (i < 20) {
				f = (b & c) | ((~b) & d);
				k = 0x5A827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDC;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}
			uint32_t temp = rotateLeft(a, 5) + f + e + k + words[i];
			e = d;
			d = c;
			c = rotateLeft(b, 30);
			b = a;
			a = temp;
		}
		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
	}

	std::array<uint8_t, 20> digest{};
	uint32_t hashes[]{h0, h1, h2, h3, h4};
	for (int i = 0; i < 5; ++i) {
		digest[i * 4]     = static_cast<uint8_t>(hashes[i] >> 24);
		digest[i * 4 + 1] = static_cast<uint8_t>(hashes[i] >> 16);
		digest[i * 4 + 2] = static_cast<uint8_t>(hashes[i] >> 8);
		digest[i * 4 + 3] = static_cast<uint8_t>(hashes[i]);
	}
	return digest;
}

std::string base64(const uint8_t *data, size_t size) {
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve((size + 2) / 3 * 4);
	for (size_t i = 0; i < size; i += 3) {
		uint32_t value = static_cast<uint32_t>(data[i]) << 16;
		if (i + 1 < size) value |= static_cast<uint32_t>(data[i + 1]) << 8;
		if (i + 2 < size) value |= data[i + 2];
		result.push_back(alphabet[(value >> 18) & 63]);
		result.push_back(alphabet[(value >> 12) & 63]);
		result.push_back(i + 1 < size ? alphabet[(value >> 6) & 63] : '=');
		result.push_back(i + 2 < size ? alphabet[value & 63] : '=');
	}
	return result;
}

std::string makeFrame(const std::string &message, uint8_t opcode = 1) {
	std::string frame;
	frame.push_back(static_cast<char>(0x80 | opcode));
	if (message.size() < 126) {
		frame.push_back(static_cast<char>(message.size()));
	} else if (message.size() <= 0xFFFF) {
		frame.push_back(126);
		frame.push_back(static_cast<char>(message.size() >> 8));
		frame.push_back(static_cast<char>(message.size()));
	} else {
		frame.push_back(127);
		uint64_t size = message.size();
		for (int i = 7; i >= 0; --i)
			frame.push_back(static_cast<char>(size >> (i * 8)));
	}
	frame += message;
	return frame;
}

std::string headerValue(const std::string &request, const char *header) {
	std::string name(header);
	auto pos = request.find(name);
	if (pos == std::string::npos)
		return {};
	pos += name.size();
	auto end = request.find("\r\n", pos);
	if (end == std::string::npos)
		return {};
	while (pos < end && (request[pos] == ' ' || request[pos] == '\t')) ++pos;
	while (end > pos && (request[end - 1] == ' ' || request[end - 1] == '\t')) --end;
	return request.substr(pos, end - pos);
}

} // namespace

struct TextWebSocketServer::Implementation {
	struct Client {
		Socket socket{InvalidSocket};
		bool connected{false};
		std::string input;
		std::string output;
		size_t sent{0};
	};

	uint16_t port{0};
	std::string host;
	SDL_atomic_t running{};
	SDL_Thread *thread{nullptr};
	SDL_mutex *queueMutex{nullptr};
	std::deque<std::string> messages;

	static int threadEntry(void *data) {
		return static_cast<Implementation *>(data)->run();
	}

	void queue(const std::string &text) {
		if (!queueMutex || !SDL_AtomicGet(&running))
			return;
		SDL_LockMutex(queueMutex);
		messages.push_back(text);
		SDL_UnlockMutex(queueMutex);
	}

	void drainMessages(std::vector<Client> &clients) {
		std::deque<std::string> pending;
		SDL_LockMutex(queueMutex);
		pending.swap(messages);
		SDL_UnlockMutex(queueMutex);
		for (auto &message : pending) {
			auto frame = makeFrame(message);
			for (auto &client : clients) {
				if (client.connected) {
					client.output += frame;
					if (client.output.size() - client.sent > 4 * 1024 * 1024)
						closeClient(client);
				}
			}
		}
	}

	void closeClient(Client &client) {
		closeSocket(client.socket);
		client.socket = InvalidSocket;
		client.connected = false;
	}

	bool handshake(Client &client) {
		auto requestEnd = client.input.find("\r\n\r\n");
		if (requestEnd == std::string::npos)
			return client.input.size() <= 16384;
		auto key = headerValue(client.input, "Sec-WebSocket-Key:");
		if (key.empty())
			return false;
		auto digest = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
		client.output = "HTTP/1.1 101 Switching Protocols\r\n"
		                "Upgrade: websocket\r\n"
		                "Connection: Upgrade\r\n"
		                "Sec-WebSocket-Accept: " + base64(digest.data(), digest.size()) + "\r\n\r\n";
		client.input.erase(0, requestEnd + 4);
		client.connected = true;
		return client.input.empty() || consumeFrames(client);
	}

	bool consumeFrames(Client &client) {
		while (client.input.size() >= 2) {
			const auto *data = reinterpret_cast<const uint8_t *>(client.input.data());
			uint8_t opcode = data[0] & 0x0F;
			bool masked = (data[1] & 0x80) != 0;
			uint64_t length = data[1] & 0x7F;
			size_t header = 2;
			if (length == 126) {
				if (client.input.size() < 4) return true;
				length = static_cast<uint64_t>(data[2]) << 8 | data[3];
				header = 4;
			} else if (length == 127) {
				if (client.input.size() < 10) return true;
				length = 0;
				for (int i = 2; i < 10; ++i) length = length << 8 | data[i];
				header = 10;
			}
			if (!masked || length > 1024 * 1024)
				return false;
			if (client.input.size() < header + 4 + length)
				return true;
			const uint8_t *mask = data + header;
			std::string payload(client.input.data() + header + 4, static_cast<size_t>(length));
			for (size_t i = 0; i < payload.size(); ++i)
				payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
			client.input.erase(0, header + 4 + static_cast<size_t>(length));
			if (opcode == 8)
				return false;
			if (opcode == 9)
				client.output += makeFrame(payload, 10);
		}
		return true;
	}

	bool readClient(Client &client) {
		char buffer[4096];
		int received = recv(client.socket, buffer, sizeof(buffer), 0);
		if (received == 0)
			return false;
		if (received < 0)
			return socketWouldBlock();
		client.input.append(buffer, static_cast<size_t>(received));
		return client.connected ? consumeFrames(client) : handshake(client);
	}

	bool writeClient(Client &client) {
		if (client.sent >= client.output.size())
			return true;
#ifdef LINUX
		int flags = MSG_NOSIGNAL;
#else
		int flags = 0;
#endif
		int written = send(client.socket, client.output.data() + client.sent,
		                   static_cast<int>(client.output.size() - client.sent), flags);
		if (written == 0)
			return false;
		if (written < 0)
			return socketWouldBlock();
		client.sent += static_cast<size_t>(written);
		if (client.sent == client.output.size()) {
			client.output.clear();
			client.sent = 0;
		}
		return true;
	}

	int run() {
#ifdef WIN32
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
			sendToLog(LogLevel::Error, "Unable to initialize the text WebSocket server\n");
			SDL_AtomicSet(&running, 0);
			return 1;
		}
#endif
		addrinfo hints{};
		hints.ai_family   = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		addrinfo *addresses{nullptr};
		std::string portString = std::to_string(port);
		if (getaddrinfo(host.c_str(), portString.c_str(), &hints, &addresses) != 0) {
			sendToLog(LogLevel::Error, "Unable to resolve text WebSocket host %s\n", host.c_str());
			SDL_AtomicSet(&running, 0);
#ifdef WIN32
			WSACleanup();
#endif
			return 1;
		}

		Socket listener{InvalidSocket};
		for (addrinfo *address = addresses; address; address = address->ai_next) {
			listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
			if (listener == InvalidSocket)
				continue;
			int reuse = 1;
			setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
			if (bind(listener, address->ai_addr, static_cast<SocketLength>(address->ai_addrlen)) == 0 &&
			    listen(listener, 8) == 0 && setNonBlocking(listener))
				break;
			closeSocket(listener);
			listener = InvalidSocket;
		}
		freeaddrinfo(addresses);
		if (listener == InvalidSocket) {
			sendToLog(LogLevel::Error, "Unable to listen for text WebSocket clients on %s:%u\n", host.c_str(), port);
			SDL_AtomicSet(&running, 0);
#ifdef WIN32
			WSACleanup();
#endif
			return 1;
		}

		std::string displayHost = host.find(':') == std::string::npos ? host : "[" + host + "]";
		sendToLog(LogLevel::Info, "Text WebSocket server listening at ws://%s:%u/\n", displayHost.c_str(), port);
		std::vector<Client> clients;
		while (SDL_AtomicGet(&running)) {
			drainMessages(clients);
			clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client &client) {
				return client.socket == InvalidSocket;
			}), clients.end());
			fd_set reads;
			fd_set writes;
			FD_ZERO(&reads);
			FD_ZERO(&writes);
			FD_SET(listener, &reads);
			Socket highest = listener;
			for (auto &client : clients) {
				FD_SET(client.socket, &reads);
				if (client.sent < client.output.size()) FD_SET(client.socket, &writes);
				highest = std::max(highest, client.socket);
			}
			timeval timeout{0, 100000};
			int ready = select(static_cast<int>(highest + 1), &reads, &writes, nullptr, &timeout);
			if (ready < 0)
				continue;
			if (FD_ISSET(listener, &reads)) {
				Socket clientSocket = accept(listener, nullptr, nullptr);
				if (clientSocket != InvalidSocket && setNonBlocking(clientSocket)) {
#ifdef MACOSX
					int noSignal = 1;
					setsockopt(clientSocket, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
					clients.emplace_back();
					clients.back().socket = clientSocket;
				} else {
					closeSocket(clientSocket);
				}
			}
			for (auto &client : clients) {
				if ((FD_ISSET(client.socket, &reads) && !readClient(client)) ||
				    (client.socket != InvalidSocket && FD_ISSET(client.socket, &writes) && !writeClient(client)))
					closeClient(client);
			}
			clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client &client) {
				return client.socket == InvalidSocket;
			}), clients.end());
		}

		for (auto &client : clients) closeClient(client);
		closeSocket(listener);
#ifdef WIN32
		WSACleanup();
#endif
		return 0;
	}
};

TextWebSocketServer::~TextWebSocketServer() {
	stop();
}

bool TextWebSocketServer::start(const std::string &host, uint16_t port) {
	if (implementation)
		return false;
	implementation = new Implementation;
	implementation->host = host;
	implementation->port = port;
	implementation->queueMutex = SDL_CreateMutex();
	if (!implementation->queueMutex) {
		delete implementation;
		implementation = nullptr;
		return false;
	}
	SDL_AtomicSet(&implementation->running, 1);
	implementation->thread = SDL_CreateThread(Implementation::threadEntry, "text-websocket", implementation);
	if (!implementation->thread) {
		SDL_DestroyMutex(implementation->queueMutex);
		delete implementation;
		implementation = nullptr;
		return false;
	}
	return true;
}

void TextWebSocketServer::stop() {
	if (!implementation)
		return;
	SDL_AtomicSet(&implementation->running, 0);
	SDL_WaitThread(implementation->thread, nullptr);
	SDL_DestroyMutex(implementation->queueMutex);
	delete implementation;
	implementation = nullptr;
}

void TextWebSocketServer::broadcast(const std::string &text) {
	if (implementation && !text.empty())
		implementation->queue(text);
}
