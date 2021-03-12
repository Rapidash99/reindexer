#include "synccororeindexer.h"
#include "client/cororeindexer.h"
#include "client/cororpcclient.h"
#include "synccororeindexerimpl.h"
#include "tools/logger.h"

namespace reindexer {
namespace client {

SyncCoroReindexer::SyncCoroReindexer(const CoroReindexerConfig& config) : impl_(new SyncCoroReindexerImpl(config)), owner_(true), ctx_() {}
SyncCoroReindexer::~SyncCoroReindexer() {
	if (owner_) {
		delete impl_;
	}
}
SyncCoroReindexer::SyncCoroReindexer(SyncCoroReindexer&& rdx) noexcept : impl_(rdx.impl_), owner_(rdx.owner_), ctx_(rdx.ctx_) {
	rdx.owner_ = false;
}
SyncCoroReindexer& SyncCoroReindexer::operator=(SyncCoroReindexer&& rdx) noexcept {
	if (this != &rdx) {
		impl_ = rdx.impl_;
		owner_ = rdx.owner_;
		ctx_ = rdx.ctx_;
		rdx.owner_ = false;
	}
	return *this;
}

Error SyncCoroReindexer::Connect(const string& dsn, const client::ConnectOpts& opts) { return impl_->Connect(dsn, opts); }
Error SyncCoroReindexer::Stop() { return impl_->Stop(); }
Error SyncCoroReindexer::AddNamespace(const NamespaceDef& nsDef) { return impl_->AddNamespace(nsDef, ctx_); }
Error SyncCoroReindexer::OpenNamespace(string_view nsName, const StorageOpts& storage) {
	return impl_->OpenNamespace(nsName, ctx_, storage);
}
Error SyncCoroReindexer::DropNamespace(string_view nsName) { return impl_->DropNamespace(nsName, ctx_); }
Error SyncCoroReindexer::CloseNamespace(string_view nsName) { return impl_->CloseNamespace(nsName, ctx_); }
Error SyncCoroReindexer::TruncateNamespace(string_view nsName) { return impl_->TruncateNamespace(nsName, ctx_); }
Error SyncCoroReindexer::RenameNamespace(string_view srcNsName, const std::string& dstNsName) {
	return impl_->RenameNamespace(srcNsName, dstNsName, ctx_);
}
Error SyncCoroReindexer::Insert(string_view nsName, Item& item) { return impl_->Insert(nsName, item, ctx_); }
Error SyncCoroReindexer::Update(string_view nsName, Item& item) { return impl_->Update(nsName, item, ctx_); }
Error SyncCoroReindexer::Update(const Query& q, SyncCoroQueryResults& result) { return impl_->Update(q, result, ctx_); }
Error SyncCoroReindexer::Upsert(string_view nsName, Item& item) { return impl_->Upsert(nsName, item, ctx_); }
Error SyncCoroReindexer::Delete(string_view nsName, Item& item) { return impl_->Delete(nsName, item, ctx_); }
Item SyncCoroReindexer::NewItem(string_view nsName) { return impl_->NewItem(nsName); }
Error SyncCoroReindexer::GetMeta(string_view nsName, const string& key, string& data) { return impl_->GetMeta(nsName, key, data, ctx_); }
Error SyncCoroReindexer::PutMeta(string_view nsName, const string& key, const string_view& data) {
	return impl_->PutMeta(nsName, key, data, ctx_);
}
Error SyncCoroReindexer::EnumMeta(string_view nsName, vector<string>& keys) { return impl_->EnumMeta(nsName, keys, ctx_); }
Error SyncCoroReindexer::Delete(const Query& q, SyncCoroQueryResults& result) { return impl_->Delete(q, result, ctx_); }
Error SyncCoroReindexer::Select(string_view query, SyncCoroQueryResults& result) { return impl_->Select(query, result, ctx_); }
Error SyncCoroReindexer::Select(const Query& q, SyncCoroQueryResults& result) { return impl_->Select(q, result, ctx_); }
Error SyncCoroReindexer::Commit(string_view nsName) { return impl_->Commit(nsName); }
Error SyncCoroReindexer::AddIndex(string_view nsName, const IndexDef& idx) { return impl_->AddIndex(nsName, idx, ctx_); }
Error SyncCoroReindexer::UpdateIndex(string_view nsName, const IndexDef& idx) { return impl_->UpdateIndex(nsName, idx, ctx_); }
Error SyncCoroReindexer::DropIndex(string_view nsName, const IndexDef& index) { return impl_->DropIndex(nsName, index, ctx_); }
Error SyncCoroReindexer::SetSchema(string_view nsName, string_view schema) { return impl_->SetSchema(nsName, schema, ctx_); }
Error SyncCoroReindexer::EnumNamespaces(vector<NamespaceDef>& defs, EnumNamespacesOpts opts) {
	return impl_->EnumNamespaces(defs, opts, ctx_);
}
Error SyncCoroReindexer::EnumDatabases(vector<string>& dbList) { return impl_->EnumDatabases(dbList, ctx_); }
Error SyncCoroReindexer::SubscribeUpdates(IUpdatesObserver* observer, const UpdatesFilters& filters, SubscriptionOpts opts) {
	return impl_->SubscribeUpdates(observer, filters, opts);
}
Error SyncCoroReindexer::UnsubscribeUpdates(IUpdatesObserver* observer) { return impl_->UnsubscribeUpdates(observer); }
Error SyncCoroReindexer::GetSqlSuggestions(const string_view sqlQuery, int pos, vector<string>& suggests) {
	return impl_->GetSqlSuggestions(sqlQuery, pos, suggests);
}
Error SyncCoroReindexer::Status() { return impl_->Status(ctx_); }

SyncCoroTransaction SyncCoroReindexer::NewTransaction(string_view nsName) { return impl_->NewTransaction(nsName, ctx_); }
Error SyncCoroReindexer::CommitTransaction(SyncCoroTransaction& tr) { return impl_->CommitTransaction(tr, ctx_); }
Error SyncCoroReindexer::RollBackTransaction(SyncCoroTransaction& tr) { return impl_->RollBackTransaction(tr, ctx_); }

}  // namespace client
}  // namespace reindexer
