#pragma once

//#include "base/thread.h"
#include "log/common.h"
#include "log/view.h"
#include "log/view_watcher.h"
#include "log/db.h"
#include "log/cache.h"
#include "log/index.h"
#include "server/server_base.h"
#include "server/ingress_connection.h"
#include "server/egress_hub.h"

namespace faas {
namespace log {

class IndexBase : public server::ServerBase {
public:
    explicit IndexBase(uint16_t node_id);
    virtual ~IndexBase();

    void StartInternal() override;
    void StopInternal() override;

protected:
    uint16_t my_node_id() const { return node_id_; }

    virtual void OnViewCreated(const View* view) = 0;
    // virtual void OnViewFrozen(const View* view) = 0;
    virtual void OnViewFinalized(const FinalizedView* finalized_view) = 0;

    virtual void HandleReadRequest(const protocol::SharedLogMessage& request) = 0;
    virtual void OnRecvNewIndexData(const protocol::SharedLogMessage& message,
                                        std::span<const char> payload) = 0;
    virtual void HandleSlaveResult(const protocol::SharedLogMessage& message, std::span<const char> payload) = 0;

    //virtual void BackgroundThreadMain() = 0;

    void MessageHandler(const protocol::SharedLogMessage& message,
                        std::span<const char> payload);
    bool IsSlave(IndexQuery index_query);
    // void SendIndexData(const View* view, const IndexDataProto& index_data_proto);

    // void SendEngineIndexResult(const protocol::SharedLogMessage& request,
    //                          protocol::SharedLogMessage* response,
    //                          std::span<const char> tags_data);

    void SendMasterIndexResult(const IndexQueryResult& result);
    void SendIndexReadResponse(const IndexQueryResult& result);
    void SendIndexReadFailureResponse(const IndexQuery& query,  protocol::SharedLogResultType result);
    bool SendStorageReadRequest(const IndexQueryResult& result, const View::StorageShard* storage_shard_node);

    struct IndexReadOp {
        uint64_t id;
        int64_t start_timestamp;
        absl::flat_hash_set<uint16_t> merged_nodes; //todo: merged shard less error prone
        IndexQueryResult index_query_result; // stores the current result
    };

private:
    const uint16_t node_id_;

    ViewWatcher view_watcher_;

    //base::Thread background_thread_;

    absl::flat_hash_map</* id */ int, std::unique_ptr<server::IngressConnection>>
        ingress_conns_;

    absl::Mutex conn_mu_;
    absl::flat_hash_map</* id */ int, std::unique_ptr<server::EgressHub>>
        egress_hubs_ ABSL_GUARDED_BY(conn_mu_);

    //std::optional<LRUCache> log_cache_;

    void SetupZKWatchers();
    void SetupTimers();


    void OnConnectionClose(server::ConnectionBase* connection) override;
    void OnRemoteMessageConn(const protocol::HandshakeMessage& handshake,
                             int sockfd) override;

    void OnRecvSharedLogMessage(int conn_type, uint16_t src_node_id,
                                const protocol::SharedLogMessage& message,
                                std::span<const char> payload);
    bool SendSharedLogMessage(protocol::ConnType conn_type, uint16_t dst_node_id,
                              const protocol::SharedLogMessage& message,
                              std::span<const char> payload1 = EMPTY_CHAR_SPAN);

    server::EgressHub* CreateEgressHub(protocol::ConnType conn_type,
                                       uint16_t dst_node_id,
                                       server::IOWorker* io_worker);

    DISALLOW_COPY_AND_ASSIGN(IndexBase);
};

}  // namespace log
}  // namespace faas