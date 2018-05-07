/*
 * fdblibtlssession.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDB_LIBTLS_SESSION_H
#define FDB_LIBTLS_SESSION_H

#pragma once

#include "ITLSPlugin.h"
#include "ReferenceCounted.h"

#include "fdblibtlspolicy.h"

#include <tls.h>

struct FDBLibTLSSession : ITLSSession, ReferenceCounted<FDBLibTLSSession> {
	FDBLibTLSSession(Reference<FDBLibTLSPolicy> policy, bool is_client, TLSSendCallbackFunc send_func, void* send_ctx, TLSRecvCallbackFunc recv_func, void* recv_ctx, void* uid);
	virtual ~FDBLibTLSSession();

	virtual void addref() { ReferenceCounted<FDBLibTLSSession>::addref(); }
	virtual void delref() { ReferenceCounted<FDBLibTLSSession>::delref(); }

	bool check_criteria();

	virtual int handshake();
	virtual int read(uint8_t* data, int length);
	virtual int write(const uint8_t* data, int length);

	Reference<FDBLibTLSPolicy> policy;

	struct tls *tls_ctx;
	struct tls *tls_sctx;

	TLSSendCallbackFunc send_func;
	void* send_ctx;
	TLSRecvCallbackFunc recv_func;
	void* recv_ctx;

	bool handshake_completed;

	void* uid;
};

#endif /* FDB_LIBTLS_SESSION_H */
