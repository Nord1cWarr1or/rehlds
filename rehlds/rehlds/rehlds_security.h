#pragma once

#include "engine.h"

class CMoveCommandRateLimiter {
public:
	CMoveCommandRateLimiter();
	void Frame();
	void MoveCommandsIssued(unsigned int clientId, unsigned int numCmds);
	void ClientConnected(unsigned int clientId);

private:
	void UpdateAverageRates(double currentTime);
	void CheckBurstRate(unsigned int clientId);
	void CheckAverageRate(unsigned int clientId);

private:
	float m_AverageMoveCmdRate[MAX_CLIENTS];
	int m_CurrentMoveCmds[MAX_CLIENTS];
	double m_LastCheckTime;
};

extern CMoveCommandRateLimiter g_MoveCommandRateLimiter;

class CStringCommandsRateLimiter {
public:
	CStringCommandsRateLimiter();
	void Frame();
	void StringCommandIssued(unsigned int clientId);
	void ClientConnected(unsigned int clientId);

private:
	void UpdateAverageRates(double currentTime);
	void CheckBurstRate(unsigned int clientId);
	void CheckAverageRate(unsigned int clientId);

private:
	float m_AverageStringCmdRate[MAX_CLIENTS];
	int m_CurrentStringCmds[MAX_CLIENTS];
	double m_LastCheckTime;
};

extern CStringCommandsRateLimiter g_StringCommandsRateLimiter;

class CDlFileRateLimiter {
public:
	CDlFileRateLimiter();
	// Consumes one token for a dlfile request. Returns TRUE when the request
	// must be dropped because the client's bucket is empty.
	qboolean DlFileIssued(unsigned int clientId);
	void ClientConnected(unsigned int clientId);

private:
	float GetBucketCapacity();
	void CheckEmptyStrikes(unsigned int clientId);

private:
	float m_Tokens[MAX_CLIENTS];
	double m_LastRefillTime[MAX_CLIENTS];
	unsigned int m_EmptyStrikes[MAX_CLIENTS];
};

extern CDlFileRateLimiter g_DlFileRateLimiter;

class CUserCmdTimeLimiter {
public:
	CUserCmdTimeLimiter();
	void Frame();
	bool CheckLimits(unsigned int clientId, usercmd_t *ucmd);
	void ClientConnected(unsigned int clientId);

	// SV_ParseMove: sequence is the netchan sequence of the move packet,
	// netDrop/numBackup are the engine's lost-command bookkeeping for it
	void OnMoveParsed(unsigned int clientId, int sequence, int netDrop, int numBackup);

	// telemetry hooks
	void OnClockWindowSet(unsigned int clientId, double dif);
	void OnCmdSkippedByClockWindow(unsigned int clientId);

	// read-only view of the measurement state (diagnostics, unit tests)
	struct measure_info_t {
		double rate;               // client speed over the sliding window, 0 - window not ready
		double winSec;             // real time the window covers
		unsigned int detectSpeed;  // commands judged as speedhack
		unsigned int detectSlow;   // commands judged as slowmo
		unsigned int budgetClamps;
		unsigned int lostPackets;  // client packets lost on the way (netchan sequence gaps)
		uint64_t exclGapMs;        // real time kept out of the window: client silence
		uint64_t exclLossMs;       // real time kept out of the window: lost client packets
	};
	void GetMeasureInfo(unsigned int clientId, measure_info_t *info) const;

private:

	// The detector measures the client's game-time speed over a sliding
	// window. Control points are running (wall, msec) totals; speed is the
	// msec delta divided by the wall delta between two points.
	static const int RATE_POINTS_MAX = 24;

	void DumpClientState(unsigned int clientId);

	enum TimeAbuseType {
		ABUSE_NONE = -1,
		ABUSE_SPEEDHACK,
		ABUSE_SLOWMO,
		ABUSE_MAX
	};

	// why the current excluded span started
	enum ExclReason {
		EXCL_GAP,   // client silence longer than gap_reset (or the first command)
		EXCL_LOSS   // client packets lost on the way to the server
	};

	struct rate_point_t {
		uint64_t wallMs;   // running wall total at sampling moment
		uint64_t msecMs;   // running client-time total at sampling moment
	};

	struct usercmd_state_t {
		// limits state
		unsigned int ticksThisFrame;
		unsigned int consecutiveNullCmds;

		// time accounting (accepted commands only)
		uint64_t joinTime;
		uint64_t lastUpdateTime;   // realtime ms of the last command that passed the drop gates
		uint64_t rateWallMs;       // wall time accumulated over accepted short intervals
		uint64_t rateMsecMs;       // client time accumulated over the same intervals
		uint64_t totalMsec;        // every msec since connect (diagnostics)
		double avgMsec;            // EMA of per-command msec (client fps estimate)
		double budgetMs;           // movement-time allowance (refills with wall time, spent by msec)

		// speed window
		rate_point_t ratePoints[RATE_POINTS_MAX];
		int ratePointCount;
		unsigned int rateRestarts; // window restarts after long silence
		uint64_t pendingDroppedMsec; // time of max_ticks-dropped cmds, added to the window with the next accepted interval

		// excluded intervals: an interval that cannot be measured (long
		// silence, lost packets) is kept out of the window together with the
		// rest of its frame and, for a silence, the client time that
		// arrives late for it
		uint64_t exclFrameMs;      // realtime ms of the frame the last excluded interval opened
		ExclReason exclFrameReason;
		uint64_t exclDebtMs;       // client time still expected for an excluded silence
		uint64_t exclDebtSinceMs;  // realtime ms the silence debt was last added
		bool lossPending;          // a packet arrived after lost ones, its commands are not judged
		int lastMoveSeq;           // netchan sequence of the last move packet, 0 - none yet

		// punishment state
		unsigned int warnings[ABUSE_MAX];
		uint64_t lastDetectMs;     // throttle of warning accrual inside one episode
		uint64_t stableSinceMs;    // start of the current "speed in range" streak

		// telemetry (never affects punishment decisions)
		unsigned int telemWarn[ABUSE_MAX];
		unsigned int ticksDrops;        // cmds dropped by sv_rehlds_movecmd_max_ticks
		unsigned int nullDrops;         // cmds dropped by sv_rehlds_movecmd_max_null_streak
		unsigned int interpDrops;       // cmds dropped by sv_rehlds_movecmd_clamp_interp
		unsigned int budgetClamps;      // commands clipped by the movement-time budget
		unsigned int stallImmuneSkipped; // slowmo warnings not accrued because a server stall was detected
		unsigned int abuseDrops[ABUSE_MAX]; // cmds dropped by abuse detection (speedhack only)
		unsigned int cwCount;           // vanilla clockwindow ignore windows set on this client
		unsigned int cwSkippedCmds;     // cmds skipped during current/last clockwindow window
		unsigned int cwSkippedTotal;    // total cmds skipped by clockwindow since connect
		bool cwActive;                  // inside (or just left) a clockwindow ignore window
		unsigned int replayedCmds;      // lost cmds replayed from lastcmd (bypass CheckLimits)
		unsigned int lostCmds;          // lost cmds not run at all (net_drop >= 24)
		unsigned int lostPackets;       // client packets lost on the way (netchan sequence gaps)
		uint64_t exclGapMs;             // real time excluded from the window: client silence
		uint64_t exclLossMs;            // real time excluded from the window: lost packets
		uint64_t dumpExclLossMs;        // exclLossMs at the previous state dump
		double lastDumpTime;
		double nextDumpTime;
		double lastWarnLogTime;
		double lastDropLogTime;
	};

	usercmd_state_t m_States[MAX_CLIENTS];

private:
	void PushRatePoint(usercmd_state_t *ust);
	static bool WindowTotals(const usercmd_state_t *ust, uint64_t *dWall, uint64_t *dMsec);
};

extern CUserCmdTimeLimiter g_UserCmdTimeLimiter;

extern void Rehlds_Security_Init();
extern void Rehlds_Security_Shutdown();
extern void Rehlds_Security_Frame();
extern void Rehlds_Security_ClientConnected(unsigned int clientId);
