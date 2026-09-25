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
cvar_t sv_rehlds_movecmdtime_max_error = { "sv_rehlds_movecmdtime_max_error", "300", 0, 300.0f, NULL };
cvar_t sv_rehlds_movecmdtime_max_scale = { "sv_rehlds_movecmdtime_max_scale", "3.0", 0, 3.0f, NULL };
cvar_t sv_rehlds_movecmdtime_min_scale = { "sv_rehlds_movecmdtime_min_scale", "0.5", 0, 0.5f, NULL };
cvar_t sv_rehlds_movecmdtime_punish = { "sv_rehlds_movecmdtime_punish", "-1", 0, -1.0f, NULL };
cvar_t sv_rehlds_movecmdtime_max_warnings = { "sv_rehlds_movecmdtime_max_warnings", "-1", 0, -1.0f, NULL };
cvar_t sv_rehlds_movecmdtime_debug = { "sv_rehlds_movecmdtime_debug", "0", 0, 0.0f, NULL };
cvar_t sv_rehlds_movecmdtime_gap_reset = { "sv_rehlds_movecmdtime_gap_reset", "0.5", 0, 0.5f, NULL };
cvar_t sv_rehlds_movecmdtime_recover_rate = { "sv_rehlds_movecmdtime_recover_rate", "25", 0, 25.0f, NULL };

CMoveCommandRateLimiter g_MoveCommandRateLimiter;
CStringCommandsRateLimiter g_StringCommandsRateLimiter;
CUserCmdTimeLimiter g_UserCmdTimeLimiter;

// movecmdtime telemetry: appends one line per event/state dump to
// logs/movecmdtime_debug.log (falls back to the server dir, then to console).
// Enabled by sv_rehlds_movecmdtime_debug; collects data even when punishment
// (sv_rehlds_movecmdtime_max_warnings) is disabled.
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

double CUserCmdTimeLimiter::TimeDifference(uint64_t start, uint64_t end) const
{
	if (end > start)
	{
		return (end - start) / 1000.0;
	}
	else
	{
		return ((start - end) / 1000.0) * -1.0;
	}
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
			// drop period must not crater the ratio window of the next
			// accepted command; the msec deficit it creates is healed by
			// the bounded clock recovery instead
			ust->lastUpdateTime = now;
			if (sv_rehlds_movecmdtime_debug.value >= 2.0f && realtime - ust->lastDropLogTime >= 5.0) {
				ust->lastDropLogTime = realtime;
				// dropped BEFORE the drift section: wall time advances, msecTime does not
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
	// Time Drift / Speedhack Detection
	//

	if (sv_rehlds_movecmdtime_samples.value <= 0.0f) {
		return false;
	}

	// telemetry: first analyzed command after a clockwindow ignore window.
	// Vanilla SV_CheckCmdTimes skipped every command in the window (before
	// CheckLimits ever saw them), so the wall time of the window is missing
	// from msecTime and shows up here as a sudden negative error jump.
	if (ust->cwActive)
	{
		ust->cwActive = false;
		uint64_t cwGapMs = (ust->lastUpdateTime != 0 && now > ust->lastUpdateTime) ? (now - ust->lastUpdateTime) : 0;
		double cwErr = (ust->msecTime != 0) ? ((double)ust->msecTime - (double)now) : 0.0;
		if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
			MCmd_Log("CW-RESUME name=%s gap=%llums skipped=%u errorNow=%+.0fms (clockwindow ignore ended, detector clock was NOT rebased)",
				cl->name, (unsigned long long)cwGapMs, ust->cwSkippedCmds, cwErr);
		}
		ust->cwSkippedCmds = 0;
	}

	// Fix: a long silence (AFK, minimized client, level load, server stall)
	// is not slowmo evidence - restart all measurements from this command.
	// The command itself still runs normally.
	uint64_t gapMs = (ust->lastUpdateTime != 0 && now > ust->lastUpdateTime) ? (now - ust->lastUpdateTime) : 0;
	if (sv_rehlds_movecmdtime_gap_reset.value > 0.0f &&
		gapMs > (uint64_t)(sv_rehlds_movecmdtime_gap_reset.value * 1000.0f))
	{
		ust->msecTime = now;
		ust->lastUpdateTime = now;
		ust->avgMsec = 0.0;
		ust->avgServerTime = 0.0;
		ust->numFrames = 0;
		ust->errorBelowSinceMs = 0;
		ust->inDip = false;
		ust->gapResets++;
		if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
			MCmd_Log("GAP-RESET name=%s gap=%llums (measurements restarted)",
				cl->name, (unsigned long long)gapMs);
		}
	}

	// Initialize states for newly active clients
	if (ust->msecTime == 0) ust->msecTime = now;
	if (ust->joinTime == 0) ust->joinTime = now;
	if (ust->lastUpdateTime == 0) ust->lastUpdateTime = now;

	ust->msecTime += ucmd->msec;
	ust->totalMsec += ucmd->msec;

	uint64_t curGapMs = (now > ust->lastUpdateTime) ? (now - ust->lastUpdateTime) : 0;

	if (ust->numFrames < (uint64_t)sv_rehlds_movecmdtime_samples.value) {
		ust->avgMsec += ucmd->msec;
		ust->avgServerTime += (now - ust->lastUpdateTime);
		ust->numFrames++;
	} else {
		// normalize averages over sample window
		ust->avgMsec /= (uint64_t)sv_rehlds_movecmdtime_samples.value;
		ust->avgServerTime /= (uint64_t)sv_rehlds_movecmdtime_samples.value;
		ust->numFrames = 0;
	}

	ust->lastUpdateTime = now;

	// calc temporal desync (error) and timescale ratio
	double error = TimeDifference(now, ust->msecTime) * 1000.0;
	double timescale_ratio = 0.0f;

	if (ust->avgMsec != 0.0f && ust->avgServerTime != 0.0f) {
		timescale_ratio = ust->avgMsec / ust->avgServerTime;
	}

	TimeAbuseType abuseType = ABUSE_NONE;

	// abnormal time acceleration (speedhack)
	float maxError = sv_rehlds_movecmdtime_max_error.value;

	// detection is only allowed on a filled window: on a fresh/recently
	// normalized window the ratio is meaningless (rollover artifact,
	// replay-burst spikes)
	bool windowReady = ust->numFrames >= (uint64_t)(sv_rehlds_movecmdtime_samples.value * 0.25f);

	// Fix: deep deficit means the SERVER failed to process commands (stall,
	// map change, mass lag) - the clock is rebased and detection is skipped
	// entirely. Punishing here hit every innocent client at once.
	if (error < -(maxError * 2.0f))
	{
		ust->msecTime = now;
		ust->lastUpdateTime = now;
		ust->avgMsec = 0.0;
		ust->avgServerTime = 0.0;
		ust->numFrames = 0;
		ust->errorBelowSinceMs = 0;
		ust->inDip = false;
		ust->noWarnResets++;
		if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
			MCmd_Log("RESET-DROP name=%s error=%+.0fms (clock rebased, detection skipped)",
				cl->name, error);
		}
		return true;
	}

	// telemetry: track time spent continuously below -max_error ("dead band":
	// between -max_error and -2*max_error msecTime is never rebased, so a
	// single lag/clockwindow episode keeps the client primed for warnings)
	if (error < -maxError)
	{
		if (ust->errorBelowSinceMs == 0)
		{
			ust->errorBelowSinceMs = now;
			if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
				double perM = (ust->numFrames > 0) ? (ust->avgMsec / (double)ust->numFrames) : ust->avgMsec;
				MCmd_Log("DEFICIT-ENTER name=%s error=%+.0fms ratio=%.2f avgMsec=%.1f fps~%.0f loss=%u cw=%u interpDrops=%u tickDrops=%u",
					cl->name, error, timescale_ratio, perM,
					(perM > 0.0) ? (1000.0 / perM) : 0.0,
					(unsigned int)cl->packet_loss, ust->cwCount, ust->interpDrops, ust->ticksDrops);
			}
		}
	}
	else if (ust->errorBelowSinceMs != 0 && error > -maxError * 0.5)
	{
		if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
			MCmd_Log("DEFICIT-EXIT name=%s belowFor=%.1fs error=%+.0fms",
				cl->name, (double)(now - ust->errorBelowSinceMs) / 1000.0, error);
		}
		ust->errorBelowSinceMs = 0;
	}

	if (error > maxError)
	{
		if (windowReady && timescale_ratio > sv_rehlds_movecmdtime_max_scale.value) {
			abuseType = ABUSE_SPEEDHACK;
		}
	}
	// abnormal time deceleration (slow-mo)
	else if (error < -maxError)
	{
		if (windowReady && timescale_ratio < sv_rehlds_movecmdtime_min_scale.value) {
			abuseType = ABUSE_SLOWMO;
		}
	}

	// telemetry: ratio dip episodes, independent of the deficit zone.
	// A dip WITHOUT deficit is harmless by design (no warning can fire);
	// counting where dips happen tells us if they are rollover artifacts.
	if (windowReady && ust->avgServerTime > 0.0f && timescale_ratio < sv_rehlds_movecmdtime_min_scale.value)
	{
		if (!ust->inDip)
		{
			ust->inDip = true;
			ust->dipEvents++;
			bool nearRollover = ust->numFrames <= 10;
			if (nearRollover) {
				ust->dipsNearRollover++;
			}
			if (sv_rehlds_movecmdtime_debug.value >= 1.0f && realtime - ust->lastDipLogTime >= 2.0) {
				ust->lastDipLogTime = realtime;
				MCmd_Log("DIP name=%s ratio=%.2f win=%llu/%d nearRollover=%d gap=%llums avgMsec=%.1f avgSrv=%.1f error=%+.0fms loss=%u",
					cl->name, timescale_ratio,
					(unsigned long long)ust->numFrames, (int)sv_rehlds_movecmdtime_samples.value,
					nearRollover ? 1 : 0,
					(unsigned long long)curGapMs, ust->avgMsec, ust->avgServerTime, error,
					(unsigned int)cl->packet_loss);
			}
		}
	}
	else
	{
		ust->inDip = false;
	}

	if (abuseType != ABUSE_NONE)
	{
		// revert accumulated time
		ust->msecTime -= ucmd->msec;

		// telemetry: counts every detected abuse event even when punishment
		// is disabled (sv_rehlds_movecmdtime_max_warnings < 0)
		ust->abuseDrops[(int)abuseType]++;
		ust->telemWarn[(int)abuseType]++;

		if (sv_rehlds_movecmdtime_debug.value >= 1.0f && realtime - ust->lastWarnLogTime >= 2.0)
		{
			ust->lastWarnLogTime = realtime;
			MCmd_Log("WARN name=%s type=%s total=%u error=%+.0fms ratio=%.2f win=%llu/%d avgMsec=%.1f avgSrv=%.1f belowFor=%.0fs loss=%u cw=%u drops(t=%u n=%u i=%u)",
				cl->name,
				(abuseType == ABUSE_SPEEDHACK) ? "speedhack" : "slowmo",
				ust->telemWarn[(int)abuseType],
				error, timescale_ratio,
				(unsigned long long)ust->numFrames, (int)sv_rehlds_movecmdtime_samples.value,
				ust->avgMsec, ust->avgServerTime,
				(ust->errorBelowSinceMs != 0 && now > ust->errorBelowSinceMs) ? ((double)(now - ust->errorBelowSinceMs) / 1000.0) : 0.0,
				(unsigned int)cl->packet_loss,
				ust->cwCount, ust->ticksDrops, ust->nullDrops, ust->interpDrops);
		}

		if (sv_rehlds_movecmdtime_max_warnings.value >= 0.0f)
		{
			ust->warnings[(int)abuseType]++;

			// apply punish action if tolerance threshold is exceeded
			if (ust->warnings[(int)abuseType] > (unsigned int)sv_rehlds_movecmdtime_max_warnings.value)
			{
				const char *punishReason = (abuseType == ABUSE_SPEEDHACK) ? "speedhack" : "slowmo";

				if (sv_rehlds_movecmdtime_punish.value < 0.0f) {
					Con_DPrintf("%s Kicked for %s (%.1f)\n", cl->name, punishReason, timescale_ratio);
					SV_DropClient(cl, FALSE, va("Kicked for %s", punishReason));
				} else {
					if (sv_rehlds_movecmdtime_punish.value == 0.0f) {
						Con_DPrintf("%s Permanently banned for %s (%.1f)\n", cl->name, punishReason, timescale_ratio);
					} else {
						Con_DPrintf("%s Banned for %s (%.1f)\n", cl->name, punishReason, timescale_ratio);
					}

					Cbuf_AddText(va("addip %.1f %s\n", sv_rehlds_movecmdtime_punish.value, NET_BaseAdrToString(cl->netchan.remote_address)));
					SV_DropClient(cl, FALSE, va("Banned for %s", punishReason));
				}

				if (sv_rehlds_movecmdtime_debug.value >= 1.0f) {
					MCmd_Log("PUNISH name=%s type=%s warnCount=%u telemWarn=%u error=%+.0fms ratio=%.2f belowFor=%.0fs",
						cl->name, punishReason, ust->warnings[(int)abuseType], ust->telemWarn[(int)abuseType],
						error, timescale_ratio,
						(ust->errorBelowSinceMs != 0 && now > ust->errorBelowSinceMs) ? ((double)(now - ust->errorBelowSinceMs) / 1000.0) : 0.0);
				}
			}
		}

		// stop command processing
		return true;
	}
	else
	{
		// recover tolerance if valid packets resume
		if (timescale_ratio <= sv_rehlds_movecmdtime_max_scale.value &&
			timescale_ratio >= sv_rehlds_movecmdtime_min_scale.value)
		{
			ust->accMsecStable += (double)ucmd->msec;

			float recoveryThreshold = sv_rehlds_movecmdtime_samples.value * 2.0f;
			if (ust->accMsecStable >= recoveryThreshold)
			{
				if (ust->warnings[ABUSE_SPEEDHACK] > 0) ust->warnings[ABUSE_SPEEDHACK]--;
				if (ust->warnings[ABUSE_SLOWMO] > 0)    ust->warnings[ABUSE_SLOWMO]--;

				ust->accMsecStable = 0.0;
			}
		}
		else
		{
			ust->accMsecStable = 0;
		}

		// Fix: bounded clock recovery - heal a leftover clock deficit/gain at
		// a limited rate so a single lag episode clears in seconds, while
		// systematic slowmo/speedhack pressure (far above the recovery rate)
		// keeps error pinned past the threshold and stays detectable
		if (sv_rehlds_movecmdtime_recover_rate.value > 0.0f && curGapMs > 0)
		{
			double heal = (double)sv_rehlds_movecmdtime_recover_rate.value * (curGapMs / 1000.0);
			if (error < 0.0)
			{
				uint64_t step = (uint64_t)((-error < heal) ? -error : heal);
				ust->msecTime += step;
			}
			else if (error > 0.0)
			{
				uint64_t step = (uint64_t)((error < heal) ? error : heal);
				ust->msecTime -= step;
			}
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

	double error = (ust->msecTime != 0) ? ((double)ust->msecTime - (double)now) : 0.0;
	uint64_t elapsedMs = (ust->joinTime != 0 && now > ust->joinTime) ? (now - ust->joinTime) : 0;
	double clockRate = (elapsedMs > 0) ? ((double)ust->totalMsec / (double)elapsedMs) : 0.0;
	double perM = (ust->numFrames > 0) ? (ust->avgMsec / (double)ust->numFrames) : ust->avgMsec;
	double ratio = (ust->avgMsec != 0.0f && ust->avgServerTime != 0.0f) ? (ust->avgMsec / ust->avgServerTime) : 0.0;

	// clockRate: client msec accepted per server ms since connect. ~1.0 for a
	// healthy client; well below 1.0 = systematic under-report (loss, msec
	// truncation at high fps, clockwindow skips, dropped cmds).
	MCmd_Log("STATE name=%s err=%+.0fms belowFor=%.0fs clockRate=%.2f ratio=%.2f perMsec=%.1f perSrv=%.1f win=%llu/%d fps~%.0f loss=%u twarn=(s:%u m:%u) dips=%u(near:%u) drops=(t:%u n:%u i:%u a:%u/%u) cw=%u skipped=%u gapR=%u noWarnR=%u msec=%llums age=%.0fs",
		cl->name,
		error,
		(ust->errorBelowSinceMs != 0 && now > ust->errorBelowSinceMs) ? ((double)(now - ust->errorBelowSinceMs) / 1000.0) : 0.0,
		clockRate, ratio, perM, ust->avgServerTime,
		(unsigned long long)ust->numFrames, (int)sv_rehlds_movecmdtime_samples.value,
		(perM > 0.0) ? (1000.0 / perM) : 0.0,
		(unsigned int)cl->packet_loss,
		ust->telemWarn[ABUSE_SPEEDHACK], ust->telemWarn[ABUSE_SLOWMO],
		ust->dipEvents, ust->dipsNearRollover,
		ust->ticksDrops, ust->nullDrops, ust->interpDrops,
		ust->abuseDrops[ABUSE_SPEEDHACK], ust->abuseDrops[ABUSE_SLOWMO],
		ust->cwCount, ust->cwSkippedTotal,
		ust->gapResets, ust->noWarnResets,
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
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_max_error);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_max_scale);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_min_scale);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_punish);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_max_warnings);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_debug);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_gap_reset);
	Cvar_RegisterVariable(&sv_rehlds_movecmdtime_recover_rate);
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
