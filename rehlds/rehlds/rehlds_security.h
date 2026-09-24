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

	// telemetry hooks (debug/movecmdtime-telemetry branch)
	void OnClockWindowSet(unsigned int clientId, double dif);
	void OnCmdSkippedByClockWindow(unsigned int clientId);

private:

	double TimeDifference(uint64_t start, uint64_t end) const;
	void DumpClientState(unsigned int clientId);

	enum TimeAbuseType {
		ABUSE_NONE = -1,
		ABUSE_SPEEDHACK,
		ABUSE_SLOWMO,
		ABUSE_MAX
	};

	struct usercmd_state_t {
		// limits state
		unsigned int ticksThisFrame;
		unsigned int consecutiveNullCmds;

		// time drift state
		uint64_t msecTime;
		uint64_t joinTime;
		uint64_t lastUpdateTime;
		double avgMsec;
		double avgServerTime;
		double accMsecStable;
		uint64_t numFrames;
		unsigned int lastTickTime;
		unsigned int warnings[ABUSE_MAX];

		// telemetry state (never affects punishment decisions)
		uint64_t totalMsec;             // all msec accepted by the drift section
		uint64_t errorBelowSinceMs;     // 0 = error not below -max_error
		unsigned int telemWarn[ABUSE_MAX]; // every detected abuse event, punish-independent
		unsigned int ticksDrops;        // cmds dropped by sv_rehlds_movecmd_max_ticks
		unsigned int nullDrops;         // cmds dropped by sv_rehlds_movecmd_max_null_streak
		unsigned int interpDrops;       // cmds dropped by sv_rehlds_movecmd_clamp_interp
		unsigned int abuseDrops[ABUSE_MAX]; // cmds dropped by abuse detection
		unsigned int dipEvents;         // times ratio fell below min_scale
		unsigned int dipsNearRollover;  // of those, dips within 10 samples of a window rollover
		unsigned int cwCount;           // vanilla clockwindow ignore windows set on this client
		unsigned int cwSkippedCmds;     // cmds skipped during current/last clockwindow window
		unsigned int cwSkippedTotal;    // total cmds skipped by clockwindow since connect
		bool cwActive;                  // inside (or just left) a clockwindow ignore window
		bool inDip;                     // currently inside a ratio dip
		double nextDumpTime;
		double lastWarnLogTime;
		double lastDipLogTime;
		double lastDropLogTime;
	};

	usercmd_state_t m_States[MAX_CLIENTS];
};

extern CUserCmdTimeLimiter g_UserCmdTimeLimiter;

extern void Rehlds_Security_Init();
extern void Rehlds_Security_Shutdown();
extern void Rehlds_Security_Frame();
extern void Rehlds_Security_ClientConnected(unsigned int clientId);
