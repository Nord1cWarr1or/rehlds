#include "precompiled.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

cvar_t sv_rehlds_movecmdrate_max_avg = { "sv_rehlds_movecmdrate_max_avg", "1800", 0, 1800.0f, NULL };
cvar_t sv_rehlds_movecmdrate_max_burst = { "sv_rehlds_movecmdrate_max_burst", "5500", 0, 5500.0f, NULL };
cvar_t sv_rehlds_stringcmdrate_max_avg = { "sv_rehlds_stringcmdrate_max_avg", "250", 0, 250.0f, NULL };
cvar_t sv_rehlds_stringcmdrate_max_burst = { "sv_rehlds_stringcmdrate_max_burst", "500", 0, 500.0f, NULL };

cvar_t sv_rehlds_movecmdrate_avg_punish = { "sv_rehlds_movecmdrate_avg_punish", "5", 0, 5.0f, NULL };
cvar_t sv_rehlds_movecmdrate_burst_punish = { "sv_rehlds_movecmdrate_burst_punish", "5", 0, 5.0f, NULL };
cvar_t sv_rehlds_stringcmdrate_avg_punish = { "sv_rehlds_stringcmdrate_avg_punish", "5", 0, 5.0f, NULL };
cvar_t sv_rehlds_stringcmdrate_burst_punish = { "sv_rehlds_stringcmdrate_burst_punish", "5", 0, 5.0f, NULL };

cvar_t sv_rehlds_dlfile_bucket_size = { "sv_rehlds_dlfile_bucket_size", "0", 0, 0.0f, NULL };
cvar_t sv_rehlds_dlfile_refillrate = { "sv_rehlds_dlfile_refillrate", "50", 0, 50.0f, NULL };
cvar_t sv_rehlds_dlfile_punish = { "sv_rehlds_dlfile_punish", "-1", 0, -1.0f, NULL };

// dlfile requests that arrive with an empty bucket are dropped; a client that
// keeps hammering a dropped bucket this many times in a row is flooding the
// filesystem on purpose.
const unsigned int MAX_DLFILE_EMPTY_STRIKES = 100;
// The default bucket is sized from the map resource list, so a legitimate
// client can always fetch every missing resource with one connect batch.
const float DLFILE_BUCKET_MARGIN = 16.0f;

cvar_t sv_rehlds_movecmd_max_ticks = { "sv_rehlds_movecmd_max_ticks", "24", 0, 24.0f, NULL };
cvar_t sv_rehlds_movecmd_max_null_streak = { "sv_rehlds_movecmd_max_null_streak", "0", 0, 0.0f, NULL };
cvar_t sv_rehlds_movecmd_clamp_interp = { "sv_rehlds_movecmd_clamp_interp", "1", 0, 1.0f, NULL };
cvar_t sv_rehlds_movecmdtime_samples = { "sv_rehlds_movecmdtime_samples", "120", 0, 120.0f, NULL };
cvar_t sv_rehlds_movecmdtime_max_scale = { "sv_rehlds_movecmdtime_max_scale", "3.0", 0, 3.0f, NULL };
cvar_t sv_rehlds_movecmdtime_min_scale = { "sv_rehlds_movecmdtime_min_scale", "0.5", 0, 0.5f, NULL };
cvar_t sv_rehlds_movecmdtime_punish = { "sv_rehlds_movecmdtime_punish", "-1", 0, -1.0f, NULL };
cvar_t sv_rehlds_movecmdtime_max_warnings = { "sv_rehlds_movecmdtime_max_warnings", "-1", 0, -1.0f, NULL };
cvar_t sv_rehlds_movecmdtime_debug = { "sv_rehlds_movecmdtime_debug", "0", 0, 0.0f, NULL };
cvar_t sv_rehlds_movecmdtime_gap_reset = { "sv_rehlds_movecmdtime_gap_reset", "0.5", 0, 0.5f, NULL };
cvar_t sv_rehlds_movecmdtime_rate_min_window = { "sv_rehlds_movecmdtime_rate_min_window", "15", 0, 15.0f, NULL };

CMoveCommandRateLimiter g_MoveCommandRateLimiter;
CStringCommandsRateLimiter g_StringCommandsRateLimiter;
CUserCmdTimeLimiter g_UserCmdTimeLimiter;

// movecmdtime telemetry: one line per event to logs/movecmdtime_debug.log
// (falls back to the server dir, then to console). Collects data even when
// punishment is disabled.
static FILE *g_pMoveCmdTimeLog = NULL;

static void MCmd_Log(const char *fmt, ...)
{
	char line[1024];
	char stamp[32];
	time_t t;
	va_list args;

	if (sv_rehlds_movecmdtime_debug.value < 1.0f) {
		return;
	}

	if (!g_pMoveCmdTimeLog) {
		g_pMoveCmdTimeLog = fopen("logs/movecmdtime_debug.log", "a");
		if (!g_pMoveCmdTimeLog) {
			g_pMoveCmdTimeLog = fopen("movecmdtime_debug.log", "a");
		}
	}

	t = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&t));

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	if (g_pMoveCmdTimeLog) {
		fprintf(g_pMoveCmdTimeLog, "[%s rt=%.3f] %s\n", stamp, realtime, line);
		fflush(g_pMoveCmdTimeLog);
	} else {
		Con_Printf("[mcmd %s rt=%.3f] %s\n", stamp, realtime, line);
	}
}
CDlFileRateLimiter g_DlFileRateLimiter;

CMoveCommandRateLimiter::CMoveCommandRateLimiter() {
	Q_memset(m_AverageMoveCmdRate, 0, sizeof(m_AverageMoveCmdRate));
	Q_memset(m_CurrentMoveCmds, 0, sizeof(m_CurrentMoveCmds));
	m_LastCheckTime = 0.0;
}

void CMoveCommandRateLimiter::UpdateAverageRates(double dt) {
	for (unsigned int i = 0; i < MAX_CLIENTS; i++) {
		m_AverageMoveCmdRate[i] = (2.0 * m_AverageMoveCmdRate[i] / 3.0) + m_CurrentMoveCmds[i] / dt / 3.0;
		m_CurrentMoveCmds[i] = 0;

		CheckAverageRate(i);
	}
}

void CMoveCommandRateLimiter::Frame() {
	double currentTime = realtime;
	double dt = currentTime - m_LastCheckTime;

	if (dt < 0.5) { //refresh avg. rate every 0.5 sec
		return;
	}

	UpdateAverageRates(dt);
	m_LastCheckTime = currentTime;
}

void CMoveCommandRateLimiter::ClientConnected(unsigned int clientId) {
	m_CurrentMoveCmds[clientId] = 0;
	m_AverageMoveCmdRate[clientId] = 0.0f;
}

void CMoveCommandRateLimiter::MoveCommandsIssued(unsigned int clientId, unsigned int numCmds) {
	m_CurrentMoveCmds[clientId] += numCmds;
	CheckBurstRate(clientId);
}

void CMoveCommandRateLimiter::CheckBurstRate(unsigned int clientId) {
	client_t* cl = &g_psvs.clients[clientId];
	if (!cl->active || sv_rehlds_movecmdrate_max_burst.value <= 0.0f) {
		return;
	}

	double dt = realtime - m_LastCheckTime;
	if (dt < 0.2) {
		dt = 0.2; //small intervals may give too high rates
	}
	if ((m_CurrentMoveCmds[clientId] / dt) > sv_rehlds_movecmdrate_max_burst.value) {
		if(sv_rehlds_movecmdrate_burst_punish.value < 0) {
			Con_DPrintf("%s Kicked for move commands flooding (burst) (%.1f)\n", cl->name, (m_CurrentMoveCmds[clientId] / dt));
			SV_DropClient(cl, false, "Kicked for move commands flooding (burst)");
		}
		else
		{
			Con_DPrintf("%s Banned for move commands flooding (burst) (%.1f)\n", cl->name, (m_CurrentMoveCmds[clientId] / dt));
			Cbuf_AddText(va("addip %.1f %s\n", sv_rehlds_movecmdrate_burst_punish.value, NET_BaseAdrToString(cl->netchan.remote_address)));
			SV_DropClient(cl, false, "Banned for move commands flooding (burst)");
		}
	}
}

void CMoveCommandRateLimiter::CheckAverageRate(unsigned int clientId) {
	client_t* cl = &g_psvs.clients[clientId];
	if (!cl->active || sv_rehlds_movecmdrate_max_avg.value <= 0.0f) {
		return;
	}

	if (m_AverageMoveCmdRate[clientId] > sv_rehlds_movecmdrate_max_avg.value) {
		if(sv_rehlds_movecmdrate_avg_punish.value < 0) {
			Con_DPrintf("%s Kicked for move commands flooding (Avg) (%.1f)\n", cl->name, m_AverageMoveCmdRate[clientId]);
			SV_DropClient(cl, false, "Kicked for move commands flooding (Avg)");
		}
		else
		{
			Con_DPrintf("%s Banned for move commands flooding (Avg) (%.1f)\n", cl->name, m_AverageMoveCmdRate[clientId]);
			Cbuf_AddText(va("addip %.1f %s\n", sv_rehlds_movecmdrate_avg_punish.value, NET_BaseAdrToString(cl->netchan.remote_address)));
			SV_DropClient(cl, false, "Banned for move commands flooding (Avg)");
		}
	}
}

CStringCommandsRateLimiter::CStringCommandsRateLimiter() {
	Q_memset(m_AverageStringCmdRate, 0, sizeof(m_AverageStringCmdRate));
	Q_memset(m_CurrentStringCmds, 0, sizeof(m_CurrentStringCmds));
	m_LastCheckTime = 0.0;
}

void CStringCommandsRateLimiter::UpdateAverageRates(double dt) {
	for (unsigned int i = 0; i < MAX_CLIENTS; i++) {
		m_AverageStringCmdRate[i] = (2.0 * m_AverageStringCmdRate[i] / 3.0) + m_CurrentStringCmds[i] / dt / 3.0;
		m_CurrentStringCmds[i] = 0;

		CheckAverageRate(i);
	}
}

void CStringCommandsRateLimiter::Frame() {
	double currentTime = realtime;
	double dt = currentTime - m_LastCheckTime;

	if (dt < 0.5) { //refresh avg. rate every 0.5 sec
		return;
	}

	UpdateAverageRates(dt);
	m_LastCheckTime = currentTime;
}

void CStringCommandsRateLimiter::ClientConnected(unsigned int clientId) {
	m_CurrentStringCmds[clientId] = 0;
	m_AverageStringCmdRate[clientId] = 0.0f;
}

void CStringCommandsRateLimiter::StringCommandIssued(unsigned int clientId) {
	m_CurrentStringCmds[clientId]++;
	CheckBurstRate(clientId);
}

void CStringCommandsRateLimiter::CheckBurstRate(unsigned int clientId) {
	client_t* cl = &g_psvs.clients[clientId];
	if (!cl->active || sv_rehlds_stringcmdrate_max_burst.value <= 0.0f) {
		return;
	}

	double dt = realtime - m_LastCheckTime;
	if (dt < 0.2) {
		dt = 0.2; //small intervals may give too high rates
	}
	if ((m_CurrentStringCmds[clientId] / dt) > sv_rehlds_stringcmdrate_max_burst.value) {
		if(sv_rehlds_stringcmdrate_burst_punish.value < 0) {
			Con_DPrintf("%s Kicked for string commands flooding (burst) (%.1f)\n", cl->name, (m_CurrentStringCmds[clientId] / dt));
			SV_DropClient(cl, false, "Kicked for string commands flooding (burst)");
		}
		else
		{
			Con_DPrintf("%s Banned for string commands flooding (burst) (%.1f)\n", cl->name, (m_CurrentStringCmds[clientId] / dt));
			Cbuf_AddText(va("addip %.1f %s\n", sv_rehlds_stringcmdrate_burst_punish.value, NET_BaseAdrToString(cl->netchan.remote_address)));
			SV_DropClient(cl, false, "Banned for string commands flooding (burst)");
		}
	}
}

void CStringCommandsRateLimiter::CheckAverageRate(unsigned int clientId) {
	client_t* cl = &g_psvs.clients[clientId];
	if (!cl->active || sv_rehlds_stringcmdrate_max_avg.value <= 0.0f) {
		return;
	}

	if (m_AverageStringCmdRate[clientId] > sv_rehlds_stringcmdrate_max_avg.value) {
		if(sv_rehlds_stringcmdrate_avg_punish.value < 0) {
			Con_DPrintf("%s Kicked for string commands flooding (Avg) (%.1f)\n", cl->name, m_AverageStringCmdRate[clientId]);
			SV_DropClient(cl, false, "Kicked for string commands flooding (Avg)");
		}
		else
		{
			Con_DPrintf("%s Banned for string commands flooding (Avg) (%.1f)\n", cl->name, m_AverageStringCmdRate[clientId]);
			Cbuf_AddText(va("addip %.1f %s\n", sv_rehlds_stringcmdrate_avg_punish.value, NET_BaseAdrToString(cl->netchan.remote_address)));
			SV_DropClient(cl, false, "Banned for string commands flooding (Avg)");
		}
	}
}

CDlFileRateLimiter::CDlFileRateLimiter() {
	Q_memset(m_Tokens, 0, sizeof(m_Tokens));
	Q_memset(m_LastRefillTime, 0, sizeof(m_LastRefillTime));
	Q_memset(m_EmptyStrikes, 0, sizeof(m_EmptyStrikes));
}

float CDlFileRateLimiter::GetBucketCapacity() {
	if (sv_rehlds_dlfile_bucket_size.value > 0.0f) {
		return sv_rehlds_dlfile_bucket_size.value;
	}

	return (float)g_psv.num_resources + DLFILE_BUCKET_MARGIN;
}

void CDlFileRateLimiter::ClientConnected(unsigned int clientId) {
	m_Tokens[clientId] = GetBucketCapacity();
	m_LastRefillTime[clientId] = realtime;
	m_EmptyStrikes[clientId] = 0;
}

qboolean CDlFileRateLimiter::DlFileIssued(unsigned int clientId) {
	// a negative bucket size disables dlfile rate limiting
	if (sv_rehlds_dlfile_bucket_size.value < 0.0f) {
		return FALSE;
	}

	if (sv_rehlds_dlfile_refillrate.value > 0.0f && realtime > m_LastRefillTime[clientId]) {
		m_Tokens[clientId] += (float)((realtime - m_LastRefillTime[clientId]) * sv_rehlds_dlfile_refillrate.value);
	}

	m_LastRefillTime[clientId] = realtime;

	float capacity = GetBucketCapacity();
	if (m_Tokens[clientId] > capacity) {
		m_Tokens[clientId] = capacity;
	}

	if (m_Tokens[clientId] >= 1.0f) {
		m_Tokens[clientId] -= 1.0f;
		m_EmptyStrikes[clientId] = 0;
		return FALSE;
	}

	// the bucket is empty: drop the request, a client that keeps asking for
	// more is flooding the filesystem on purpose (issue #1200)
	m_EmptyStrikes[clientId]++;
	CheckEmptyStrikes(clientId);
	return TRUE;
}

void CDlFileRateLimiter::CheckEmptyStrikes(unsigned int clientId) {
	client_t* cl = &g_psvs.clients[clientId];
	if (m_EmptyStrikes[clientId] < MAX_DLFILE_EMPTY_STRIKES) {
		return;
	}

	if (sv_rehlds_dlfile_punish.value < 0.0f) {
		Con_DPrintf("%s Kicked for dlfile flooding (%u dropped requests)\n", cl->name, m_EmptyStrikes[clientId]);
		SV_DropClient(cl, false, "Kicked for dlfile flooding");
	}
	else
	{
		Con_DPrintf("%s Banned for dlfile flooding (%u dropped requests)\n", cl->name, m_EmptyStrikes[clientId]);
		Cbuf_AddText(va("addip %.1f %s\n", sv_rehlds_dlfile_punish.value, NET_BaseAdrToString(cl->netchan.remote_address)));
		SV_DropClient(cl, false, "Banned for dlfile flooding");
	}

	m_EmptyStrikes[clientId] = 0;
}

CUserCmdTimeLimiter::CUserCmdTimeLimiter()
{
	Q_memset(m_States, 0, sizeof(m_States));
}

void CUserCmdTimeLimiter::ClientConnected(unsigned int clientId)
{
	Q_memset(&m_States[clientId], 0, sizeof(m_States[clientId]));
}

// detector internals (policy knobs are cvars, these are not)
#define RATE_POINT_STEP_MS   2500.0  // sample a control point each 2.5s of wall progress
#define RATE_PAUSE_RESET_MS  10000   // window restarts after this much command silence
#define MIN_RATE_MSEC_MS     3000    // need this much reported client time to judge speed
#define RATE_WARN_ACCRUE_MS  10000   // one warning accrual per this ms inside an episode
#define RATE_STABLE_DECAY_MS 60000   // in-range speed time that forgives one warning

void CUserCmdTimeLimiter::PushRatePoint(usercmd_state_t *ust, double at, const char *name)
{
	// a long command silence makes the stored window stale: restart it
	if (ust->ratePointCount > 0
		&& at - ust->ratePoints[ust->ratePointCount - 1].at > RATE_PAUSE_RESET_MS / 1000.0)
	{
		ust->ratePointCount = 0;
		ust->rateRestarts++;
		if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
			MCmd_Log("RATE-RESTART name=%s (window stale after silence)", name);
		}
	}

	if (ust->ratePointCount == RATE_POINTS_MAX) {
		memmove(&ust->ratePoints[0], &ust->ratePoints[1], sizeof(rate_point_t) * (RATE_POINTS_MAX - 1));
		ust->ratePointCount--;
	}

	ust->ratePoints[ust->ratePointCount].wallMs = ust->rateWallMs;
	ust->ratePoints[ust->ratePointCount].msecMs = ust->rateMsecMs;
	ust->ratePoints[ust->ratePointCount].at = at;
	ust->ratePointCount++;
}

#define MAX_EX_INTERP              0.1f
#define MIN_EX_INTERP              0.05f
#define MAX_EX_INTERP_SPECTATOR    0.2f

bool CUserCmdTimeLimiter::CheckLimits(unsigned int clientId, usercmd_t *ucmd)
{
	client_t *cl = &g_psvs.clients[clientId];
	if (!cl->active) {
		return false;
	}

	usercmd_state_t *ust = &m_States[clientId];

	uint64_t now = (uint64_t)(realtime * 1000.0);

	// check move command flood within a single server tick
	if (sv_rehlds_movecmd_max_ticks.value > 0)
	{
		if (ust->ticksThisFrame >= (unsigned int)sv_rehlds_movecmd_max_ticks.value) {
			ust->ticksDrops++;
			// keep the drop period out of the speed window
			ust->lastUpdateTime = now;
			if (sv_rehlds_movecmdtime_debug.value >= 2.0f && realtime - ust->lastDropLogTime >= 5.0) {
				ust->lastDropLogTime = realtime;
				// dropped BEFORE the speed accounting: wall time advances, client msec does not
				MCmd_Log("DROP-TICKS name=%s total=%u limit=%u (cmd dropped, not counted in client clock)",
					cl->name, ust->ticksDrops, (unsigned int)sv_rehlds_movecmd_max_ticks.value);
			}
			return true;
		}

		ust->ticksThisFrame++;
	}

	// air-stuck (consecutive 0 msec commands)
	// legitimate >1000 FPS clients may occasionally send msec=0 due to byte truncation,
	// but only illegitimate clients send long unbroken streaks of zero time
	if (sv_rehlds_movecmd_max_null_streak.value > 0)
	{
		if (ucmd->msec == 0)
		{
			if (++ust->consecutiveNullCmds > (unsigned int)sv_rehlds_movecmd_max_null_streak.value) {
				ust->nullDrops++;
				ust->lastUpdateTime = now;
				if (sv_rehlds_movecmdtime_debug.value >= 2.0f && realtime - ust->lastDropLogTime >= 5.0) {
					ust->lastDropLogTime = realtime;
					MCmd_Log("DROP-NULL name=%s total=%u streak=%u (cmd dropped, not counted in client clock)",
						cl->name, ust->nullDrops, ust->consecutiveNullCmds);
				}
				return true; // streak exceeded, drop command
			}
		}
		else
		{
			ust->consecutiveNullCmds = 0; // reset streak on valid time
		}
	}

	// check lerp_msec bounds
	if (sv_rehlds_movecmd_clamp_interp.value > 0) {
		int maxexinterp = cl->proxy ? (MAX_EX_INTERP_SPECTATOR * 1000.0f) : (MAX_EX_INTERP * 1000.0f);
		if (ucmd->lerp_msec < 0 || ucmd->lerp_msec > maxexinterp) {
			ust->interpDrops++;
			ust->lastUpdateTime = now;
			if (sv_rehlds_movecmdtime_debug.value >= 1.0f && realtime - ust->lastDropLogTime >= 5.0) {
				ust->lastDropLogTime = realtime;
				MCmd_Log("DROP-INTERP name=%s total=%u lerp_msec=%d limit=%d (cmd dropped, not counted in client clock)",
					cl->name, ust->interpDrops, (int)ucmd->lerp_msec, maxexinterp);
			}
			return true;
		}
	}

	//
	// Time speed detection
	//

	// sv_rehlds_movecmdtime_samples <= 0 keeps working as the detector master switch
	if (sv_rehlds_movecmdtime_samples.value <= 0.0f) {
		return false;
	}

	// first command after a clockwindow ignore window; vanilla skipped every
	// command in it before CheckLimits saw them - the gap rule below keeps
	// that interval out of the speed accounting
	if (ust->cwActive)
	{
		ust->cwActive = false;
		uint64_t cwGapMs = (ust->lastUpdateTime != 0 && now > ust->lastUpdateTime) ? (now - ust->lastUpdateTime) : 0;
		if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
			MCmd_Log("CW-RESUME name=%s gap=%llums skipped=%u (clockwindow ignore ended)",
				cl->name, (unsigned long long)cwGapMs, ust->cwSkippedCmds);
		}
		ust->cwSkippedCmds = 0;
	}

	// Wall time between accepted commands counts towards the speed window
	// only when shorter than sv_rehlds_movecmdtime_gap_reset - long
	// silences mean the client was not playing.
	uint64_t gapMs = (ust->lastUpdateTime != 0 && now > ust->lastUpdateTime) ? (now - ust->lastUpdateTime) : 0;
	bool intervalValid = (ust->lastUpdateTime != 0
		&& sv_rehlds_movecmdtime_gap_reset.value > 0.0f
		&& gapMs <= (uint64_t)(sv_rehlds_movecmdtime_gap_reset.value * 1000.0f));

	if (ust->lastUpdateTime == 0) {
		ust->joinTime = now;
		ust->lastUpdateTime = now;
	}
	ust->lastUpdateTime = now;

	ust->totalMsec += ucmd->msec;
	ust->avgMsec = (ust->avgMsec == 0.0) ? (double)ucmd->msec : ust->avgMsec * 0.95 + ucmd->msec * 0.05;

	if (intervalValid) {
		ust->rateWallMs += gapMs;
		ust->rateMsecMs += ucmd->msec;
	}

	// sample a control point every RATE_POINT_STEP_MS of wall progress;
	// a pause longer than RATE_PAUSE_RESET_MS makes the stored window stale
	if (ust->ratePointCount == 0
		|| ust->rateWallMs - ust->ratePoints[ust->ratePointCount - 1].wallMs >= RATE_POINT_STEP_MS)
	{
		PushRatePoint(ust, realtime, cl->name);
	}

	// measure the client's game-time speed over the sliding window
	TimeAbuseType abuseType = ABUSE_NONE;
	double rate = 0.0;
	double winSec = 0.0;

	if (ust->ratePointCount >= 2)
	{
		const rate_point_t *first = &ust->ratePoints[0];
		const rate_point_t *last = &ust->ratePoints[ust->ratePointCount - 1];
		uint64_t dWall = last->wallMs - first->wallMs;
		uint64_t dMsec = last->msecMs - first->msecMs;
		winSec = dWall / 1000.0;

		// need enough real time for smoothing and enough reported client
		// time - degenerate msec=0 streams are the null-streak limiter's job
		if (dWall >= (uint64_t)(sv_rehlds_movecmdtime_rate_min_window.value * 1000.0f) && dMsec >= MIN_RATE_MSEC_MS)
		{
			rate = (double)dMsec / (double)dWall;

			if (rate > sv_rehlds_movecmdtime_max_scale.value) {
				abuseType = ABUSE_SPEEDHACK;
			} else if (rate < sv_rehlds_movecmdtime_min_scale.value) {
				abuseType = ABUSE_SLOWMO;
			}
		}
	}

	if (abuseType != ABUSE_NONE)
	{
		// counted even when punishment is disabled, for telemetry
		ust->abuseDrops[(int)abuseType]++;
		ust->telemWarn[(int)abuseType]++;

		if (sv_rehlds_movecmdtime_debug.value >= 1.0f && realtime - ust->lastWarnLogTime >= 2.0)
		{
			ust->lastWarnLogTime = realtime;
			MCmd_Log("WARN name=%s type=%s total=%u rate=%.2f winSec=%.1f avgMsec=%.1f fps~%.0f loss=%u cw=%u drops(t=%u n=%u i=%u)",
				cl->name,
				(abuseType == ABUSE_SPEEDHACK) ? "speedhack" : "slowmo",
				ust->telemWarn[(int)abuseType],
				rate, winSec, ust->avgMsec,
				(ust->avgMsec > 0.0) ? (1000.0 / ust->avgMsec) : 0.0,
				(unsigned int)cl->packet_loss,
				ust->cwCount, ust->ticksDrops, ust->nullDrops, ust->interpDrops);
		}

		// one warning per RATE_WARN_ACCRUE_MS of the ongoing episode: a
		// session-long cheat still accumulates, a glitch does not stack
		if (now >= ust->lastDetectMs + RATE_WARN_ACCRUE_MS)
		{
			ust->lastDetectMs = now;
			ust->warnings[(int)abuseType]++;

			if (sv_rehlds_movecmdtime_max_warnings.value >= 0.0f
				&& ust->warnings[(int)abuseType] > (unsigned int)sv_rehlds_movecmdtime_max_warnings.value)
			{
				const char *punishReason = (abuseType == ABUSE_SPEEDHACK) ? "speedhack" : "slowmo";

				if (sv_rehlds_movecmdtime_punish.value < 0.0f) {
					Con_Printf("%s Kicked for %s (speed %.2f)\n", cl->name, punishReason, rate);
					SV_DropClient(cl, FALSE, va("Kicked for %s", punishReason));
				} else {
					if (sv_rehlds_movecmdtime_punish.value == 0.0f) {
						Con_Printf("%s Permanently banned for %s (speed %.2f)\n", cl->name, punishReason, rate);
					} else {
						Con_Printf("%s Banned for %s (speed %.2f)\n", cl->name, punishReason, rate);
					}

					Cbuf_AddText(va("addip %.1f %s\n", sv_rehlds_movecmdtime_punish.value, NET_BaseAdrToString(cl->netchan.remote_address)));
					SV_DropClient(cl, FALSE, va("Banned for %s", punishReason));
				}

				if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
					MCmd_Log("PUNISH name=%s type=%s warnCount=%u telemWarn=%u rate=%.2f winSec=%.1f",
						cl->name, punishReason, ust->warnings[(int)abuseType], ust->telemWarn[(int)abuseType],
						rate, winSec);
				}
			}
		}

		// drop commands while the episode lasts: the client keeps gaining
		// speed it did not play through
		return true;
	}
	else
	{
		// one warning of each type decays per minute of in-range speed
		if (ust->ratePointCount >= 2 && winSec > 0.0)
		{
			if (ust->stableSinceMs == 0) {
				ust->stableSinceMs = now;
			} else if (now - ust->stableSinceMs >= RATE_STABLE_DECAY_MS) {
				if (ust->warnings[ABUSE_SPEEDHACK] > 0) ust->warnings[ABUSE_SPEEDHACK]--;
				if (ust->warnings[ABUSE_SLOWMO] > 0) ust->warnings[ABUSE_SLOWMO]--;
				ust->stableSinceMs = now - RATE_STABLE_DECAY_MS / 2;
			}
		} else {
			ust->stableSinceMs = 0;
		}
	}

	return false;
}

void CUserCmdTimeLimiter::Frame()
{
	for (unsigned int i = 0; i < MAX_CLIENTS; i++) {
		m_States[i].ticksThisFrame = 0;
	}

	if (sv_rehlds_movecmdtime_debug.value < 1.0f) {
		return;
	}

	// periodic per-client state dump, one line every 10 seconds
	for (unsigned int i = 0; i < MAX_CLIENTS; i++)
	{
		client_t *cl = &g_psvs.clients[i];
		if (!cl->connected || cl->fakeclient) {
			continue;
		}

		if (realtime < m_States[i].nextDumpTime) {
			continue;
		}

		m_States[i].nextDumpTime = realtime + 10.0;
		DumpClientState(i);
	}
}

void CUserCmdTimeLimiter::DumpClientState(unsigned int clientId)
{
	client_t *cl = &g_psvs.clients[clientId];
	usercmd_state_t *ust = &m_States[clientId];
	uint64_t now = (uint64_t)(realtime * 1000.0);

	// current measured speed over the sliding window (0 = window not ready)
	double rate = 0.0;
	double winSec = 0.0;
	if (ust->ratePointCount >= 2)
	{
		const rate_point_t *first = &ust->ratePoints[0];
		const rate_point_t *last = &ust->ratePoints[ust->ratePointCount - 1];
		uint64_t dWall = last->wallMs - first->wallMs;
		uint64_t dMsec = last->msecMs - first->msecMs;
		winSec = dWall / 1000.0;
		if (dWall > 0) {
			rate = (double)dMsec / (double)dWall;
		}
	}

	uint64_t elapsedMs = (ust->joinTime != 0 && now > ust->joinTime) ? (now - ust->joinTime) : 0;
	double clockRate = (elapsedMs > 0) ? ((double)ust->totalMsec / (double)elapsedMs) : 0.0;

	// clockRate drifts permanently after speedhack sessions (reported msec
	// cannot be unreported) - rate above is the live metric
	MCmd_Log("STATE name=%s rate=%.2f winSec=%.0f clockRate=%.2f perMsec=%.1f fps~%.0f loss=%u twarn=(s:%u m:%u) drops=(t:%u n:%u i:%u a:%u/%u) cw=%u skipped=%u restarts=%u msec=%llums age=%.0fs",
		cl->name,
		rate, winSec, clockRate,
		ust->avgMsec,
		(ust->avgMsec > 0.0) ? (1000.0 / ust->avgMsec) : 0.0,
		(unsigned int)cl->packet_loss,
		ust->telemWarn[ABUSE_SPEEDHACK], ust->telemWarn[ABUSE_SLOWMO],
		ust->ticksDrops, ust->nullDrops, ust->interpDrops,
		ust->abuseDrops[ABUSE_SPEEDHACK], ust->abuseDrops[ABUSE_SLOWMO],
		ust->cwCount, ust->cwSkippedTotal,
		ust->rateRestarts,
		(unsigned long long)ust->totalMsec,
		elapsedMs / 1000.0);
}

void CUserCmdTimeLimiter::OnClockWindowSet(unsigned int clientId, double dif)
{
	client_t *cl = &g_psvs.clients[clientId];
	usercmd_state_t *ust = &m_States[clientId];

	ust->cwCount++;
	ust->cwActive = true;

	if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
		MCmd_Log("CW-SET name=%s drift=%+.3fs window=%.2fs total=%u (vanilla clockwindow will ignore cmds, detector clock not counted during it)",
			cl->name, dif, clockwindow.value, ust->cwCount);
	}
}

void CUserCmdTimeLimiter::OnCmdSkippedByClockWindow(unsigned int clientId)
{
	usercmd_state_t *ust = &m_States[clientId];

	ust->cwActive = true;
	ust->cwSkippedCmds++;
	ust->cwSkippedTotal++;
}

void Rehlds_Security_Init() {
#ifdef REHLDS_FIXES
	Cvar_RegisterVariable(&sv_rehlds_movecmdrate_max_avg);
	Cvar_RegisterVariable(&sv_rehlds_movecmdrate_max_burst);
	Cvar_RegisterVariable(&sv_rehlds_stringcmdrate_max_avg);
	Cvar_RegisterVariable(&sv_rehlds_stringcmdrate_max_burst);

	Cvar_RegisterVariable(&sv_rehlds_movecmdrate_avg_punish);
	Cvar_RegisterVariable(&sv_rehlds_movecmdrate_burst_punish);
	Cvar_RegisterVariable(&sv_rehlds_stringcmdrate_avg_punish);
	Cvar_RegisterVariable(&sv_rehlds_stringcmdrate_burst_punish);

	Cvar_RegisterVariable(&sv_rehlds_dlfile_bucket_size);
	Cvar_RegisterVariable(&sv_rehlds_dlfile_refillrate);
	Cvar_RegisterVariable(&sv_rehlds_dlfile_punish);

	Cvar_RegisterVariable(&sv_rehlds_movecmd_max_ticks);
	Cvar_RegisterVariable(&sv_rehlds_movecmd_max_null_streak);
	Cvar_RegisterVariable(&sv_rehlds_movecmd_clamp_interp);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_samples);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_max_scale);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_min_scale);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_punish);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_max_warnings);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_debug);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_gap_reset);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_rate_min_window);
#endif
}

void Rehlds_Security_Shutdown() {
}

void Rehlds_Security_Frame() {
#ifdef REHLDS_FIXES
	g_MoveCommandRateLimiter.Frame();
	g_StringCommandsRateLimiter.Frame();
	g_UserCmdTimeLimiter.Frame();
#endif
}

void Rehlds_Security_ClientConnected(unsigned int clientId) {
#ifdef REHLDS_FIXES
	g_MoveCommandRateLimiter.ClientConnected(clientId);
	g_StringCommandsRateLimiter.ClientConnected(clientId);
	g_UserCmdTimeLimiter.ClientConnected(clientId);
	g_DlFileRateLimiter.ClientConnected(clientId);
#endif
}
