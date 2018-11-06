/*
 * BackupToDBUpgrade.actor.cpp
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

#include "flow/actorcompiler.h"
#include "fdbrpc/simulator.h"
#include "fdbclient/BackupAgent.h"
#include "workloads.h"
#include "BulkSetup.actor.h"
#include "fdbclient/ManagementAPI.h"

//A workload which test the correctness of upgrading DR from 5.1 to 5.2
struct BackupToDBUpgradeWorkload : TestWorkload {
	double backupAfter, stopDifferentialAfter;
	Key backupTag, restoreTag, backupPrefix, extraPrefix;
	int backupRangesCount, backupRangeLengthMax;
	Standalone<VectorRef<KeyRangeRef>> backupRanges;
	Database extraDB;
	bool shareLogRange;

	BackupToDBUpgradeWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		backupAfter = getOption(options, LiteralStringRef("backupAfter"), g_random->random01() * 10.0);
		backupPrefix = getOption(options, LiteralStringRef("backupPrefix"), StringRef());
		backupRangeLengthMax = getOption(options, LiteralStringRef("backupRangeLengthMax"), 1);
		stopDifferentialAfter = getOption(options, LiteralStringRef("stopDifferentialAfter"), 60.0);
		backupTag = getOption(options, LiteralStringRef("backupTag"), BackupAgentBase::getDefaultTag());
		restoreTag = getOption(options, LiteralStringRef("restoreTag"), LiteralStringRef("restore"));
		backupRangesCount = getOption(options, LiteralStringRef("backupRangesCount"), 5);
		extraPrefix = backupPrefix.withPrefix(LiteralStringRef("\xfe\xff\xfe"));
		backupPrefix = backupPrefix.withPrefix(LiteralStringRef("\xfe\xff\xff"));

		ASSERT(backupPrefix != StringRef());

		KeyRef beginRange;
		KeyRef endRange;

		if(backupRangesCount <= 0) {
			backupRanges.push_back_deep(backupRanges.arena(), KeyRangeRef(normalKeys.begin, std::min(backupPrefix, extraPrefix)));
		} else {
			// Add backup ranges
			for (int rangeLoop = 0; rangeLoop < backupRangesCount; rangeLoop++)
			{
				// Get a random range of a random sizes
				beginRange = KeyRef(backupRanges.arena(), g_random->randomAlphaNumeric(g_random->randomInt(1, backupRangeLengthMax + 1)));
				endRange = KeyRef(backupRanges.arena(), g_random->randomAlphaNumeric(g_random->randomInt(1, backupRangeLengthMax + 1)));

				// Add the range to the array
				backupRanges.push_back_deep(backupRanges.arena(), (beginRange < endRange) ? KeyRangeRef(beginRange, endRange) : KeyRangeRef(endRange, beginRange));

				// Track the added range
				TraceEvent("DRU_backup_range").detail("rangeBegin", (beginRange < endRange) ? printable(beginRange) : printable(endRange))
					.detail("rangeEnd", (beginRange < endRange) ? printable(endRange) : printable(beginRange));
			}
		}

		Reference<ClusterConnectionFile> extraFile(new ClusterConnectionFile(*g_simulator.extraDB));
		Reference<Cluster> extraCluster = Cluster::createCluster(extraFile, -1);
		extraDB = extraCluster->createDatabase(LiteralStringRef("DB")).get();

		TraceEvent("DRU_start");
	}

	virtual std::string description() {
		return "BackupToDBUpgrade";
	}

	virtual Future<Void> setup(Database const& cx) {
		if (clientId != 0)
			return Void();
		return _setup(cx, this);
	}

	virtual Future<Void> start(Database const& cx) {
		if (clientId != 0)
			return Void();
		return _start(cx, this);
	}

	virtual Future<bool> check(Database const& cx) {
		return true;
	}

	virtual void getMetrics(vector<PerfMetric>& m) {
	}

	ACTOR static Future<Void> doBackup(BackupToDBUpgradeWorkload* self, DatabaseBackupAgent* backupAgent, Database cx, Key tag, Standalone<VectorRef<KeyRangeRef>> backupRanges) {
		try {
			state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(self->extraDB));
			loop{
				try {
					for (auto r : self->backupRanges) {
						if (!r.empty()) {
							auto targetRange = r.withPrefix(self->backupPrefix);
							printf("Clearing %s in destination\n", printable(targetRange).c_str());
							tr->addReadConflictRange(targetRange);
							tr->clear(targetRange);
						}
					}
					Void _ = wait(backupAgent->submitBackup(tr, tag, backupRanges, false, self->backupPrefix, StringRef()));
					Void _ = wait(tr->commit());
					break;
				} catch (Error &e) {
					Void _ = wait(tr->onError(e));
				}
			}

			TraceEvent("DRU_doBackupInDifferentialMode").detail("tag", printable(tag));
		} catch (Error &e) {
			TraceEvent("DRU_doBackupSubmitBackupError").detail("tag", printable(tag)).error(e);
			if (e.code() != error_code_backup_unneeded && e.code() != error_code_backup_duplicate) {
				throw e;
			}
		}

		int _ = wait( backupAgent->waitBackup(self->extraDB, tag, false) );

		return Void();
	}

	ACTOR static Future<Void> checkData(Database cx, UID logUid, UID destUid, Key tag, DatabaseBackupAgent* backupAgent, bool shareLogRange) {
		state Key backupAgentKey = uidPrefixKey(logRangesRange.begin, logUid);
		state Key backupLogValuesKey = uidPrefixKey(backupLogKeys.begin, destUid);
		state Key backupLatestVersionsPath = uidPrefixKey(backupLatestVersionsPrefix, destUid);
		state Key backupLatestVersionsKey = uidPrefixKey(backupLatestVersionsPath, logUid);
		state int displaySystemKeys = 0;

		ASSERT(destUid.isValid());

		// Ensure that there is no left over key within the backup subspace
		loop {
			state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(cx));

			TraceEvent("DRU_checkLeftoverkeys").detail("backupTag", printable(tag));

			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);

				// Check the left over tasks
				// We have to wait for the list to empty since an abort and get status
				// can leave extra tasks in the queue
				TraceEvent("DRU_checkLeftovertasks").detail("backupTag", printable(tag));
				state int64_t taskCount = wait( backupAgent->getTaskCount(tr) );
				state int waitCycles = 0;

				if ((taskCount) && (0)) {
					TraceEvent("DRU_EndingNonzeroTaskCount").detail("backupTag", printable(tag)).detail("taskCount", taskCount).detail("waitCycles", waitCycles);
					printf("EndingNonZeroTasks: %ld\n", (long) taskCount);
					Void _ = wait(TaskBucket::debugPrintRange(cx, LiteralStringRef("\xff"), StringRef()));
				}

				loop {
					waitCycles ++;

					TraceEvent("DRU_NonzeroTaskWait").detail("backupTag", printable(tag)).detail("taskCount", taskCount).detail("waitCycles", waitCycles);
					printf("%.6f Wait #%4d for %lld tasks to end\n", now(), waitCycles, (long long) taskCount);

					Void _ = wait(delay(20.0));
					tr->commit();
					tr = Reference<ReadYourWritesTransaction>(new ReadYourWritesTransaction(cx));
					int64_t _taskCount = wait( backupAgent->getTaskCount(tr) );
					taskCount = _taskCount;

					if (!taskCount) {
						break;
					}
				}

				if (taskCount) {
					displaySystemKeys ++;
					TraceEvent(SevError, "DRU_NonzeroTaskCount").detail("backupTag", printable(tag)).detail("taskCount", taskCount).detail("waitCycles", waitCycles);
					printf("BackupCorrectnessLeftOverLogTasks: %ld\n", (long) taskCount);
				}

				Standalone<RangeResultRef> agentValues = wait(tr->getRange(KeyRange(KeyRangeRef(backupAgentKey, strinc(backupAgentKey))), 100));

				// Error if the system keyspace for the backup tag is not empty
				if (agentValues.size() > 0) {
					displaySystemKeys++;
					printf("BackupCorrectnessLeftOverMutationKeys: (%d) %s\n", agentValues.size(), printable(backupAgentKey).c_str());
					TraceEvent(SevError, "BackupCorrectnessLeftOverMutationKeys").detail("backupTag", printable(tag))
						.detail("LeftOverKeys", agentValues.size()).detail("keySpace", printable(backupAgentKey));
					for (auto & s : agentValues) {
						TraceEvent("DRU_LeftOverKey").detail("key", printable(StringRef(s.key.toString()))).detail("value", printable(StringRef(s.value.toString())));
						printf("   Key: %-50s  Value: %s\n", printable(StringRef(s.key.toString())).c_str(), printable(StringRef(s.value.toString())).c_str());
					}
				}
				else {
					printf("No left over backup agent configuration keys\n");
				}

				Optional<Value> latestVersion = wait(tr->get(backupLatestVersionsKey));
				if (latestVersion.present()) {
					TraceEvent(SevError, "BackupCorrectnessLeftOverVersionKey").detail("backupTag", printable(tag)).detail("key", backupLatestVersionsKey.printable()).detail("value", BinaryReader::fromStringRef<Version>(latestVersion.get(), Unversioned()));
				} else {
					printf("No left over backup version key\n");
				}

				Standalone<RangeResultRef> versions = wait(tr->getRange(KeyRange(KeyRangeRef(backupLatestVersionsPath, strinc(backupLatestVersionsPath))), 1));
				if (!shareLogRange || !versions.size()) {
					Standalone<RangeResultRef> logValues = wait(tr->getRange(KeyRange(KeyRangeRef(backupLogValuesKey, strinc(backupLogValuesKey))), 100));

					// Error if the log/mutation keyspace for the backup tag is not empty
					if (logValues.size() > 0) {
						displaySystemKeys++;
						printf("BackupCorrectnessLeftOverLogKeys: (%d) %s\n", logValues.size(), printable(backupLogValuesKey).c_str());
						TraceEvent(SevError, "BackupCorrectnessLeftOverLogKeys").detail("backupTag", printable(tag))
							.detail("LeftOverKeys", logValues.size()).detail("keySpace", printable(backupLogValuesKey)).detail("version", decodeBKMutationLogKey(logValues[0].key).first);
						for (auto & s : logValues) {
							TraceEvent("DRU_LeftOverKey").detail("key", printable(StringRef(s.key.toString()))).detail("value", printable(StringRef(s.value.toString())));
							printf("   Key: %-50s  Value: %s\n", printable(StringRef(s.key.toString())).c_str(), printable(StringRef(s.value.toString())).c_str());
						}
					}
					else {
						printf("No left over backup log keys\n");
					}
				}

				break;
			}
			catch (Error &e) {
				TraceEvent("DRU_checkError").error(e);
				Void _ = wait(tr->onError(e));
			}
		}

		if (displaySystemKeys) {
			Void _ = wait(TaskBucket::debugPrintRange(cx, LiteralStringRef("\xff"), StringRef()));
		}

		return Void();
	}

	ACTOR static Future<Void> _setup(Database cx, BackupToDBUpgradeWorkload* self) {
		state DatabaseBackupAgent backupAgent(cx);
		state Future<Void> disabler = disableConnectionFailuresAfter(300, "BackupToDBUpgradeSetup");

		try{
			Void _ = wait(delay(self->backupAfter));

			TraceEvent("DRU_doBackup").detail("tag", printable(self->backupTag));
			state Future<Void> b = doBackup(self, &backupAgent, self->extraDB, self->backupTag, self->backupRanges);

			TraceEvent("DRU_doBackupWait").detail("backupTag", printable(self->backupTag));
			Void _ = wait(b);
			TraceEvent("DRU_doBackupWaitEnd").detail("backupTag", printable(self->backupTag));
		}
		catch (Error& e) {
			TraceEvent(SevError, "BackupToDBUpgradeSetuEerror").error(e);
			throw;
		}

		return Void();
	}

	ACTOR static Future<Void> diffRanges(Standalone<VectorRef<KeyRangeRef>> ranges, StringRef backupPrefix, Database src, Database dest) {
		state int rangeIndex;
		for (rangeIndex = 0; rangeIndex < ranges.size(); ++rangeIndex) {
			state KeyRangeRef range = ranges[rangeIndex];
			state Key begin = range.begin;
			if(range.empty()) {
				continue;
			}
			loop {
				state Transaction tr(src);
				state Transaction tr2(dest);
				try {
					loop {
						tr.setOption(FDBTransactionOptions::LOCK_AWARE);
						tr2.setOption(FDBTransactionOptions::LOCK_AWARE);
						state Future<Standalone<RangeResultRef>> srcFuture = tr.getRange(KeyRangeRef(begin, range.end), 1000);
						state Future<Standalone<RangeResultRef>> bkpFuture = tr2.getRange(KeyRangeRef(begin, range.end).withPrefix(backupPrefix), 1000);
						Void _ = wait(success(srcFuture) && success(bkpFuture));

						auto src = srcFuture.get().begin();
						auto bkp = bkpFuture.get().begin();

						while (src != srcFuture.get().end() && bkp != bkpFuture.get().end()) {
							KeyRef bkpKey = bkp->key.substr(backupPrefix.size());
							if (src->key != bkpKey && src->value != bkp->value) {
								TraceEvent(SevError, "MismatchKeyAndValue").detail("srcKey", printable(src->key)).detail("srcVal", printable(src->value)).detail("bkpKey", printable(bkpKey)).detail("bkpVal", printable(bkp->value));
							}
							else if (src->key != bkpKey) {
								TraceEvent(SevError, "MismatchKey").detail("srcKey", printable(src->key)).detail("srcVal", printable(src->value)).detail("bkpKey", printable(bkpKey)).detail("bkpVal", printable(bkp->value));
							}
							else if (src->value != bkp->value) {
								TraceEvent(SevError, "MismatchValue").detail("srcKey", printable(src->key)).detail("srcVal", printable(src->value)).detail("bkpKey", printable(bkpKey)).detail("bkpVal", printable(bkp->value));
							}
							begin = std::min(src->key, bkpKey);
							if (src->key == bkpKey) {
								++src;
								++bkp;
							}
							else if (src->key < bkpKey) {
								++src;
							}
							else {
								++bkp;
							}
						}
						while (src != srcFuture.get().end() && !bkpFuture.get().more) {
							TraceEvent(SevError, "MissingBkpKey").detail("srcKey", printable(src->key)).detail("srcVal", printable(src->value));
							begin = src->key;
							++src;
						}
						while (bkp != bkpFuture.get().end() && !srcFuture.get().more) {
							TraceEvent(SevError, "MissingSrcKey").detail("bkpKey", printable(bkp->key.substr(backupPrefix.size()))).detail("bkpVal", printable(bkp->value));
							begin = bkp->key;
							++bkp;
						}

						if (!srcFuture.get().more && !bkpFuture.get().more) {
							break;
						}

						begin = keyAfter(begin);
					}

					break;
				}
				catch (Error &e) {
					Void _ = wait(tr.onError(e));
				}
			}
		}

		return Void();
	}

	ACTOR static Future<Void> _start(Database cx, BackupToDBUpgradeWorkload* self) {
		state DatabaseBackupAgent backupAgent(cx);
		state DatabaseBackupAgent restoreAgent(self->extraDB);
		state Future<Void> disabler = disableConnectionFailuresAfter(300, "BackupToDBUpgradeStart");
		state Standalone<VectorRef<KeyRangeRef>> prevBackupRanges;
		state UID logUid;
		state Version commitVersion;

		state Future<Void> stopDifferential = delay(self->stopDifferentialAfter);
		state Future<Void> waitUpgrade = backupAgent.waitUpgradeToLatestDrVersion(self->extraDB, self->backupTag);
		Void _ = wait(success(stopDifferential) && success(waitUpgrade));
		TraceEvent("DRU_waitDifferentialEnd").detail("tag", printable(self->backupTag));

		try {
			// Get restore ranges before aborting
			state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(self->extraDB));
			loop {
				try {
					// Get backup ranges
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					UID _logUid = wait(backupAgent.getLogUid(tr, self->backupTag));
					logUid = _logUid;

					Optional<Key> backupKeysPacked = wait(tr->get(backupAgent.config.get(BinaryWriter::toValue(logUid, Unversioned())).pack(BackupAgentBase::keyConfigBackupRanges)));
					ASSERT(backupKeysPacked.present());

					BinaryReader br(backupKeysPacked.get(), IncludeVersion());
					prevBackupRanges = Standalone<VectorRef<KeyRangeRef>>();
					br >> prevBackupRanges;
					Void _ = wait( lockDatabase(tr, logUid) );
					tr->addWriteConflictRange(singleKeyRange(StringRef()));
					Void _ = wait( tr->commit() );
					commitVersion = tr->getCommittedVersion();
					break;
				} catch( Error &e ) {
					Void _ = wait( tr->onError(e) );
				}
			}

			TraceEvent("DRU_locked").detail("lockedVersion", commitVersion);

			// Wait for the destination to apply mutations up to the lock commit before switching over.
			state ReadYourWritesTransaction versionCheckTr(self->extraDB);
			loop {
				try {
					versionCheckTr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					versionCheckTr.setOption(FDBTransactionOptions::LOCK_AWARE);
					Optional<Value> v = wait(versionCheckTr.get(BinaryWriter::toValue(logUid, Unversioned()).withPrefix(applyMutationsBeginRange.begin)));
					TraceEvent("DRU_applied").detail("appliedVersion", v.present() ? BinaryReader::fromStringRef<Version>(v.get(), Unversioned()) : -1);
					if( v.present() && BinaryReader::fromStringRef<Version>(v.get(), Unversioned()) >= commitVersion)
						break;

					state Future<Void> versionWatch = versionCheckTr.watch(BinaryWriter::toValue(logUid, Unversioned()).withPrefix(applyMutationsBeginRange.begin));
					Void _ = wait(versionCheckTr.commit());
					Void _ = wait(versionWatch);
					versionCheckTr.reset();
				} catch( Error &e ) {
					Void _ = wait(versionCheckTr.onError(e));
				}
			}

			TraceEvent("DRU_diffRanges");
			Void _ = wait( diffRanges(prevBackupRanges, self->backupPrefix, cx, self->extraDB ) );

			// abort backup
			TraceEvent("DRU_abortBackup").detail("tag", printable(self->backupTag));
			Void _ = wait(backupAgent.abortBackup(self->extraDB, self->backupTag));
			Void _ = wait( unlockDatabase(self->extraDB, logUid) );

			// restore database
			TraceEvent("DRU_prepareRestore").detail("restoreTag", printable(self->restoreTag));
			state Reference<ReadYourWritesTransaction> tr2(new ReadYourWritesTransaction(cx));
			loop{
				try{
					tr2->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					tr2->setOption(FDBTransactionOptions::LOCK_AWARE);
					for (auto r : prevBackupRanges) {
						if(!r.empty()) {
							std::cout << "r: " << r.begin.printable() << " - " << r.end.printable() << std::endl;
							tr2->addReadConflictRange(r);
							tr2->clear(r);
						}
					}
					Void _ = wait( tr2->commit() );
					break;
				} catch( Error &e ) {
					TraceEvent("DRU_restoreSetupError").error(e, true);
					Void _ = wait( tr2->onError(e) );
				}
			}

			state Standalone<VectorRef<KeyRangeRef>> restoreRanges;
			for (auto r : prevBackupRanges) {
				restoreRanges.push_back_deep(restoreRanges.arena(), KeyRangeRef( r.begin.withPrefix(self->backupPrefix), r.end.withPrefix(self->backupPrefix) ) );
			}

			// start restoring db
			try {
				TraceEvent("DRU_restoreDb").detail("restoreTag", printable(self->restoreTag));
				Void _ = wait(restoreAgent.submitBackup(cx, self->restoreTag, restoreRanges, true, StringRef(), self->backupPrefix));
			}
			catch (Error& e) {
				TraceEvent("DRU_restoreSubmitBackupError").detail("tag", printable(self->restoreTag)).error(e);
				if (e.code() != error_code_backup_unneeded && e.code() != error_code_backup_duplicate)
					throw;
			}

			int _ = wait(restoreAgent.waitBackup(cx, self->restoreTag));
			Void _ = wait(restoreAgent.unlockBackup(cx, self->restoreTag));
			Void _ = wait(checkData(self->extraDB, logUid, logUid, self->backupTag, &backupAgent, self->shareLogRange));

			state UID restoreUid = wait(restoreAgent.getLogUid(cx, self->restoreTag));
			Void _ = wait(checkData(cx, restoreUid, restoreUid, self->restoreTag, &restoreAgent, self->shareLogRange));

			TraceEvent("DRU_complete").detail("backupTag", printable(self->backupTag));

			if (g_simulator.drAgents == ISimulator::BackupToDB) {
				g_simulator.drAgents = ISimulator::NoBackupAgents;
			}
		} catch (Error& e) {
			TraceEvent(SevError, "BackupAndRestoreCorrectnessError").error(e);
			throw;
		}

		return Void();
	}
};

WorkloadFactory<BackupToDBUpgradeWorkload> BackupToDBUpgradeWorkloadFactory("BackupToDBUpgrade");
