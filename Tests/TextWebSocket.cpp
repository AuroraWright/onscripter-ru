/**
 *  TextWebSocket.cpp
 *  ONScripter-RU
 *
 *  Protocol, resource limit, and script-selection regression tests.
 *  Run with Scripts/test_text_websocket_server.sh.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Support/TextWebSocket.cpp"
#include "Support/TextStreamSelection.hpp"
#include "Support/TextImageGeometry.hpp"
#include "Support/WindowGeometry.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>

void sendToLog(LogLevel, const char *, ...) {}

static void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(1);
	}
}

static std::string maskedFrame(const std::string &payload, uint8_t first) {
	std::string frame = makeFrame(payload, first & 15);
	frame[0] = static_cast<char>(first);
	frame[1] = static_cast<char>(static_cast<uint8_t>(frame[1]) | 128);
	size_t header = payload.size() < 126 ? 2 : payload.size() <= 65535 ? 4 : 10;
	const uint8_t mask[]{0x12, 0x34, 0x56, 0x78};
	frame.insert(header, reinterpret_cast<const char *>(mask), 4);
	for (size_t i = 0; i < payload.size(); ++i)
		frame[header + 4 + i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
	return frame;
}

static std::string request(const std::string &keyHeader = "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==") {
	return "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n" +
	       keyHeader + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
}

struct TextWebSocketServerTests {
	using Server = TextWebSocketServer::Implementation;
	using Client = Server::Client;

	static void expectClose(Server &server, const std::string &frame, uint16_t code) {
		Client client;
		client.connected = true;
		client.input = frame;
		check(server.consumeFrames(client), "queue protocol close");
		std::string payload{static_cast<char>(code >> 8), static_cast<char>(code)};
		check(client.closing && client.output == makeFrame(payload, 8), "correct close status");
	}

	static void unit() {
		Server server;
		Client normal;
		normal.input = request();
		check(server.handshake(normal) && normal.connected, "standard handshake");
		check(normal.output.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos, "RFC handshake digest");
		Client lower;
		lower.input = request("sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==");
		check(server.handshake(lower) && lower.connected, "lowercase header");
		Client incremental;
		auto valid = request();
		for (char ch : valid) {
			incremental.input += ch;
			check(server.handshake(incremental), "incremental handshake");
		}
		check(incremental.connected, "incremental handshake completed");
		for (const auto &bad : {request("X-Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="),
		                       request("Sec-WebSocket-Key: invalid"),
		                       request("Sec-WebSocket-Key : dGhlIHNhbXBsZSBub25jZQ=="),
		                       request("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Key: duplicate"),
		                       std::string("POST / HTTP/1.0\r\nSec-WebSocket-Key: invalid\r\n\r\n"),
		                       std::string(Server::MaxHandshake + 1, 'x') + "\r\n\r\n"}) {
			Client client;
			client.input = bad;
			check(server.handshake(client) && !client.connected && client.closing, "reject invalid handshake");
			check(client.output.find("400 Bad Request") != std::string::npos, "HTTP handshake rejection");
		}
		Client missing;
		missing.input = "GET / HTTP/1.1\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
		check(server.handshake(missing) && missing.closing, "missing upgrade headers");
		Client version;
		version.input = request();
		version.input.replace(version.input.find("Version: 13"), 11, "Version: 12");
		check(server.handshake(version) && !version.connected && version.closing, "unsupported WebSocket version");
		check(version.output.find("426 Upgrade Required") != std::string::npos &&
		      version.output.find("Sec-WebSocket-Version: 13") != std::string::npos, "version negotiation response");
		for (size_t length : {size_t{0}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}}) {
			Client client;
			client.input = maskedFrame(std::string(length, 'x'), 0x81);
			check(server.consumeFrames(client) && !client.closing && client.input.empty(), "frame length boundary");
		}
		expectClose(server, maskedFrame(std::string(126, 'x'), 0x89), 1002);
		expectClose(server, maskedFrame("x", 0x09), 1002);
		expectClose(server, maskedFrame("", 0x83), 1002);
		expectClose(server, maskedFrame("", 0xC1), 1002);
		expectClose(server, makeFrame("unmasked"), 1002);
		expectClose(server, maskedFrame("", 0x80), 1002);
		expectClose(server, maskedFrame("x", 0x88), 1002);
		expectClose(server, maskedFrame(std::string("\x03\xED", 2), 0x88), 1002);
		expectClose(server, maskedFrame(std::string("\xC0\x80", 2), 0x81), 1007);
		expectClose(server, maskedFrame(std::string(Server::MaxInput + 1, 'x'), 0x82), 1009);
		Client fragmented;
		fragmented.input = maskedFrame(std::string("\xE3", 1), 0x01) + maskedFrame("ping", 0x89) +
		                   maskedFrame(std::string("\x81\x82", 2), 0x80);
		check(server.consumeFrames(fragmented) && !fragmented.closing && !fragmented.fragmentedOpcode,
		      "fragmented UTF-8 with interleaved ping");
		check(fragmented.output == makeFrame("ping", 10), "pong echoes ping");
		Client coalesced;
		coalesced.connected = true;
		auto largeFrame = maskedFrame(std::string(Server::MaxInput, 'x'), 0x82);
		coalesced.input = largeFrame.substr(0, largeFrame.size() - 1);
		check(server.consumeInput(coalesced) && !coalesced.closing, "partial maximum-sized frame");
		coalesced.input += largeFrame.substr(largeFrame.size() - 1) + maskedFrame("ping", 0x89);
		check(coalesced.input.size() > Server::MaxInput + 14, "coalesced input exceeds single-frame limit");
		check(server.consumeInput(coalesced) && !coalesced.closing && coalesced.input.empty(),
		      "parse maximum-sized frame before checking remaining input");
		check(coalesced.output == makeFrame("ping", 10), "coalesced ping receives pong");
		Client closing;
		closing.input = maskedFrame(std::string("\x03\xE8", 2) + "done", 0x88);
		check(server.consumeFrames(closing) && closing.closing, "close enters closing state");
		check(closing.output == makeFrame(std::string("\x03\xE8", 2) + "done", 8), "close reply");
		Client flooded;
		auto ping = maskedFrame(std::string(125, 'x'), 0x89);
		bool accepted = true;
		for (int i = 0; i < 40000 && accepted; ++i) {
			flooded.input = ping;
			accepted = server.consumeFrames(flooded);
		}
		check(!accepted && flooded.output.size() <= Server::MaxOutput, "bounded pong queue");
		Client partial;
		for (int i = 0; i < 5000; ++i) {
			check(server.appendOutput(partial, std::string(1024, 'x')), "append after partial write");
			partial.sent = partial.output.size() - 1;
		}
		check(partial.output.size() == 1025, "discard sent output prefix");
		server.queueMutex = SDL_CreateMutex();
		check(server.queueMutex != nullptr, "queue mutex");
		SDL_AtomicSet(&server.running, 1);
		for (int i = 0; i < 2000; ++i) server.queue("x");
		check(server.messages.size() == 1024, "bounded producer message count");
		server.messages.clear();
		server.queuedBytes = 0;
		server.queue(std::string(Server::MaxOutput, 'x'));
		server.queue("x");
		check(server.queuedBytes == Server::MaxOutput && server.messages.size() == 1, "bounded producer bytes");
		SDL_DestroyMutex(server.queueMutex);
#ifndef WIN32
		check(!selectableSocket(FD_SETSIZE) && !selectableSocket(-1), "reject unselectable descriptor");
#endif
		check(Server::MaxClients + 1 <= FD_SETSIZE, "connection cap fits descriptor set");
		std::puts("Protocol and resource-limit tests passed");
	}

	static void integration() {
#ifdef WIN32
		WSADATA data;
		check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "test Winsock startup");
#endif
		Socket reservation = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		check(reservation != InvalidSocket, "create port reservation");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		check(bind(reservation, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "reserve test port");
		SocketLength size = sizeof(address);
		check(getsockname(reservation, reinterpret_cast<sockaddr *>(&address), &size) == 0, "read test port");
		uint16_t port = ntohs(address.sin_port);
		TextWebSocketServer occupied;
		check(!occupied.start("127.0.0.1", port), "occupied port reports startup failure");
		closeSocket(reservation);
		TextWebSocketServer server;
		check(server.start("127.0.0.1", port), "start loopback server");
		Socket client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		check(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect test client");
		check(setNonBlocking(client), "nonblocking test client");
		std::string received;
		auto receiveUntil = [&](const std::string &expected, uint32_t timeout = 2000) {
			uint32_t start = SDL_GetTicks();
			while (SDL_GetTicks() - start < timeout) {
				char buffer[4096];
				int count = recv(client, buffer, sizeof(buffer), 0);
				if (count > 0) received.append(buffer, static_cast<size_t>(count));
				if (received.find(expected) != std::string::npos) return true;
				if (count == 0) break;
				SDL_Delay(1);
			}
			return false;
		};
		auto handshake = request("sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==");
		check(send(client, handshake.data(), static_cast<int>(handshake.size()), 0) == static_cast<int>(handshake.size()), "send handshake");
		check(receiveUntil("\r\n\r\n"), "receive handshake response");
		received.clear();
		server.broadcast("日本語 dialogue");
		check(receiveUntil(makeFrame("日本語 dialogue")), "broadcast UTF-8 text");
		received.clear();
		auto close = maskedFrame("", 0x88);
		check(send(client, close.data(), static_cast<int>(close.size()), 0) == static_cast<int>(close.size()), "send close");
		check(receiveUntil(makeFrame("", 8)), "receive close response before disconnect");
		closeSocket(client);
		std::vector<Socket> idle;
		for (size_t i = 0; i < Server::MaxClients; ++i) {
			Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			check(connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect idle client");
			check(setNonBlocking(socket), "nonblocking idle client");
			idle.push_back(socket);
		}
		SDL_Delay(200);
		Socket excess = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		check(connect(excess, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect excess client");
		check(setNonBlocking(excess), "nonblocking excess client");
		auto disconnected = [](Socket socket, uint32_t timeout) {
			uint32_t start = SDL_GetTicks();
			while (SDL_GetTicks() - start < timeout) {
				char byte;
				int result = recv(socket, &byte, 1, 0);
				if (result == 0 || (result < 0 && !socketWouldBlock())) return true;
				SDL_Delay(1);
			}
			return false;
		};
		check(disconnected(excess, 2000), "excess client rejected");
		closeSocket(excess);
		check(disconnected(idle.front(), Server::HandshakeTimeout + 1000), "idle handshake timeout");
		for (Socket socket : idle) closeSocket(socket);
		server.stop();
		check(server.start("127.0.0.1", port), "restart after shutdown");
		server.stop();
#ifdef WIN32
		WSACleanup();
#endif
		std::puts("Loopback startup, broadcast, close, admission, and timeout tests passed");
	}
};

static void selectionTests() {
	TextStreamSelection selection;
	selection.mark(0);
	{
		TextStreamSelection::Scope callback(selection, "mov", 1);
		check(!selection.requested(), "callback cannot consume caller's pending annotation");
	}
	selection.mark(1);
	{
		TextStreamSelection::Scope callbackText(selection, "lsp", 1);
		check(selection.consume(), "callback can use its own annotation");
	}
	{
		TextStreamSelection::Scope callbackReturn(selection, "return", 1);
		check(!selection.consume(), "callback return does not steal caller's annotation");
	}
	{
		TextStreamSelection::Scope target(selection, "lsp", 0);
		check(selection.consume(), "caller annotation survives callback execution");
	}
	selection.mark();
	{
		TextStreamSelection::Scope newline(selection, "\n");
		check(!selection.requested(), "newline does not activate marker");
	}
	{
		TextStreamSelection::Scope command(selection, "lsp");
		check(selection.requested(), "marked command selected");
		check(!selection.consume(false) && selection.requested(), "scrollable redraw cannot steal sprite selection");
		{
			TextStreamSelection::Scope nested(selection, "unrelated callback");
			check(!selection.consume(), "nested unrelated command excluded");
		}
		check(selection.consume() && !selection.consume(), "consume marker exactly once");
	}
	selection.mark();
	{
		TextStreamSelection::Scope cachedOrEmpty(selection, "lsp");
		check(selection.requested(), "cached or empty target selected");
	}
	{
		TextStreamSelection::Scope unrelated(selection, "lsp");
		check(!selection.consume(), "unused marker cannot leak to following command");
	}
	selection.mark();
	{
		TextStreamSelection::Scope command(selection, "wait");
		{
			TextStreamSelection::Scope nested(selection, "reset");
			selection.clear();
		}
		check(!selection.requested(), "reset does not restore outer selection");
	}
	selection.mark();
	selection.clear();
	{
		TextStreamSelection::Scope command(selection, "lsp");
		check(!selection.consume(), "reset clears pending marker");
	}
	std::puts("Script-selection tests passed");
}

static void imageGeometryTests() {
	check(!useComputedImageCenter(0, false, false, false), "ordinary untransformed sprites keep their original anchor");
	check(useComputedImageCenter(0, false, true, false) && useComputedImageCenter(0, false, true, true),
	      "custom scale centers use computed anchors with and without ruby padding");
	check(useComputedImageCenter(90, false, false, false) && useComputedImageCenter(0, true, false, false) &&
	      useComputedImageCenter(0, false, false, true), "rotation, hotspots and padding use computed anchors");
	for (bool hotspot : {false, true}) {
		for (auto padding : {std::array<float, 4>{{20, 10, 20, 10}},
		                     std::array<float, 4>{{30, 8, 0, 0}},
		                     std::array<float, 4>{{0, 0, 16, 24}},
		                     std::array<float, 4>{{0, 0, 0, 0}}}) {
			float left = padding[0], top = padding[1];
			float extraWidth = left + padding[2], extraHeight = top + padding[3];
			auto offset = textImageCenterOffset(-left, -top, extraWidth, extraHeight, hotspot);
			for (float angle : {0.0f, 90.0f, 35.0f}) {
				for (float scaleX : {1.0f, 2.0f, -1.5f}) {
					float radians = angle * 3.14159265358979323846f / 180;
					float a = std::cos(radians) * scaleX, b = -std::sin(radians) * 0.75f;
					float c = std::sin(radians) * scaleX, d = std::cos(radians) * 0.75f;
					float oldX = hotspot ? 7 : 7 - 100 / 2.0f;
					float oldY = hotspot ? 5 : 5 - 40 / 2.0f;
					float newX = hotspot ? 7 + left : 7 + left - (100 + extraWidth) / 2;
					float newY = hotspot ? 5 + top : 5 + top - (40 + extraHeight) / 2;
					check(std::fabs(a * oldX + b * oldY - (a * (newX + offset[0]) + b * (newY + offset[1]))) < 0.0001f &&
					      std::fabs(c * oldX + d * oldY - (c * (newX + offset[0]) + d * (newY + offset[1]))) < 0.0001f,
					      "ruby padding preserves centered and hotspot anchors under transforms");
				}
			}
		}
	}
	std::puts("Text-image geometry tests passed");
}

static void windowGeometryTests() {
	check(fullscreenToWindowedCoordinate(800, 1440, 1080, 1080, 60) == 540,
	      "fullscreen exit subtracts vertical letterboxing at fullscreen scale");
	check(fullscreenToWindowedCoordinate(1280, 1920, 1920, 1920, 320) == 960,
	      "fullscreen exit subtracts horizontal letterboxing without stretching");
	check(fullscreenToWindowedCoordinate(800, 1440, 540, 1080, 60) == 270,
	      "fullscreen exit respects a smaller windowed size");
	check(fullscreenToWindowedCoordinate(80, 1440, 1080, 1080, 60) == 0 &&
	      fullscreenToWindowedCoordinate(1520, 1440, 1080, 1080, 60) == 1080,
	      "fullscreen content edges map to windowed content edges");
	check(fullscreenToWindowedCoordinate(640, 2560, 1920, 1920, 0) == 480,
	      "fullscreen exit with no letterboxing preserves proportional position");
	std::puts("Window geometry tests passed");
}

int main(int argc, char **argv) {
	check(SDL_Init(SDL_INIT_TIMER) == 0, "SDL timer initialization");
	TextWebSocketServerTests::unit();
	selectionTests();
	imageGeometryTests();
	windowGeometryTests();
	if (argc < 2 || std::strcmp(argv[1], "--unit") != 0) TextWebSocketServerTests::integration();
	SDL_Quit();
	return 0;
}
