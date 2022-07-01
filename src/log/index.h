#pragma once

#include "log/log_space_base.h"
#include "log/index_dto.h"

namespace faas {
namespace log {

class Index final : public LogSpaceBase {
public:
    static constexpr absl::Duration kBlockingQueryTimeout = absl::Seconds(1);

    Index(const View* view, uint16_t sequencer_id);
    Index(const View* view, uint16_t sequencer_id, uint32_t index_shard_id, size_t num_shards);
    ~Index();

    void ProvideIndexData(const IndexDataProto& index_data);
    void ProvideIndexDataShard(const IndexDataProto& index_data);

    void MakeQuery(const IndexQuery& query);

    using QueryResultVec = absl::InlinedVector<IndexQueryResult, 4>;
    void PollQueryResults(QueryResultVec* results);

    void AdvanceIndexProgress();
    bool AdvanceIndexProgress(const IndexDataProto& index_data, size_t num_index_shards);

    bool TryCompleteIndexUpdates(uint32_t* seqnum_position, size_t num_index_shards);
    bool CheckIfNewIndexData(const IndexDataProto& index_data);

    void Aggregate(size_t* num_seqnums, size_t* num_tags, size_t* num_seqnums_of_tags, size_t* size);

    uint32_t indexed_metalog_position(){
        return indexed_metalog_position_;
    }

private:
    class PerSpaceIndex;
    absl::flat_hash_map</* user_logspace */ uint32_t,
                        std::unique_ptr<PerSpaceIndex>> index_;

    static constexpr uint32_t kMaxMetalogPosition = std::numeric_limits<uint32_t>::max();

    std::multimap</* metalog_position */ uint32_t,
                  IndexQuery> pending_queries_;
    std::vector<std::pair</* start_timestamp */ int64_t,
                          IndexQuery>> blocking_reads_;
    QueryResultVec pending_query_results_;

    std::deque<std::pair</* metalog_seqnum */ uint32_t,
                         /* end_seqnum */ uint32_t>> cuts_;
    uint32_t indexed_metalog_position_;

    bool first_index_data_; // for local indexing

    // for index tier
    absl::flat_hash_map<uint32_t /* metalog_position */, std::pair<size_t, absl::flat_hash_set<uint16_t>>> storage_shards_index_updates_;
    absl::flat_hash_map<uint32_t /* metalog_position */, uint32_t> end_seqnum_positions_;

    struct IndexData {
        uint16_t   engine_id;
        uint32_t   user_logspace;
        UserTagVec user_tags;
        bool skip;
    };
    std::map</* seqnum */ uint32_t, IndexData> received_data_;
    uint32_t data_received_seqnum_position_;
    uint32_t indexed_seqnum_position_;

    size_t num_shards_;

    uint64_t index_metalog_progress() const {
        return bits::JoinTwo32(identifier(), indexed_metalog_position_);
    }

    uint64_t sharded_index_metalog_progress() const {
        uint32_t real_index_metalog_progress = indexed_metalog_position_;
        if (real_index_metalog_progress <= num_shards_) {
            real_index_metalog_progress = 0;
        } else {
            real_index_metalog_progress -= num_shards_;
        }
        return bits::JoinTwo32(identifier(), real_index_metalog_progress);
    }

    void OnMetaLogApplied(const MetaLogProto& meta_log_proto) override;
    void OnFinalized(uint32_t metalog_position) override;
    PerSpaceIndex* GetOrCreateIndex(uint32_t user_logspace);
    void TryCreateIndex(uint32_t user_logspace);

    void ProcessQuery(const IndexQuery& query);
    void ProcessReadNext(const IndexQuery& query);
    void ProcessReadPrev(const IndexQuery& query);
    bool ProcessBlockingQuery(const IndexQuery& query);

    bool IndexFindNext(const IndexQuery& query, uint64_t* seqnum, uint16_t* engine_id);
    bool IndexFindPrev(const IndexQuery& query, uint64_t* seqnum, uint16_t* engine_id);

    IndexQueryResult BuildFoundResult(const IndexQuery& query, uint16_t view_id,
                                      uint64_t seqnum, uint16_t engine_id);
    IndexQueryResult BuildNotFoundResult(const IndexQuery& query);
    IndexQueryResult BuildContinueResult(const IndexQuery& query, bool found,
                                         uint64_t seqnum, uint16_t engine_id);

    DISALLOW_COPY_AND_ASSIGN(Index);
};

}  // namespace log
}  // namespace faas
