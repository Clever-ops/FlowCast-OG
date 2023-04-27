#pragma once

#include <atomic>
#include <map>
#include <string>

#include "gdxsv.pb.h"
#include "lbs_message.h"
#include "mcs_message.h"
#include "types.h"

// Mock network implementation to replay local battle log
class GdxsvBackendReplay {
   public:
	enum class State {
		None,
		Start,
		LbsStartBattleFlow,
		McsWaitJoin,
		McsSessionExchange,
		McsInBattle,
		End,
	};

	void Reset();
	bool StartFile(const char *path);
	bool StartBuffer(const char *buf, int size);
	void Open();
	void Close();
	u32 OnSockWrite(u32 addr, u32 size);
	u32 OnSockRead(u32 addr, u32 size);
	u32 OnSockPoll();

   private:
	bool Start();
	void PrintDisconnectionSummary();
	void ProcessLbsMessage();
	void ProcessMcsMessage();
	void ApplyPatch(bool first_time);
	void RestorePatch();

	State state_;
	LbsMessageReader lbs_tx_reader_;
	McsMessageReader mcs_tx_reader_;
	proto::BattleLogFile log_file_;
	std::deque<u8> recv_buf_;
	int recv_delay_;
	int me_;
	int key_msg_count_;
};