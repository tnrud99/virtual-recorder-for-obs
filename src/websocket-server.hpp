#pragma once

#include <libwebsockets.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

struct WsPerSessionData {
	struct lws *wsi;
};

class WebSocketServer {
public:
	using MessageCallback = std::function<bool(const std::string &)>;

	WebSocketServer();
	~WebSocketServer();

	bool start();
	void stop();
	int getPort() const;
	bool isRunning() const;

	void sendMessage(const std::string &msg);
	void setOnMessage(MessageCallback cb);

	// lws callback — public so the C callback trampoline can reach it
	int handleCallback(struct lws *wsi, enum lws_callback_reasons reason,
			   void *in, size_t len);

private:
	void serviceThread();

	struct lws_context *ctx = nullptr;
	std::thread thread;
	std::atomic<bool> running{false};
	int port = 0;

	// Connected client (single client expected)
	struct lws *clientWsi = nullptr;
	std::mutex clientMtx;

	// Outbound queue (main thread → lws thread)
	std::queue<std::string> outQueue;
	std::mutex outMtx;

	// Inbound callback
	MessageCallback onMessage;
	std::mutex cbMtx;
};
