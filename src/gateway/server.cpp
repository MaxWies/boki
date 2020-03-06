#include "gateway/server.h"

#include "utils/uv_utils.h"

#define HLOG(l) LOG(l) << "Server: "
#define HVLOG(l) VLOG(l) << "Server: "

namespace faas {
namespace gateway {

using protocol::Status;
using protocol::HandshakeMessage;
using protocol::HandshakeResponse;

Server::Server()
    : state_(kCreated), port_(-1), listen_backlog_(kDefaultListenBackLog),
      num_http_workers_(kDefaultNumHttpWorkers), num_ipc_workers_(kDefaultNumIpcWorkers),
      event_loop_thread_("Server_EventLoop", std::bind(&Server::EventLoopThreadMain, this)),
      next_http_connection_id_(0), next_http_worker_id_(0), next_ipc_worker_id_(0) {
    UV_CHECK_OK(uv_loop_init(&uv_loop_));
    uv_loop_.data = &event_loop_thread_;
    UV_CHECK_OK(uv_tcp_init(&uv_loop_, &uv_tcp_handle_));
    uv_tcp_handle_.data = this;
    UV_CHECK_OK(uv_async_init(&uv_loop_, &stop_event_, &Server::StopCallback));
    stop_event_.data = this;
}

Server::~Server() {
    State state = state_.load();
    CHECK(state == kCreated || state == kStopped);
    UV_CHECK_OK(uv_loop_close(&uv_loop_));
}

namespace {
void PipeReadBufferAllocCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    size_t buf_size = 256;
    buf->base = reinterpret_cast<char*>(malloc(buf_size));
    buf->len = buf_size;
}
}

void Server::Start() {
    CHECK(state_.load() == kCreated);
    // Start IO workers
    for (int i = 0; i < num_http_workers_; i++) {
        auto io_worker = CreateAndStartIOWorker(absl::StrFormat("HttpWorker-%d", i));
        http_workers_.push_back(io_worker.get());
        io_workers_.push_back(std::move(io_worker));
    }
    for (int i = 0; i < num_ipc_workers_; i++) {
        auto io_worker = CreateAndStartIOWorker(absl::StrFormat("IpcWorker-%d", i));
        ipc_workers_.push_back(io_worker.get());
        io_workers_.push_back(std::move(io_worker));
    }
    // Listen on address:port
    struct sockaddr_in bind_addr;
    CHECK(!address_.empty());
    CHECK_NE(port_, -1);
    UV_CHECK_OK(uv_ip4_addr(address_.c_str(), port_, &bind_addr));
    UV_CHECK_OK(uv_tcp_bind(&uv_tcp_handle_, (const struct sockaddr *)&bind_addr, 0));
    HLOG(INFO) << "Listen on " << address_ << ":" << port_;
    UV_CHECK_OK(uv_listen(
        reinterpret_cast<uv_stream_t*>(&uv_tcp_handle_), listen_backlog_,
        &Server::HttpConnectionCallback));
    // Listen on ipc_path
    UV_CHECK_OK(uv_pipe_init(&uv_loop_, &uv_ipc_handle_, 0));
    uv_ipc_handle_.data = this;
    unlink(ipc_path_.c_str());
    UV_CHECK_OK(uv_pipe_bind(&uv_ipc_handle_, ipc_path_.c_str()));
    HLOG(INFO) << "Listen on " << ipc_path_ << " for IPC with watchdog processes";
    UV_CHECK_OK(uv_listen(
        reinterpret_cast<uv_stream_t*>(&uv_ipc_handle_), listen_backlog_,
        &Server::MessageConnectionCallback));
    // Start thread for running event loop
    event_loop_thread_.Start();
    state_.store(kRunning);
}

void Server::ScheduleStop() {
    HLOG(INFO) << "Scheduled to stop";
    UV_CHECK_OK(uv_async_send(&stop_event_));
}

void Server::WaitForFinish() {
    CHECK(state_.load() != kCreated);
    for (const auto& io_worker : io_workers_) {
        io_worker->WaitForFinish();
    }
    event_loop_thread_.Join();
    CHECK(state_.load() == kStopped);
    HLOG(INFO) << "Stopped";
}

void Server::RegisterSyncRequestHandler(RequestMatcher matcher, SyncRequestHandler handler) {
    CHECK(state_.load() == kCreated);
    request_handlers_.emplace_back(new RequestHandler(std::move(matcher), std::move(handler)));
}

void Server::RegisterAsyncRequestHandler(RequestMatcher matcher, AsyncRequestHandler handler) {
    CHECK(state_.load() == kCreated);
    request_handlers_.emplace_back(new RequestHandler(std::move(matcher), std::move(handler)));
}

bool Server::MatchRequest(absl::string_view method, absl::string_view path,
                          const RequestHandler** request_handler) const {
    for (const std::unique_ptr<RequestHandler>& entry : request_handlers_) {
        if (entry->matcher_(method, path)) {
            *request_handler = entry.get();
            return true;
        }
    }
    return false;
}

void Server::EventLoopThreadMain() {
    HLOG(INFO) << "Event loop starts";
    int ret = uv_run(&uv_loop_, UV_RUN_DEFAULT);
    if (ret != 0) {
        HLOG(WARNING) << "uv_run returns non-zero value: " << ret;
    }
    HLOG(INFO) << "Event loop finishes";
    state_.store(kStopped);
}

IOWorker* Server::PickHttpWorker() {
    IOWorker* io_worker = http_workers_[next_http_worker_id_];
    next_http_worker_id_ = (next_http_worker_id_ + 1) % http_workers_.size();
    return io_worker;
}

IOWorker* Server::PickIpcWorker() {
    IOWorker* io_worker = ipc_workers_[next_ipc_worker_id_];
    next_ipc_worker_id_ = (next_ipc_worker_id_ + 1) % ipc_workers_.size();
    return io_worker;
}

std::unique_ptr<IOWorker> Server::CreateAndStartIOWorker(absl::string_view worker_name) {
    std::unique_ptr<IOWorker> io_worker = absl::make_unique<IOWorker>(this, worker_name);
    int pipe_fd_for_worker;
    pipes_to_io_worker_[io_worker.get()] = CreatePipeToWorker(&pipe_fd_for_worker);
    uv_pipe_t* pipe_to_worker = pipes_to_io_worker_[io_worker.get()].get();
    UV_CHECK_OK(uv_read_start(reinterpret_cast<uv_stream_t*>(pipe_to_worker),
                                &PipeReadBufferAllocCallback,
                                &Server::ReturnConnectionCallback));
    io_worker->Start(pipe_fd_for_worker);
    return io_worker;
}

std::unique_ptr<uv_pipe_t> Server::CreatePipeToWorker(int* pipe_fd_for_worker) {
    int pipe_fds[2];
    // pipe2 does not work with uv_write2, use socketpair instead
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_fds), 0);
    std::unique_ptr<uv_pipe_t> pipe_to_worker = absl::make_unique<uv_pipe_t>();
    UV_CHECK_OK(uv_pipe_init(&uv_loop_, pipe_to_worker.get(), 1));
    pipe_to_worker->data = this;
    UV_CHECK_OK(uv_pipe_open(pipe_to_worker.get(), pipe_fds[0]));
    *pipe_fd_for_worker = pipe_fds[1];
    return pipe_to_worker;
}

void Server::TransferConnectionToWorker(IOWorker* io_worker, Connection* connection,
                                        uv_stream_t* send_handle) {
    CHECK_IN_EVENT_LOOP_THREAD(&uv_loop_);
    uv_write_t* write_req = connection->uv_write_req_for_transfer();
    size_t buf_len = sizeof(void*);
    char* buf = connection->pipe_write_buf_for_transfer();
    memcpy(buf, &connection, buf_len);
    uv_buf_t uv_buf = uv_buf_init(buf, buf_len);
    uv_pipe_t* pipe_to_worker = pipes_to_io_worker_[io_worker].get();
    UV_CHECK_OK(uv_write2(write_req, reinterpret_cast<uv_stream_t*>(pipe_to_worker),
                          &uv_buf, 1, reinterpret_cast<uv_stream_t*>(send_handle),
                          &PipeWrite2Callback));
    uv_close(reinterpret_cast<uv_handle_t*>(send_handle), nullptr);
}

void Server::ReturnConnection(Connection* connection) {
    CHECK_IN_EVENT_LOOP_THREAD(&uv_loop_);
    if (connection->type() == Connection::Type::Http) {
        HttpConnection* http_connection = static_cast<HttpConnection*>(connection);
        free_http_connections_.push_back(http_connection);
        active_http_connections_.erase(http_connection);
        HLOG(INFO) << "HttpConnection with ID " << http_connection->id() << " is returned, "
                   << "free connection count is " << free_http_connections_.size() << ", "
                   << "active connection is " << active_http_connections_.size();
    } else if (connection->type() == Connection::Type::Message) {
        MessageConnection* message_connection = static_cast<MessageConnection*>(connection);
        message_connections_.erase(message_connection);
    } else {
        LOG(FATAL) << "Unknown connection type!";
    }
}

void Server::OnNewHandshake(MessageConnection* connection,
                            const HandshakeMessage& message, HandshakeResponse* response) {
    response->status = static_cast<uint16_t>(Status::OK);
}

UV_CONNECTION_CB_FOR_CLASS(Server, HttpConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open HTTP connection: " << uv_strerror(status);
        return;
    }
    HttpConnection* connection;
    if (!free_http_connections_.empty()) {
        connection = free_http_connections_.back();
        free_http_connections_.pop_back();
        connection->Reset(next_http_connection_id_++);
    } else {
        http_connections_.push_back(absl::make_unique<HttpConnection>(this, next_http_connection_id_++));
        connection = http_connections_.back().get();
        HLOG(INFO) << "Allocate new HttpConnection object, current count is " << http_connections_.size();
    }
    uv_tcp_t client;
    UV_CHECK_OK(uv_tcp_init(&uv_loop_, &client));
    UV_CHECK_OK(uv_accept(reinterpret_cast<uv_stream_t*>(&uv_tcp_handle_),
                          reinterpret_cast<uv_stream_t*>(&client)));
    TransferConnectionToWorker(PickHttpWorker(), connection, reinterpret_cast<uv_stream_t*>(&client));
    active_http_connections_.insert(connection);
}

UV_CONNECTION_CB_FOR_CLASS(Server, MessageConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open message connection: " << uv_strerror(status);
        return;
    }
    HLOG(INFO) << "New message connection";
    std::unique_ptr<MessageConnection> connection = absl::make_unique<MessageConnection>(this);
    uv_pipe_t client;
    UV_CHECK_OK(uv_pipe_init(&uv_loop_, &client, 0));
    UV_CHECK_OK(uv_accept(reinterpret_cast<uv_stream_t*>(&uv_ipc_handle_),
                          reinterpret_cast<uv_stream_t*>(&client)));
    TransferConnectionToWorker(PickIpcWorker(), connection.get(),
                               reinterpret_cast<uv_stream_t*>(&client));
    message_connections_.insert(std::move(connection));
}

UV_READ_CB_FOR_CLASS(Server, ReturnConnection) {
    if (nread < 0) {
        if (nread == UV_EOF) {
            HLOG(WARNING) << "Pipe is closed by the corresponding IO worker";
        } else {
            HLOG(ERROR) << "Failed to read from pipe: " << uv_strerror(nread);
        }
    }
    if (nread <= 0) return;
    size_t remaing_length = nread;
    char* new_data = buf->base;
    size_t ptr_size = sizeof(void*);
    while (remaing_length + return_connection_read_buffer_.length() >= ptr_size) {
        size_t copy_size = ptr_size - return_connection_read_buffer_.length();
        return_connection_read_buffer_.AppendData(new_data, copy_size);
        CHECK_EQ(return_connection_read_buffer_.length(), ptr_size);
        Connection* connection;
        memcpy(&connection, return_connection_read_buffer_.data(), ptr_size);
        ReturnConnection(connection);
        return_connection_read_buffer_.Reset();
        remaing_length -= copy_size;
        new_data += copy_size;
    }
    if (remaing_length > 0) {
        return_connection_read_buffer_.AppendData(new_data, remaing_length);
    }
    free(buf->base);
}

UV_ASYNC_CB_FOR_CLASS(Server, Stop) {
    if (state_.load(std::memory_order_consume) == kStopping) {
        HLOG(WARNING) << "Already in stopping state";
        return;
    }
    HLOG(INFO) << "Start stopping process";
    for (const auto& io_worker : io_workers_) {
        io_worker->ScheduleStop();
        uv_pipe_t* pipe = pipes_to_io_worker_[io_worker.get()].get();
        UV_CHECK_OK(uv_read_stop(reinterpret_cast<uv_stream_t*>(pipe)));
        uv_close(reinterpret_cast<uv_handle_t*>(pipe), nullptr);
    }
    for (const auto& message_connection : message_connections_) {
        message_connection->ScheduleClose();
    }
    uv_close(reinterpret_cast<uv_handle_t*>(&uv_tcp_handle_), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&uv_ipc_handle_), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_event_), nullptr);
    state_.store(kStopping);
}

UV_WRITE_CB_FOR_CLASS(Server, PipeWrite2) {
    if (status != 0) {
        HLOG(ERROR) << "Failed to write to pipe: " << uv_strerror(status);
    }
}

}  // namespace gateway
}  // namespace faas