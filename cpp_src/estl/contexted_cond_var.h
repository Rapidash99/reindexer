#pragma once

#include <condition_variable>
#include "contexted_locks.h"

namespace reindexer {

class contexted_cond_var {
public:
	using CondVarType = std::condition_variable_any;

	explicit contexted_cond_var(milliseconds __chk_timeout = kDefaultCondChkTime)
		: _M_cond_var(new CondVarType), _M_chk_timeout(__chk_timeout) {}
	contexted_cond_var(contexted_cond_var&& other) = default;
	contexted_cond_var(const contexted_cond_var&) = delete;
	contexted_cond_var& operator=(const contexted_cond_var&) = delete;
	contexted_cond_var& operator=(contexted_cond_var&& other) = default;

	template <typename _Lock, typename _Predicate, typename _ContextT>
	void wait(_Lock& __lock, _Predicate __p, const _ContextT& __context) {
		assert(_M_cond_var);
		// const auto lockWard = _M_context->BeforeLock(_Mutex::mark);
		if (_M_chk_timeout.count() > 0 && __context.isCancelable()) {
			do {
				ThrowOnCancel(__context, "Cond wait was canceled on condition"_sv);
			} while (!_M_cond_var->wait_for(__lock, _M_chk_timeout, __p));
		} else {
			_M_cond_var->wait(__lock, std::move(__p));
		}
	}

	template <typename _Lock, typename _ContextT>
	void wait(_Lock& __lock, const _ContextT& __context) {
		assert(_M_cond_var);
		// const auto lockWard = _M_context->BeforeLock(_Mutex::mark);
		if (_M_chk_timeout.count() > 0 && __context.isCancelable()) {
			do {
				ThrowOnCancel(__context, "Cond wait was canceled on condition"_sv);
			} while (_M_cond_var->wait_for(__lock, _M_chk_timeout) == std::cv_status::timeout);
		} else {
			_M_cond_var->wait(__lock);
		}
	}

	void notify_all() {
		assert(_M_cond_var);
		_M_cond_var->notify_all();
	}

	void notify_one() {
		assert(_M_cond_var);
		_M_cond_var->notify_one();
	}

private:
	std::unique_ptr<CondVarType> _M_cond_var;
	milliseconds _M_chk_timeout;
};

}  // namespace reindexer
