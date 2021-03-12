#include "functionexecutor.h"
#include "core/namespace/namespaceimpl.h"
#include "core/selectfunc/selectfuncparser.h"
#include "tools/timetools.h"

namespace reindexer {

FunctionExecutor::FunctionExecutor(NamespaceImpl& ns, h_vector<cluster::UpdateRecord, 1>& replUpdates)
	: ns_(ns), replUpdates_(replUpdates) {}

Variant FunctionExecutor::Execute(SelectFuncStruct& funcData) {
	if (funcData.funcName == "now") {
		string mode = "sec";
		if (!funcData.funcArgs.empty() && !funcData.funcArgs.front().empty()) {
			mode = funcData.funcArgs.front();
		}
		return Variant(getTimeNow(mode));
	} else if (funcData.funcName == "serial") {
		return Variant(ns_.GetSerial(funcData.field, replUpdates_));
	} else {
		throw Error(errParams, "Unknown function %s", funcData.field);
	}
}

}  // namespace reindexer
