#include "websocket-server.hpp"
#include <obs.h>
#include <cstring>

// --- C trampoline for lws callback ---
static int ws_callback_trampoline(struct lws *wsi,
				  enum lws_callback_reasons reason,
				  void *user, void *in, size_t len)
{
	auto *vhost = lws_get_vhost(wsi);
	if (!vhost)
		return 0;
	auto *server = static_cast<WebSocketServer *>(
		lws_context_user(lws_get_context(wsi)));
	if (!server)
		return 0;
	return server->handleCallback(wsi, reason, in, len);
}

static const struct lws_protocols protocols[] = {
	{"virtuedit", ws_callback_trampoline, sizeof(WsPerSessionData), 4096},
	LWS_PROTOCOL_LIST_TERM};

// --- WebSocketServer ---

WebSocketServer::WebSocketServer() = default;

WebSocketServer::~WebSocketServer()
{
	stop();
}

bool WebSocketServer::start()
{
	if (running.load())
		return true;

	struct lws_context_creation_info info {};
	info.port = 0; // kernel picks free port
	info.iface = "127.0.0.1"; // localhost only (Win + Mac)
	info.protocols = protocols;
	info.user = this;
	info.options = LWS_SERVER_OPTION_DISABLE_IPV6;

	ctx = lws_create_context(&info);
	if (!ctx) {
		blog(LOG_ERROR, "[VirtuEdit] Failed to create lws context");
		return false;
	}

	struct lws_vhost *vh = lws_get_vhost_by_name(ctx, "default");
	if (!vh)
		vh = lws_get_vhost_by_name(ctx, "virtuedit");
	port = vh ? lws_get_vhost_listen_port(vh) : 0;

	if (port <= 0) {
		blog(LOG_ERROR, "[VirtuEdit] Failed to get listen port");
		lws_context_destroy(ctx);
		ctx = nullptr;
		return false;
	}

	running.store(true);
	thread = std::thread(&WebSocketServer::serviceThread, this);

	blog(LOG_INFO, "[VirtuEdit] WebSocket server started on port %d",
	     port);
	return true;
}

void WebSocketServer::stop()
{
	if (!running.load())
		return;

	running.store(false);
	if (ctx)
		lws_cancel_service(ctx);

	if (thread.joinable())
		thread.join();

	blog(LOG_INFO, "[VirtuEdit] WebSocket server stopped");
}

int WebSocketServer::getPort() const
{
	return port;
}

bool WebSocketServer::isRunning() const
{
	return running.load();
}

void WebSocketServer::sendMessage(const std::string &msg)
{
	{
		std::lock_guard<std::mutex> lock(outMtx);
		outQueue.push(msg);
	}
	if (ctx)
		lws_cancel_service(ctx);
}

void WebSocketServer::setOnMessage(MessageCallback cb)
{
	std::lock_guard<std::mutex> lock(cbMtx);
	onMessage = std::move(cb);
}

int WebSocketServer::handleCallback(struct lws *wsi,
				    enum lws_callback_reasons reason,
				    void *in, size_t len)
{
	switch (reason) {
	case LWS_CALLBACK_ESTABLISHED: {
		std::lock_guard<std::mutex> lock(clientMtx);
		clientWsi = wsi;
		blog(LOG_INFO, "[VirtuEdit] WebSocket client connected");
		break;
	}

	case LWS_CALLBACK_RECEIVE: {
		std::string msg(static_cast<const char *>(in), len);
		std::lock_guard<std::mutex> lock(cbMtx);
		if (onMessage && !onMessage(msg))
			return -1;
		break;
	}

	case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
		std::lock_guard<std::mutex> lock(outMtx);
		std::lock_guard<std::mutex> cLock(clientMtx);
		if (!clientWsi)
			break;
		while (!outQueue.empty()) {
			auto &msg = outQueue.front();
			size_t needed = LWS_PRE + msg.size();
			auto *buf = new unsigned char[needed];
			std::memcpy(&buf[LWS_PRE], msg.data(), msg.size());
			lws_write(clientWsi, &buf[LWS_PRE], msg.size(),
				  LWS_WRITE_TEXT);
			delete[] buf;
			outQueue.pop();
		}
		break;
	}

	case LWS_CALLBACK_CLOSED: {
		std::lock_guard<std::mutex> lock(clientMtx);
		if (clientWsi == wsi)
			clientWsi = nullptr;
		blog(LOG_INFO, "[VirtuEdit] WebSocket client disconnected");
		break;
	}

	default:
		break;
	}
	return 0;
}

void WebSocketServer::serviceThread()
{
	while (running.load()) {
		if (ctx)
			lws_service(ctx, 250);
	}
	if (ctx) {
		lws_context_destroy(ctx);
		ctx = nullptr;
	}
}
