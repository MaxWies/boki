#include "gateway/server.h"

#include "common/time.h"
#include "utils/fs.h"

#include <absl/strings/match.h>
#include <absl/strings/strip.h>
#include <absl/strings/numbers.h>

#define HLOG(l) LOG(l) << "Server: "
#define HVLOG(l) VLOG(l) << "Server: "

namespace faas {
namespace gateway {

using protocol::Status;
using protocol::Role;
using protocol::HandshakeMessage;
using protocol::HandshakeResponse;
using protocol::FuncCall;
using protocol::MessageType;
using protocol::Message;

constexpr int Server::kDefaultListenBackLog;
constexpr int Server::kDefaultNumHttpWorkers;
constexpr int Server::kDefaultNumIpcWorkers;
constexpr size_t Server::kHttpConnectionBufferSize;
constexpr size_t Server::kMessageConnectionBufferSize;

Server::Server()
    : state_(kCreated), port_(-1), grpc_port_(-1), listen_backlog_(kDefaultListenBackLog),
      num_http_workers_(kDefaultNumHttpWorkers), num_ipc_workers_(kDefaultNumIpcWorkers),
      event_loop_thread_("Server_EventLoop", std::bind(&Server::EventLoopThreadMain, this)),
      next_http_connection_id_(0), next_grpc_connection_id_(0),
      next_http_worker_id_(0), next_ipc_worker_id_(0), next_client_id_(1), next_call_id_(0),
      message_delay_stat_(
          stat::StatisticsCollector<uint32_t>::StandardReportCallback("message_delay")) {
    UV_DCHECK_OK(uv_loop_init(&uv_loop_));
    uv_loop_.data = &event_loop_thread_;
    UV_DCHECK_OK(uv_tcp_init(&uv_loop_, &uv_http_handle_));
    uv_http_handle_.data = this;
    UV_DCHECK_OK(uv_tcp_init(&uv_loop_, &uv_grpc_handle_));
    uv_grpc_handle_.data = this;
    UV_DCHECK_OK(uv_pipe_init(&uv_loop_, &uv_ipc_handle_, 0));
    uv_ipc_handle_.data = this;
    UV_DCHECK_OK(uv_async_init(&uv_loop_, &stop_event_, &Server::StopCallback));
    stop_event_.data = this;
}

Server::~Server() {
    State state = state_.load();
    DCHECK(state == kCreated || state == kStopped);
    UV_DCHECK_OK(uv_loop_close(&uv_loop_));
}

void Server::RegisterInternalRequestHandlers() {
    // POST /shutdown
    RegisterSyncRequestHandler([] (absl::string_view method, absl::string_view path) -> bool {
        return method == "POST" && path == "/shutdown";
    }, [this] (HttpSyncRequestContext* context) {
        context->AppendToResponseBody("Server is shutting down\n");
        ScheduleStop();
    });
    // GET /hello
    RegisterSyncRequestHandler([] (absl::string_view method, absl::string_view path) -> bool {
        return method == "GET" && path == "/hello";
    }, [] (HttpSyncRequestContext* context) {
        context->AppendToResponseBody("Hello world\n");
    });
    // POST /function/[:name]
    RegisterAsyncRequestHandler([this] (absl::string_view method, absl::string_view path) -> bool {
        if (method != "POST" || !absl::StartsWith(path, "/function/")) {
            return false;
        }
        absl::string_view func_name = absl::StripPrefix(path, "/function/");
        const FuncConfig::Entry* func_entry = func_config_.find_by_func_name(func_name);
        return func_entry != nullptr;
    }, [this] (std::shared_ptr<HttpAsyncRequestContext> context) {
        const FuncConfig::Entry* func_entry = func_config_.find_by_func_name(
            absl::StripPrefix(context->path(), "/function/"));
        DCHECK(func_entry != nullptr);
        OnExternalFuncCall(static_cast<uint16_t>(func_entry->func_id), std::move(context));
    });
}

void Server::Start() {
    DCHECK(state_.load() == kCreated);
    RegisterInternalRequestHandlers();
    // Load function config file
    CHECK(!func_config_file_.empty());
    CHECK(func_config_.Load(func_config_file_));
    // Create shared memory pool
    CHECK(!shared_mem_path_.empty());
    if (fs_utils::IsDirectory(shared_mem_path_)) {
        PCHECK(fs_utils::RemoveDirectoryRecursively(shared_mem_path_));
    } else if (fs_utils::Exists(shared_mem_path_)) {
        PCHECK(fs_utils::Remove(shared_mem_path_));
    }
    PCHECK(fs_utils::MakeDirectory(shared_mem_path_));
    shared_memory_ = absl::make_unique<utils::SharedMemory>(shared_mem_path_);
    // Start IO workers
    for (int i = 0; i < num_http_workers_; i++) {
        auto io_worker = absl::make_unique<IOWorker>(this, absl::StrFormat("HttpWorker-%d", i),
                                                     kHttpConnectionBufferSize);
        InitAndStartIOWorker(io_worker.get());
        http_workers_.push_back(io_worker.get());
        io_workers_.push_back(std::move(io_worker));
    }
    for (int i = 0; i < num_ipc_workers_; i++) {
        auto io_worker = absl::make_unique<IOWorker>(this, absl::StrFormat("IpcWorker-%d", i),
                                                     kMessageConnectionBufferSize,
                                                     kMessageConnectionBufferSize);
        InitAndStartIOWorker(io_worker.get());
        ipc_workers_.push_back(io_worker.get());
        io_workers_.push_back(std::move(io_worker));
    }
    // Listen on address:port for HTTP requests
    struct sockaddr_in bind_addr;
    CHECK(!address_.empty());
    CHECK_NE(port_, -1);
    UV_DCHECK_OK(uv_ip4_addr(address_.c_str(), port_, &bind_addr));
    UV_DCHECK_OK(uv_tcp_bind(&uv_http_handle_, (const struct sockaddr *)&bind_addr, 0));
    HLOG(INFO) << "Listen on " << address_ << ":" << port_ << " for HTTP requests";
    UV_DCHECK_OK(uv_listen(
        UV_AS_STREAM(&uv_http_handle_), listen_backlog_,
        &Server::HttpConnectionCallback));
    // Listen on address:grpc_port for gRPC requests
    CHECK_NE(grpc_port_, -1);
    UV_DCHECK_OK(uv_ip4_addr(address_.c_str(), grpc_port_, &bind_addr));
    UV_DCHECK_OK(uv_tcp_bind(&uv_grpc_handle_, (const struct sockaddr *)&bind_addr, 0));
    HLOG(INFO) << "Listen on " << address_ << ":" << grpc_port_ << " for gRPC requests";
    UV_DCHECK_OK(uv_listen(
        UV_AS_STREAM(&uv_grpc_handle_), listen_backlog_,
        &Server::GrpcConnectionCallback));
    // Listen on ipc_path
    if (fs_utils::Exists(ipc_path_)) {
        PCHECK(fs_utils::Remove(ipc_path_));
    }
    UV_DCHECK_OK(uv_pipe_bind(&uv_ipc_handle_, ipc_path_.c_str()));
    HLOG(INFO) << "Listen on " << ipc_path_ << " for IPC with watchdog processes";
    UV_DCHECK_OK(uv_listen(
        UV_AS_STREAM(&uv_ipc_handle_), listen_backlog_,
        &Server::MessageConnectionCallback));
    // Start thread for running event loop
    event_loop_thread_.Start();
    state_.store(kRunning);
}

void Server::ScheduleStop() {
    HLOG(INFO) << "Scheduled to stop";
    UV_DCHECK_OK(uv_async_send(&stop_event_));
}

void Server::WaitForFinish() {
    DCHECK(state_.load() != kCreated);
    for (const auto& io_worker : io_workers_) {
        io_worker->WaitForFinish();
    }
    event_loop_thread_.Join();
    DCHECK(state_.load() == kStopped);
    HLOG(INFO) << "Stopped";
}

void Server::RegisterSyncRequestHandler(RequestMatcher matcher, SyncRequestHandler handler) {
    DCHECK(state_.load() == kCreated);
    request_handlers_.emplace_back(new RequestHandler(std::move(matcher), std::move(handler)));
}

void Server::RegisterAsyncRequestHandler(RequestMatcher matcher, AsyncRequestHandler handler) {
    DCHECK(state_.load() == kCreated);
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

namespace {
void PipeReadBufferAllocCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    size_t buf_size = 256;
    buf->base = reinterpret_cast<char*>(malloc(buf_size));
    buf->len = buf_size;
}
}

void Server::InitAndStartIOWorker(IOWorker* io_worker) {
    int pipe_fd_for_worker;
    pipes_to_io_worker_[io_worker] = CreatePipeToWorker(&pipe_fd_for_worker);
    uv_pipe_t* pipe_to_worker = pipes_to_io_worker_[io_worker].get();
    UV_DCHECK_OK(uv_read_start(UV_AS_STREAM(pipe_to_worker),
                               &PipeReadBufferAllocCallback,
                               &Server::ReturnConnectionCallback));
    io_worker->Start(pipe_fd_for_worker);
}

std::unique_ptr<uv_pipe_t> Server::CreatePipeToWorker(int* pipe_fd_for_worker) {
    int pipe_fds[2] = { -1, -1 };
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_fds), 0);
    std::unique_ptr<uv_pipe_t> pipe_to_worker = absl::make_unique<uv_pipe_t>();
    UV_DCHECK_OK(uv_pipe_init(&uv_loop_, pipe_to_worker.get(), 1));
    pipe_to_worker->data = this;
    UV_DCHECK_OK(uv_pipe_open(pipe_to_worker.get(), pipe_fds[0]));
    *pipe_fd_for_worker = pipe_fds[1];
    return pipe_to_worker;
}

void Server::TransferConnectionToWorker(IOWorker* io_worker, Connection* connection,
                                        uv_stream_t* send_handle) {
    DCHECK_IN_EVENT_LOOP_THREAD(&uv_loop_);
    uv_write_t* write_req = connection->uv_write_req_for_transfer();
    size_t buf_len = sizeof(void*);
    char* buf = connection->pipe_write_buf_for_transfer();
    memcpy(buf, &connection, buf_len);
    uv_buf_t uv_buf = uv_buf_init(buf, buf_len);
    uv_pipe_t* pipe_to_worker = pipes_to_io_worker_[io_worker].get();
    write_req->data = send_handle;
    UV_DCHECK_OK(uv_write2(write_req, UV_AS_STREAM(pipe_to_worker),
                          &uv_buf, 1, UV_AS_STREAM(send_handle),
                          &PipeWrite2Callback));
}

void Server::ReturnConnection(Connection* connection) {
    DCHECK_IN_EVENT_LOOP_THREAD(&uv_loop_);
    if (connection->type() == Connection::Type::Http) {
        HttpConnection* http_connection = static_cast<HttpConnection*>(connection);
        DCHECK(http_connections_.contains(http_connection));
        http_connections_.erase(http_connection);
    } else if (connection->type() == Connection::Type::Grpc) {
        GrpcConnection* grpc_connection = static_cast<GrpcConnection*>(connection);
        DCHECK(grpc_connections_.contains(grpc_connection));
        grpc_connections_.erase(grpc_connection);
    } else if (connection->type() == Connection::Type::Message) {
        MessageConnection* message_connection = static_cast<MessageConnection*>(connection);
        {
            absl::MutexLock lk(&message_connection_mu_);
            DCHECK(message_connections_by_client_id_.contains(message_connection->client_id()));
            message_connections_by_client_id_.erase(message_connection->client_id());
            if (message_connection->role() == Role::WATCHDOG) {
                uint16_t func_id = message_connection->func_id();
                if (watchdog_connections_by_func_id_.contains(func_id)) {
                    if (watchdog_connections_by_func_id_[func_id] == connection) {
                        watchdog_connections_by_func_id_.erase(func_id);
                    }
                } else {
                    HLOG(WARNING) << "Cannot find watchdog connection of func_id " << func_id;
                }
            }
            message_connections_.erase(message_connection);
        }
        HLOG(INFO) << "A MessageConnection is returned";
    } else {
        LOG(FATAL) << "Unknown connection type!";
    }
}

void Server::OnNewHandshake(MessageConnection* connection,
                            const HandshakeMessage& message, HandshakeResponse* response) {
    HLOG(INFO) << "Receive new handshake message from message connection";
    uint16_t client_id = next_client_id_.fetch_add(1);
    response->status = static_cast<uint16_t>(Status::OK);
    response->client_id = client_id;
    {
        absl::MutexLock lk(&message_connection_mu_);
        message_connections_by_client_id_[client_id] = connection;
        if (static_cast<Role>(message.role) == Role::WATCHDOG) {
            if (watchdog_connections_by_func_id_.contains(message.func_id)) {
                HLOG(ERROR) << "Watchdog for func_id " << message.func_id << " already exists";
                response->status = static_cast<uint16_t>(Status::WATCHDOG_EXISTS);
            } else {
                watchdog_connections_by_func_id_[message.func_id] = connection;
            }
        }
    }
}

class Server::ExternalFuncCallContext {
public:
    ExternalFuncCallContext(FuncCall call, std::shared_ptr<HttpAsyncRequestContext> http_context)
        : call_(call), http_context_(http_context), grpc_context_(nullptr),
          input_region_(nullptr), output_region_(nullptr) {}
    
    ExternalFuncCallContext(FuncCall call, std::shared_ptr<GrpcCallContext> grpc_context)
        : call_(call), http_context_(nullptr), grpc_context_(grpc_context),
          input_region_(nullptr), output_region_(nullptr) {}
    
    ~ExternalFuncCallContext() {
        if (input_region_ != nullptr) {
            input_region_->Close(true);
        }
        if (output_region_ != nullptr) {
            output_region_->Close(true);
        }
    }

    const FuncCall& call() const { return call_; }

    void CreateInputRegion(utils::SharedMemory* shared_memory) {
        if (http_context_ != nullptr) {
            absl::Span<const char> body = http_context_->body();
            input_region_ = shared_memory->Create(
                absl::StrCat(call_.full_call_id, ".i"), body.length());
            memcpy(input_region_->base(), body.data(), body.length());
        }
        if (grpc_context_ != nullptr) {
            absl::string_view method_name = grpc_context_->method_name();
            absl::Span<const char> body = grpc_context_->request_body();
            size_t region_size = method_name.length() + 1 + body.length();
            input_region_ = shared_memory->Create(
                absl::StrCat(call_.full_call_id, ".i"), region_size);
            char* buf = input_region_->base();
            memcpy(buf, method_name.data(), method_name.length());
            buf[method_name.length()] = '\0';
            if (body.length() > 0) {
                buf += method_name.length() + 1;
                memcpy(buf, body.data(), body.length());
            }
        }
    }

    void WriteOutput(utils::SharedMemory* shared_memory) {
        output_region_ = shared_memory->OpenReadOnly(
            absl::StrCat(call_.full_call_id, ".o"));
        if (http_context_ != nullptr) {
            http_context_->AppendToResponseBody(output_region_->to_span());
        }
        if (grpc_context_ != nullptr) {
            grpc_context_->AppendToResponseBody(output_region_->to_span());
        }
    }

    bool CheckInputNotEmpty() {
        if (http_context_ != nullptr && http_context_->body().length() == 0) {
            http_context_->AppendToResponseBody("Request body cannot be empty!\n");
            http_context_->SetStatus(400);
            Finish();
            return false;
        }
        // gRPC allows empty input (when protobuf serialized to empty string)
        // However, the actual input buffer will not be empty because method name
        // is appended.
        return true;
    }

    void FinishWithError() {
        if (http_context_ != nullptr) {
            http_context_->AppendToResponseBody("Function call failed\n");
            http_context_->SetStatus(500);
        }
        if (grpc_context_ != nullptr) {
            grpc_context_->set_grpc_status(GrpcStatus::UNKNOWN);
        }
        Finish();
    }

    void FinishWithWatchdogNotFound() {
        if (http_context_ != nullptr) {
            http_context_->AppendToResponseBody(
                absl::StrFormat("Cannot find watchdog for func_id %d\n",
                                static_cast<int>(call_.func_id)));
            http_context_->SetStatus(404);
        }
        if (grpc_context_ != nullptr) {
            grpc_context_->set_grpc_status(GrpcStatus::UNIMPLEMENTED);
        }
        Finish();
    }

    void Finish() {
        if (http_context_ != nullptr) {
            http_context_->Finish();
        }
        if (grpc_context_ != nullptr) {
            grpc_context_->Finish();
        }
    }

private:
    FuncCall call_;
    std::shared_ptr<HttpAsyncRequestContext> http_context_;
    std::shared_ptr<GrpcCallContext> grpc_context_;
    utils::SharedMemory::Region* input_region_;
    utils::SharedMemory::Region* output_region_;

    DISALLOW_COPY_AND_ASSIGN(ExternalFuncCallContext);
};

void Server::OnRecvMessage(MessageConnection* connection, const Message& message) {
#ifdef __FAAS_ENABLE_PROFILING
    message_delay_stat_.AddSample(
        GetMonotonicMicroTimestamp() - message.send_timestamp);
#endif
    MessageType type = static_cast<MessageType>(message.message_type);
    if (type == MessageType::INVOKE_FUNC) {
        uint16_t func_id = message.func_call.func_id;
        absl::MutexLock lk(&message_connection_mu_);
        if (watchdog_connections_by_func_id_.contains(func_id)) {
            MessageConnection* connection = watchdog_connections_by_func_id_[func_id];
            connection->WriteMessage({
#ifdef __FAAS_ENABLE_PROFILING
                .send_timestamp = GetMonotonicMicroTimestamp(),
                .processing_time = 0,
#endif
                .message_type = static_cast<uint16_t>(MessageType::INVOKE_FUNC),
                .func_call = message.func_call
            });
        } else {
            HLOG(ERROR) << "Cannot find message connection of watchdog with func_id " << func_id;
        }
    } else if (type == MessageType::FUNC_CALL_COMPLETE || type == MessageType::FUNC_CALL_FAILED) {
        uint16_t client_id = message.func_call.client_id;
        if (client_id > 0) {
            absl::MutexLock lk(&message_connection_mu_);
            if (message_connections_by_client_id_.contains(client_id)) {
                MessageConnection* connection = message_connections_by_client_id_[client_id];
                connection->WriteMessage({
#ifdef __FAAS_ENABLE_PROFILING
                    .send_timestamp = GetMonotonicMicroTimestamp(),
                    .processing_time = message.processing_time,
#endif
                    .message_type = static_cast<uint16_t>(type),
                    .func_call = message.func_call
                });
            } else {
                HLOG(ERROR) << "Cannot find message connection with client_id " << client_id;
            }
        } else {
            absl::MutexLock lk(&external_func_calls_mu_);
            uint64_t full_call_id = message.func_call.full_call_id;
            if (external_func_calls_.contains(full_call_id)) {
                ExternalFuncCallContext* func_call_context = external_func_calls_[full_call_id].get();
                if (type == MessageType::FUNC_CALL_COMPLETE) {
                    func_call_context->WriteOutput(shared_memory_.get());
                    func_call_context->Finish();
                } else if (type == MessageType::FUNC_CALL_FAILED) {
                    func_call_context->FinishWithError();
                }
                external_func_calls_.erase(full_call_id);
            } else {
                HLOG(ERROR) << "Cannot find external call with func_id=" << message.func_call.func_id << ", "
                            << "call_id=" << message.func_call.call_id;
            }
        }
    } else {
        LOG(ERROR) << "Unknown message type!";
    }
}

void Server::OnNewGrpcCall(std::shared_ptr<GrpcCallContext> call_context) {
    const FuncConfig::Entry* func_entry = func_config_.find_by_func_name(
        absl::StrFormat("grpc:%s", call_context->service_name()));
    if (func_entry == nullptr || !func_entry->grpc_methods.contains(call_context->method_name())) {
        call_context->set_grpc_status(GrpcStatus::NOT_FOUND);
        call_context->Finish();
        return;
    }
    NewExternalFuncCall(absl::WrapUnique(
        new ExternalFuncCallContext(NewFuncCall(func_entry->func_id), std::move(call_context))));
}

FuncCall Server::NewFuncCall(uint16_t func_id) {
    FuncCall call;
    call.func_id = func_id;
    call.client_id = 0;
    call.call_id = next_call_id_.fetch_add(1);
    return call;
}

void Server::OnExternalFuncCall(uint16_t func_id, std::shared_ptr<HttpAsyncRequestContext> http_context) {
    NewExternalFuncCall(absl::WrapUnique(
        new ExternalFuncCallContext(NewFuncCall(func_id), std::move(http_context))));
}

void Server::NewExternalFuncCall(std::unique_ptr<ExternalFuncCallContext> func_call_context) {
    if (!func_call_context->CheckInputNotEmpty()) {
        return;
    }
    func_call_context->CreateInputRegion(shared_memory_.get());
    uint16_t func_id = func_call_context->call().func_id;
    {
        absl::MutexLock lk(&message_connection_mu_);
        if (watchdog_connections_by_func_id_.contains(func_id)) {
            MessageConnection* connection = watchdog_connections_by_func_id_[func_id];
            connection->WriteMessage({
#ifdef __FAAS_ENABLE_PROFILING
                .send_timestamp = GetMonotonicMicroTimestamp(),
                .processing_time = 0,
#endif
                .message_type = static_cast<uint16_t>(MessageType::INVOKE_FUNC),
                .func_call = func_call_context->call()
            });
        } else {
            HLOG(WARNING) << "Watchdog for func_id " << func_id << " not found";
            func_call_context->FinishWithWatchdogNotFound();
            return;
        }
    }
    {
        absl::MutexLock lk(&external_func_calls_mu_);
        external_func_calls_[func_call_context->call().full_call_id] = std::move(func_call_context);
    }
}

UV_CONNECTION_CB_FOR_CLASS(Server, HttpConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open HTTP connection: " << uv_strerror(status);
        return;
    }
    std::unique_ptr<HttpConnection> connection = absl::make_unique<HttpConnection>(
        this, next_http_connection_id_++);
    uv_tcp_t* client = reinterpret_cast<uv_tcp_t*>(malloc(sizeof(uv_tcp_t)));
    UV_DCHECK_OK(uv_tcp_init(&uv_loop_, client));
    if (uv_accept(UV_AS_STREAM(&uv_http_handle_), UV_AS_STREAM(client)) == 0) {
        TransferConnectionToWorker(PickHttpWorker(), connection.get(), UV_AS_STREAM(client));
        http_connections_.insert(std::move(connection));
    } else {
        LOG(ERROR) << "Failed to accept new HTTP connection";
        free(client);
    }
}

UV_CONNECTION_CB_FOR_CLASS(Server, GrpcConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open gRPC connection: " << uv_strerror(status);
        return;
    }
    std::unique_ptr<GrpcConnection> connection = absl::make_unique<GrpcConnection>(
        this, next_grpc_connection_id_++);
    uv_tcp_t* client = reinterpret_cast<uv_tcp_t*>(malloc(sizeof(uv_tcp_t)));
    UV_DCHECK_OK(uv_tcp_init(&uv_loop_, client));
    if (uv_accept(UV_AS_STREAM(&uv_grpc_handle_), UV_AS_STREAM(client)) == 0) {
        TransferConnectionToWorker(PickHttpWorker(), connection.get(), UV_AS_STREAM(client));
        grpc_connections_.insert(std::move(connection));
    } else {
        LOG(ERROR) << "Failed to accept new gRPC connection";
        free(client);
    }
}

UV_CONNECTION_CB_FOR_CLASS(Server, MessageConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open message connection: " << uv_strerror(status);
        return;
    }
    HLOG(INFO) << "New message connection";
    std::unique_ptr<MessageConnection> connection = absl::make_unique<MessageConnection>(this);
    uv_pipe_t* client = reinterpret_cast<uv_pipe_t*>(malloc(sizeof(uv_pipe_t)));
    UV_DCHECK_OK(uv_pipe_init(&uv_loop_, client, 0));
    if (uv_accept(UV_AS_STREAM(&uv_ipc_handle_), UV_AS_STREAM(client)) == 0) {
        TransferConnectionToWorker(PickIpcWorker(), connection.get(),
                                   UV_AS_STREAM(client));
        message_connections_.insert(std::move(connection));
    } else {
        LOG(ERROR) << "Failed to accept new message connection";
        free(client);
    }
}

UV_READ_CB_FOR_CLASS(Server, ReturnConnection) {
    if (nread < 0) {
        if (nread == UV_EOF) {
            HLOG(WARNING) << "Pipe is closed by the corresponding IO worker";
        } else {
            HLOG(ERROR) << "Failed to read from pipe: " << uv_strerror(nread);
        }
    } else if (nread > 0) {
        utils::ReadMessages<Connection*>(
            &return_connection_read_buffer_, buf->base, nread,
            [this] (Connection** connection) {
                ReturnConnection(*connection);
            });
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
        UV_DCHECK_OK(uv_read_stop(UV_AS_STREAM(pipe)));
        uv_close(UV_AS_HANDLE(pipe), nullptr);
    }
    uv_close(UV_AS_HANDLE(&uv_http_handle_), nullptr);
    uv_close(UV_AS_HANDLE(&uv_grpc_handle_), nullptr);
    uv_close(UV_AS_HANDLE(&uv_ipc_handle_), nullptr);
    uv_close(UV_AS_HANDLE(&stop_event_), nullptr);
    state_.store(kStopping);
}

namespace {
void HandleFreeCallback(uv_handle_t* handle) {
    free(handle);
}
}

UV_WRITE_CB_FOR_CLASS(Server, PipeWrite2) {
    DCHECK(status == 0) << "Failed to write to pipe: " << uv_strerror(status);
    uv_close(UV_AS_HANDLE(req->data), HandleFreeCallback);
}

}  // namespace gateway
}  // namespace faas
