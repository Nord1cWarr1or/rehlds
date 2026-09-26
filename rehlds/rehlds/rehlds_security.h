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

	struct rate_point_t {
		uint64_t wallMs;   // running wall total at sampling moment
		uint64_t msecMs;   // running client-time total at sampling moment
		double at;         // realtime seconds (stale-window detection)
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

		// speed window
		rate_point_t ratePoints[RATE_POINTS_MAX];
		int ratePointCount;
		unsigned int rateRestarts; // window restarts after long silence

		// punishment state
		unsigned int warnings[ABUSE_MAX];
		uint64_t lastDetectMs;     // throttle of warning accrual inside one episode
		uint64_t stableSinceMs;    // start of the current "speed in range" streak

		// telemetry (never affects punishment decisions)
		unsigned int telemWarn[ABUSE_MAX];
		unsigned int ticksDrops;        // cmds dropped by sv_rehlds_movecmd_max_ticks
		unsigned int nullDrops;         // cmds dropped by sv_rehlds_movecmd_max_null_streak
		unsigned int interpDrops;       // cmds dropped by sv_rehlds_movecmd_clamp_interp
		unsigned int abuseDrops[ABUSE_MAX]; // cmds dropped by abuse detection
		unsigned int cwCount;           // vanilla clockwindow ignore windows set on this client
		unsigned int cwSkippedCmds;     // cmds skipped during current/last clockwindow window
		unsigned int cwSkippedTotal;    // total cmds skipped by clockwindow since connect
		bool cwActive;                  // inside (or just left) a clockwindow ignore window
		double nextDumpTime;
		double lastWarnLogTime;
		double lastDropLogTime;
	};

		usercmd_state_t m_States[MAX_CLIENTS];

	private:
		void PushRatePoint(usercmd_state_t *ust, double at, const char *name);
	};

extern CUserCmdTimeLimiter g_UserCmdTimeLimiter;

extern void Rehlds_Security_Init();
extern void Rehlds_Security_Shutdown();
extern void Rehlds_Security_Frame();
extern void Rehlds_Security_ClientConnected(unsigned int clientId);
