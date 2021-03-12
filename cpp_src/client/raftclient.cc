#include "client/raftclient.h"
#include "client/cororpcclient.h"
#include "tools/logger.h"

namespace reindexer {
namespace client {

RaftClient::RaftClient(const CoroReindexerConfig& config) : impl_(new CoroRPCClient(config)), owner_(true), ctx_() {}
RaftClient::~RaftClient() {
	if (owner_) {
		delete impl_;
	}
}
RaftClient::RaftClient(RaftClient&& rdx) noexcept : impl_(rdx.impl_), owner_(rdx.owner_), ctx_(rdx.ctx_) { rdx.owner_ = false; }
RaftClient& RaftClient::operator=(RaftClient&& rdx) noexcept {
	if (this != &rdx) {
		impl_ = rdx.impl_;
		owner_ = rdx.owner_;
		ctx_ = rdx.ctx_;
		rdx.owner_ = false;
	}
	return *this;
}

Error RaftClient::Connect(const string& dsn, net::ev::dynamic_loop& loop, const client::ConnectOpts& opts) {
	return impl_->Connect(dsn, loop, opts);
}
Error RaftClient::Stop() { return impl_->Stop(); }

Error RaftClient::SuggestLeader(const NodeData& suggestion, NodeData& response) { return impl_->SuggestLeader(suggestion, response, ctx_); }

Error RaftClient::LeadersPing(const NodeData& leader) { return impl_->LeadersPing(leader, ctx_); }

Error RaftClient::GetRaftInfo(RaftClient::RaftInfo& info) { return impl_->GetRaftInfo(info, ctx_); }

// Error RaftClient::GetClusterConfig() {}

// Error RaftClient::SetClusterConfig() {}

Error RaftClient::Status() { return impl_->Status(ctx_); }

}  // namespace client
}  // namespace reindexer
