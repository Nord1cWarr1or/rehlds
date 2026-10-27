#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

TEST(IsSafeFileExtension, SecurityChecks, 1000)
{
	struct testdata_t
	{
		const char* filename;
		qboolean safe;
	};

	testdata_t testdata[] = {
		{"radio/go.wav", TRUE},
		{"radio/go.WAV", TRUE},
		{"textures.wad", TRUE},
#ifdef REHLDS_FIXES
		{"!QWERTY", TRUE},
		// TODO:
		//{"file.dll2", TRUE},
		//{"noext", FALSE},
		//{".hidden", FALSE},
		//{"subdir/.hidden", FALSE},
#else
		{"file.dll2", FALSE},
#endif
		{"../file.txt", FALSE},
		{"/home/file.txt", FALSE},
		{"C:/Windows/file.txt", FALSE},
		{"models\\terror.mdl", FALSE},
		{"file~.mdl", FALSE},
		{"file.wav.", FALSE},
		{"file.dll.txt", FALSE},
		{"halflife.wad", FALSE},
		{"pak0.pak", FALSE},
		{"xeno.wad", FALSE},
		{"file.cfg", FALSE},
		{"file.lst", FALSE},
		{"file.exe", FALSE},
		{"file.vbs", FALSE},
		{"file.com", FALSE},
		{"file.bat", FALSE},
		{"file.dll", FALSE},
		{"file.ini", FALSE},
		{"file.log", FALSE},
		{"file.so", FALSE},
		{"file.dylib", FALSE},
		{"file.sys", FALSE},
		{"file.SYS", FALSE},
	};

	for (int i = 0; i < ARRAYSIZE(testdata); i++) {
		testdata_t* d = &testdata[i];
		qboolean result = IsSafeFileToDownload(d->filename);

		char msg[256];
		Q_snprintf(msg, sizeof msg, "IsSafeFileToDownload(%s) check", d->filename);
		CHECK(msg, result == d->safe);
	}

}

#ifdef REHLDS_FIXES

// movecmdtime detector simulation: one client plays against
// CUserCmdTimeLimiter. The client runs frames at a fixed fps and reports
// msec scaled by its game speed, sends a packet every few frames (new
// commands + 2 backups); the network may lose or hold packets. Like the
// GoldSrc client (CL_Move), every command takes a netchan sequence number,
// a held-back one too, so a packet's sequence moves on by its command
// count. The server advances to every packet arrival (1ms frames) and
// processes the packet the way SV_ParseMove + SV_RunCmd do: net_drop is
// the sequence gap less the held-back commands, backups run through
// CheckLimits, commands lost beyond them are replayed from lastcmd without it.

struct movesim_params_t
{
	double seconds;
	int fps;
	int cmdsPerPacket;
	int numBackup;             // cl_cmdbackup
	double speed;              // game-time scale the client reports
	unsigned int lossPermille; // random packet loss
	int fakeSkip;              // extra sequence numbers the client skips before every packet

	// network outage: every outageEvery seconds packets are lost for outageLen
	double outageEvery;
	double outageLen;

	// hold (fakelag/choke): every holdEvery seconds packets are held for
	// holdLen, then released releasePerMs packets per server frame
	double holdEvery;
	double holdLen;
	int releasePerMs;
	double releaseIntervalMs;  // > 0: one released packet per this many ms instead (a slow link)

	// AFK: the client generates no commands at all
	double silenceAt;
	double silenceLen;

	// client hitch: no frames for hitchLen, the next frame carries the time
	// capped at 250ms like Host_FilterTime does
	double hitchAt;
	double hitchLen;

	// level change: the client is inactive, its signon packets carry no
	// moves but take sequence numbers the server's netchan does see
	double levelChangeAt;
	double levelChangeLen;
	int signonPackets;

	// server freeze (a freeze plugin sets FL_FROZEN): the engine zeroes the
	// msec of the new commands, the client keeps sending them. With
	// frozenEvery > 0 the freeze repeats every that many seconds.
	double frozenAt;
	double frozenLen;
	double frozenEvery;

	// sv_rehlds_movecmd_max_null_streak for the run
	int nullStreak;

	// sv_rehlds_movecmdtime_min_scale for the run, 0 - keep the default
	float minScale;

	// sv_rehlds_movecmdtime_budget_max and _speedhack_budget for the run
	float budgetMax;
	int speedhackBudget;

	// sv_rehlds_movecmdtime_*_warnings for the run
	int speedhackWarnings;
	int slowmoWarnings;
	int airstuckWarnings;

	// sv_rehlds_movecmdtime_debug for the run (1 - observe: the detector
	// measures and judges with every switch off); with activateAt >= 0 it is
	// 0 until activateAt, 1 from it, 0 from deactivateAt, 1 from reactivateAt
	int debug;
	double activateAt;
	double deactivateAt;
	double reactivateAt;

	// sv_rehlds_movecmdtime_speedhack_drop for the run
	int speedhackDrop;

	// with speedLen > 0 the client runs at speed only from speedAt for
	// speedLen seconds, at real speed otherwise
	double speedAt;
	double speedLen;

	// air-stuck cheat: the client keeps sending commands with no time in
	// them; with stuckTickEvery > 0 every that many-th command carries 1ms
	// (dodges a plain zero-streak limit)
	double stuckAt;
	double stuckLen;
	int stuckTickEvery;
	int stuckSkipEvery;        // while stuck, every that many-th packet skips a sequence number (faked loss)
};

struct movesim_result_t
{
	CUserCmdTimeLimiter::measure_info_t info;
	unsigned int judged;  // commands that reached CheckLimits
	unsigned int dropped; // commands CheckLimits refused
	unsigned int droppedAfter; // of them, more than 3s after the speed window ended
};

struct movesim_packet_t
{
	int seq;
	int numCmds;
	byte msec[32];
	byte backup[2]; // [0] older, [1] newer
};

extern cvar_t sv_rehlds_movecmd_max_null_streak;
extern cvar_t sv_rehlds_movecmdtime_min_scale;
extern cvar_t sv_rehlds_movecmdtime_budget_max;
extern cvar_t sv_rehlds_movecmdtime_speedhack_budget;
extern cvar_t sv_rehlds_movecmdtime_speedhack_warnings;
extern cvar_t sv_rehlds_movecmdtime_slowmo_warnings;
extern cvar_t sv_rehlds_movecmdtime_airstuck_warnings;
extern cvar_t sv_rehlds_movecmdtime_debug;
extern cvar_t sv_rehlds_movecmdtime_speedhack_drop;

static double s_simDropAfterRealtime; // drops past this realtime count as droppedAfter

static client_t s_simClients[MAX_CLIENTS];
static edict_t s_simPlayer;
static byte s_simLastCmdMsec; // host_client->lastcmd.msec

static void MoveSim_Defaults(movesim_params_t *p)
{
	Q_memset(p, 0, sizeof(*p));
	p->seconds = 60.0;
	p->fps = 100;
	p->cmdsPerPacket = 3;
	p->numBackup = 2;
	p->budgetMax = 250.0f;
	p->speedhackBudget = 1;
	p->debug = 1;
	p->activateAt = -1.0;
	p->deactivateAt = -1.0;
	p->reactivateAt = -1.0;
	p->speed = 1.0;
	p->releasePerMs = 10;
	p->silenceAt = -1.0;
	p->hitchAt = -1.0;
	p->levelChangeAt = -1.0;
	p->frozenAt = -1.0;
	p->stuckAt = -1.0;
}

static void MoveSim_RunCmd(byte msec, movesim_result_t *res)
{
	usercmd_t cmd;
	Q_memset(&cmd, 0, sizeof(cmd));
	cmd.msec = msec;
	cmd.lerp_msec = 50;

	res->judged++;
	if (g_UserCmdTimeLimiter.CheckLimits(0, &cmd)) {
		res->dropped++;
		if (realtime > s_simDropAfterRealtime) {
			res->droppedAfter++;
		}
	}
}

static void MoveSim_Deliver(const movesim_params_t *p, const movesim_packet_t *pkt, double atSec, int *lastSeq, movesim_result_t *res, bool frozen)
{
	// server frames are 1ms long, a packet is processed in the frame it arrives
	double serverTime = 1000.0 + ceil(atSec * 1000.0 - 1e-6) / 1000.0;
	if (serverTime > realtime + 1e-9) {
		realtime = serverTime;
		g_UserCmdTimeLimiter.Frame();
	}

	int seqGap = (*lastSeq != 0) ? (pkt->seq - *lastSeq - 1) : 0;
	*lastSeq = pkt->seq;

	// SV_ParseMove bookkeeping: the held-back commands of this packet are
	// not lost, only what is left of the gap is
	int netDrop = seqGap + 1 - pkt->numCmds;

	// a frozen player's moves are zeroed and nothing lost is rerun
	if (frozen) {
		netDrop = 0;
	}

	g_UserCmdTimeLimiter.OnMoveParsed(0, netDrop, p->numBackup, s_simLastCmdMsec, frozen);

	if (netDrop > 0 && netDrop < 24) {
		int backups = (netDrop < p->numBackup) ? netDrop : p->numBackup;
		for (int i = 2 - backups; i < 2; i++) {
			MoveSim_RunCmd(pkt->backup[i], res);
		}
	}

	for (int i = 0; i < pkt->numCmds; i++) {
		MoveSim_RunCmd(frozen ? 0 : pkt->msec[i], res);
	}

	// REHLDS_FIXES keeps the oldest new command as lastcmd
	s_simLastCmdMsec = frozen ? 0 : pkt->msec[0];
}

static double MoveSim_ReleaseDelay(const movesim_params_t *p, int i)
{
	if (p->releaseIntervalMs > 0.0) {
		return i * p->releaseIntervalMs * 0.001;
	}
	return (i / p->releasePerMs) * 0.001;
}

static bool MoveSim_InWindow(double t, double every, double len)
{
	return every > 0.0 && len > 0.0 && fmod(t, every) < len;
}

static void MoveSim_Run(const movesim_params_t *p, movesim_result_t *res)
{
	client_t *savedClients = g_psvs.clients;
	int savedLimit = g_psvs.maxclientslimit;
	edict_t *savedPlayer = sv_player;
	double savedRealtime = realtime;
	qboolean savedPaused = g_psv.paused;
	float savedNullStreak = sv_rehlds_movecmd_max_null_streak.value;
	float savedMinScale = sv_rehlds_movecmdtime_min_scale.value;
	float savedBudgetMax = sv_rehlds_movecmdtime_budget_max.value;
	float savedSpeedhackBudget = sv_rehlds_movecmdtime_speedhack_budget.value;
	float savedSpeedhackWarnings = sv_rehlds_movecmdtime_speedhack_warnings.value;
	float savedSlowmoWarnings = sv_rehlds_movecmdtime_slowmo_warnings.value;
	float savedAirstuckWarnings = sv_rehlds_movecmdtime_airstuck_warnings.value;
	float savedDebug = sv_rehlds_movecmdtime_debug.value;
	float savedSpeedhackDrop = sv_rehlds_movecmdtime_speedhack_drop.value;

	Q_memset(res, 0, sizeof(*res));
	Q_memset(s_simClients, 0, sizeof(s_simClients));
	Q_memset(&s_simPlayer, 0, sizeof(s_simPlayer));
	s_simLastCmdMsec = 0;
	s_simClients[0].active = TRUE;
	s_simClients[0].connected = TRUE;
	g_psvs.clients = s_simClients;
	g_psvs.maxclientslimit = MAX_CLIENTS;
	sv_player = &s_simPlayer;
	g_psv.paused = FALSE;
	sv_rehlds_movecmd_max_null_streak.value = (float)p->nullStreak;
	sv_rehlds_movecmdtime_budget_max.value = p->budgetMax;
	sv_rehlds_movecmdtime_speedhack_budget.value = (float)p->speedhackBudget;
	sv_rehlds_movecmdtime_speedhack_warnings.value = (float)p->speedhackWarnings;
	sv_rehlds_movecmdtime_slowmo_warnings.value = (float)p->slowmoWarnings;
	sv_rehlds_movecmdtime_airstuck_warnings.value = (float)p->airstuckWarnings;
	sv_rehlds_movecmdtime_debug.value = (p->activateAt >= 0.0) ? 0.0f : (float)p->debug;
	sv_rehlds_movecmdtime_speedhack_drop.value = (float)p->speedhackDrop;
	s_simDropAfterRealtime = (p->speedLen > 0.0) ? 1000.0 + p->speedAt + p->speedLen + 3.0 : 1.0e12;
	if (p->minScale > 0.0f) {
		sv_rehlds_movecmdtime_min_scale.value = p->minScale;
	}
	realtime = 1000.0;
	g_UserCmdTimeLimiter.ClientConnected(0);

	movesim_packet_t held[1024];
	int numHeld = 0;
	movesim_packet_t pkt;
	byte prev[2] = { 0, 0 };
	unsigned int lcg = 12345;
	double frameSec = 1.0 / p->fps;
	double msecCarry = 0.0;
	int seq = 0;
	int lastSeq = 0;
	int stuckCmds = 0;
	int stuckPackets = 0;
	bool inLevelChange = false;
	bool inHitch = false;

	Q_memset(&pkt, 0, sizeof(pkt));

	for (double t = frameSec; t < p->seconds; t += frameSec)
	{
		if (p->activateAt >= 0.0) {
			bool on = t >= p->activateAt
				&& !(p->deactivateAt >= 0.0 && t >= p->deactivateAt && !(p->reactivateAt >= 0.0 && t >= p->reactivateAt));
			sv_rehlds_movecmdtime_debug.value = on ? 1.0f : 0.0f;
		}

		if (p->levelChangeAt >= 0.0 && t >= p->levelChangeAt && t < p->levelChangeAt + p->levelChangeLen)
		{
			if (!inLevelChange) {
				inLevelChange = true;
				s_simClients[0].active = FALSE;
				realtime = 1000.0 + ceil(t * 1000.0) / 1000.0;
				g_UserCmdTimeLimiter.Frame();
				pkt.numCmds = 0;
			}
			continue;
		}

		if (inLevelChange) {
			inLevelChange = false;
			seq += p->signonPackets;
			lastSeq = seq;
			s_simClients[0].active = TRUE;
		}

		if (p->silenceAt >= 0.0 && t >= p->silenceAt && t < p->silenceAt + p->silenceLen) {
			continue;
		}

		if (p->hitchAt >= 0.0 && t >= p->hitchAt && t < p->hitchAt + p->hitchLen) {
			inHitch = true;
			continue;
		}

		if (inHitch) {
			inHitch = false;
			msecCarry += ((p->hitchLen < 0.25) ? p->hitchLen : 0.25) * 1000.0 * p->speed;
		}

		bool stuck = p->stuckAt >= 0.0 && t >= p->stuckAt && t < p->stuckAt + p->stuckLen;
		int msec;
		if (stuck) {
			stuckCmds++;
			msec = (p->stuckTickEvery > 0 && stuckCmds % p->stuckTickEvery == 0) ? 1 : 0;
		} else {
			// Host_FilterTime caps a client frame at 250ms
			double speed = (p->speedLen > 0.0 && (t < p->speedAt || t >= p->speedAt + p->speedLen)) ? 1.0 : p->speed;
			msecCarry += ((frameSec < 0.25) ? frameSec : 0.25) * 1000.0 * speed;
			msec = (int)msecCarry;
			if (msec > 255) msec = 255;
			msecCarry -= msec;
		}
		pkt.msec[pkt.numCmds++] = (byte)msec;

		if (pkt.numCmds < p->cmdsPerPacket) {
			continue;
		}

		seq += pkt.numCmds + p->fakeSkip;
		if (stuck && p->stuckSkipEvery > 0 && ++stuckPackets % p->stuckSkipEvery == 0) {
			seq++;
		}
		pkt.seq = seq;
		pkt.backup[0] = prev[0];
		pkt.backup[1] = prev[1];
		prev[0] = (pkt.numCmds >= 2) ? pkt.msec[pkt.numCmds - 2] : prev[1];
		prev[1] = pkt.msec[pkt.numCmds - 1];

		lcg = lcg * 1103515245 + 12345;
		bool lost = ((lcg >> 16) % 1000) < p->lossPermille
			|| MoveSim_InWindow(t, p->outageEvery, p->outageLen);
		bool frozen = (p->frozenEvery > 0.0)
			? MoveSim_InWindow(t, p->frozenEvery, p->frozenLen)
			: (p->frozenAt >= 0.0 && t >= p->frozenAt && t < p->frozenAt + p->frozenLen);

		if (!lost)
		{
			if (MoveSim_InWindow(t, p->holdEvery, p->holdLen)) {
				if (numHeld < 1024) {
					held[numHeld++] = pkt;
				}
			}
			else {
				// a released hold arrives in a burst: releasePerMs packets
				// per server frame, then the current packet
				for (int i = 0; i < numHeld; i++) {
					MoveSim_Deliver(p, &held[i], t + MoveSim_ReleaseDelay(p, i), &lastSeq, res, frozen);
				}
				MoveSim_Deliver(p, &pkt, t + MoveSim_ReleaseDelay(p, numHeld), &lastSeq, res, frozen);
				numHeld = 0;
			}
		}

		pkt.numCmds = 0;
	}

	g_UserCmdTimeLimiter.GetMeasureInfo(0, &res->info);

	g_psvs.clients = savedClients;
	g_psvs.maxclientslimit = savedLimit;
	sv_player = savedPlayer;
	realtime = savedRealtime;
	g_psv.paused = savedPaused;
	sv_rehlds_movecmd_max_null_streak.value = savedNullStreak;
	sv_rehlds_movecmdtime_min_scale.value = savedMinScale;
	sv_rehlds_movecmdtime_budget_max.value = savedBudgetMax;
	sv_rehlds_movecmdtime_speedhack_budget.value = savedSpeedhackBudget;
	sv_rehlds_movecmdtime_speedhack_warnings.value = savedSpeedhackWarnings;
	sv_rehlds_movecmdtime_slowmo_warnings.value = savedSlowmoWarnings;
	sv_rehlds_movecmdtime_airstuck_warnings.value = savedAirstuckWarnings;
	sv_rehlds_movecmdtime_debug.value = savedDebug;
	sv_rehlds_movecmdtime_speedhack_drop.value = savedSpeedhackDrop;
}

static void MoveSim_Check(const char *what, bool cond, const movesim_result_t *res)
{
	char msg[512];
	Q_snprintf(msg, sizeof(msg), "%s (rate=%.3f win=%.1fs slow=%u speed=%u clamps=%u dropped=%u/%u exclGap=%llums lostEst=%llums null=%u frz=%u air=%u/%llums)",
		what, res->info.rate, res->info.winSec, res->info.detectSlow, res->info.detectSpeed,
		res->info.budgetClamps, res->dropped, res->judged,
		(unsigned long long)res->info.exclGapMs, (unsigned long long)res->info.lostEstMs,
		res->info.nullDrops, res->info.frozenCmds,
		res->info.airstuckCount, (unsigned long long)res->info.airstuckMs);
	CHECK(msg, cond);
}

TEST(MoveCmdTimeHonestClient, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	MoveSim_Run(&p, &res);

	MoveSim_Check("honest client is measured at real speed", res.info.rate > 0.98 && res.info.rate < 1.02, &res);
	MoveSim_Check("honest client is never judged", res.info.detectSlow == 0 && res.info.detectSpeed == 0, &res);
	MoveSim_Check("honest client keeps every command", res.dropped == 0 && res.info.budgetClamps == 0, &res);
}

// fps above cl_cmdrate: every packet carries several commands and its
// sequence jumps over the held-back ones - that is not packet loss (the
// whole window used to be excluded for such clients)
TEST(MoveCmdTimeHighFpsHonest, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.fps = 300;
	MoveSim_Run(&p, &res);

	MoveSim_Check("high fps client is measured at real speed", res.info.rate > 0.98 && res.info.rate < 1.02, &res);
	MoveSim_Check("high fps client is measured over the full window", res.info.winSec >= 50.0, &res);
	MoveSim_Check("held-back commands are not loss", res.info.lostEstMs == 0, &res);
	MoveSim_Check("high fps client is never judged", res.info.detectSlow == 0 && res.info.detectSpeed == 0, &res);
}

TEST(MoveCmdTimeHighFpsSlowmo, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.fps = 300;
	p.speed = 0.4;
	MoveSim_Run(&p, &res);

	MoveSim_Check("high fps slowmo x0.4 is measured", res.info.rate > 0.38 && res.info.rate < 0.42, &res);
	MoveSim_Check("high fps slowmo x0.4 is detected", res.info.detectSlow > 0, &res);
}

TEST(MoveCmdTimeHighFpsSpeedhack, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.fps = 300;
	p.speed = 4.0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("high fps speedhack x4 is clipped by the budget", res.info.budgetClamps > 0, &res);
	MoveSim_Check("high fps speedhack x4 is detected", res.info.detectSpeed > 0, &res);
}

// Short network outages (shorter than gap_reset) lose whole packets: their
// time never reaches the detector, the window used to measure 0.7 here.
TEST(MoveCmdTimeShortOutages, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.outageEvery = 1.0;
	p.outageLen = 0.3;
	MoveSim_Run(&p, &res);

	MoveSim_Check("outages are seen as lost commands", res.info.lostEstMs > 0, &res);
	MoveSim_Check("outages are not slowmo", res.info.detectSlow == 0, &res);
	MoveSim_Check("outages are not air-stuck", res.info.airstuckCount == 0, &res);
	MoveSim_Check("speed between outages is measured and real", res.info.winSec >= 15.0 && res.info.rate > 0.95 && res.info.rate < 1.05, &res);
}

// The 11.10 19:37 episode: heavy random inbound loss for minutes.
TEST(MoveCmdTimeHeavyRandomLoss, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 120.0;
	p.lossPermille = 500;
	MoveSim_Run(&p, &res);

	MoveSim_Check("heavy loss is not slowmo", res.info.detectSlow == 0, &res);
	MoveSim_Check("heavy loss is not air-stuck", res.info.airstuckCount == 0, &res);
	MoveSim_Check("speed under heavy loss is measured and real", res.info.winSec >= 15.0 && res.info.rate > 0.9 && res.info.rate < 1.1, &res);
	MoveSim_Check("no command of a lossy client is dropped", res.dropped == 0, &res);
}

// A held client (fakelag, choke) delivers the held time in a burst spread
// over several frames and trimmed by max_ticks; it used to measure 1.1-1.4.
TEST(MoveCmdTimeHeldBursts, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.holdEvery = 3.0;
	p.holdLen = 0.7;
	p.releasePerMs = 10;
	MoveSim_Run(&p, &res);

	MoveSim_Check("held bursts do not inflate the speed", res.info.rate > 0.95 && res.info.rate < 1.05, &res);
	MoveSim_Check("held bursts are not judged", res.info.detectSlow == 0 && res.info.detectSpeed == 0, &res);
}

TEST(MoveCmdTimeAfkReturn, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 110.0;
	p.silenceAt = 20.0;
	p.silenceLen = 30.0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("AFK silence is excluded", res.info.exclGapMs >= 29000, &res);
	MoveSim_Check("AFK return is measured at real speed", res.info.rate > 0.98 && res.info.rate < 1.02, &res);
	MoveSim_Check("AFK is not slowmo", res.info.detectSlow == 0, &res);
	MoveSim_Check("AFK is not air-stuck", res.info.airstuckCount == 0, &res);
}

// signon packets after a level change carry no moves: not command loss
TEST(MoveCmdTimeLevelChange, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 80.0;
	p.levelChangeAt = 30.0;
	p.levelChangeLen = 5.0;
	p.signonPackets = 50;
	MoveSim_Run(&p, &res);

	MoveSim_Check("signon packets are not counted as lost", res.info.lostEstMs == 0, &res);
	MoveSim_Check("level change is not slowmo", res.info.detectSlow == 0, &res);
}

// A freeze plugin sets FL_FROZEN, the engine zeroes the msec of the
// player's commands while the client keeps sending them. Those zeros are
// the server's, not an air-stuck client's: they are no null streak (the
// commands run, as in the stock engine) and no slowdown.
TEST(MoveCmdTimeFrozenByServer, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.frozenAt = 20.0;
	p.frozenLen = 30.0;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("server-zeroed commands are counted as frozen", res.info.frozenCmds > 0, &res);
	MoveSim_Check("server-zeroed commands are no null streak", res.info.nullDrops == 0 && res.dropped == 0, &res);
	MoveSim_Check("a server freeze is not slowmo", res.info.detectSlow == 0, &res);
	MoveSim_Check("a server freeze is not air-stuck", res.info.airstuckCount == 0, &res);
	MoveSim_Check("speed after the freeze is real", res.info.rate > 0.98 && res.info.rate < 1.02, &res);
}

// air-stuck: commands keep coming with no game time in them. The null
// streak limit drops them, the hang must still show in the speed window.
TEST(MoveCmdTimeAirstuck, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.stuckAt = 40.0;
	p.stuckLen = 6.0;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("air-stuck is detected once", res.info.airstuckCount == 1, &res);
	MoveSim_Check("air-stuck duration is measured", res.info.airstuckMs >= 4500 && res.info.airstuckMs <= 6500, &res);
	MoveSim_Check("the hang is visible to the speed window", res.info.winSec >= 50.0 && res.info.rate < 0.95, &res);
	MoveSim_Check("zero-time commands past the streak limit are dropped", res.info.nullDrops > 0, &res);
}

TEST(MoveCmdTimeAirstuckTicks, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.stuckAt = 40.0;
	p.stuckLen = 6.0;
	p.stuckTickEvery = 20;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("air-stuck with a 1ms tick every 20 commands is detected", res.info.airstuckCount == 1, &res);
}

// faked loss (cl_cmdbackup 0, a sequence number skipped every few packets)
// makes the engine replay lastcmd - with no time in it, the hang goes on
TEST(MoveCmdTimeAirstuckFakeLoss, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.stuckAt = 40.0;
	p.stuckLen = 6.0;
	p.numBackup = 0;
	p.stuckSkipEvery = 5;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("air-stuck under faked loss is detected", res.info.airstuckCount == 1, &res);
}

// lag-switch style: the stuck commands are held back and released in bursts
TEST(MoveCmdTimeAirstuckHeld, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.stuckAt = 40.0;
	p.stuckLen = 8.0;
	p.holdEvery = 0.8;
	p.holdLen = 0.6;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("air-stuck released in held bursts is detected", res.info.airstuckCount >= 1, &res);
}

// over 1000 fps the client carries the fraction of a millisecond over:
// zeros alternate with ones, never a long zero run
TEST(MoveCmdTimeHighFps2000, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.fps = 2000;
	p.cmdsPerPacket = 20;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("2000 fps is not air-stuck", res.info.airstuckCount == 0 && res.info.nullDrops == 0, &res);
	MoveSim_Check("2000 fps is measured at real speed", res.info.rate > 0.98 && res.info.rate < 1.02, &res);
}

// the connection dies right after a zero-time command: no commands, no
// evidence of a hang
TEST(MoveCmdTimeHighFps2000Afk, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.fps = 2000;
	p.cmdsPerPacket = 20;
	p.silenceAt = 20.0;
	p.silenceLen = 5.0;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("a dead connection at 2000 fps is not air-stuck", res.info.airstuckCount == 0, &res);
}

// held bursts at 2000 fps: the burst after the hold carries its time
TEST(MoveCmdTimeHighFps2000Held, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.fps = 2000;
	p.cmdsPerPacket = 20;
	p.holdEvery = 3.0;
	p.holdLen = 0.7;
	p.releasePerMs = 1;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("held bursts at 2000 fps are not air-stuck", res.info.airstuckCount == 0, &res);
	MoveSim_Check("held bursts at 2000 fps are not slowmo", res.info.detectSlow == 0, &res);
}

// a 1.5s client hitch at 2000 fps: the next frame carries 250ms
TEST(MoveCmdTimeHighFps2000Hitch, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.fps = 2000;
	p.cmdsPerPacket = 20;
	p.hitchAt = 20.0;
	p.hitchLen = 1.5;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("a client hitch at 2000 fps is not air-stuck", res.info.airstuckCount == 0, &res);
}

// DDoS: nearly every packet lost, the few that arrive carry little time
TEST(MoveCmdTimeHighFpsHeavyLoss, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.fps = 2000;
	p.cmdsPerPacket = 20;
	p.lossPermille = 900;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("90% loss at 2000 fps is not air-stuck", res.info.airstuckCount == 0, &res);
	MoveSim_Check("90% loss at 2000 fps is not slowmo", res.info.detectSlow == 0, &res);
}

// a plugin freezing for 0.3s every second: the frozen time is the
// server's, not a slowdown
TEST(MoveCmdTimeFrozenPulses, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.frozenEvery = 1.0;
	p.frozenLen = 0.3;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("frequent short freezes are counted as frozen", res.info.frozenCmds > 0, &res);
	MoveSim_Check("frequent short freezes are not slowmo", res.info.detectSlow == 0 && res.info.rate > 0.95 && res.info.rate < 1.05, &res);
	MoveSim_Check("frequent short freezes are not air-stuck", res.info.airstuckCount == 0, &res);
}

// a client running at 3 fps: each frame lasts 333ms but reports the 250ms
// the client caps it at - the lost third is the client's lag, not slowmo
TEST(MoveCmdTimeLowFpsClient, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.fps = 3;
	p.cmdsPerPacket = 1;
	p.minScale = 0.85f;
	MoveSim_Run(&p, &res);

	MoveSim_Check("a 3 fps client is not slowmo", res.info.detectSlow == 0, &res);
}

// a speedhack sending 255ms commands at full fps stays measurable
TEST(MoveCmdTimeSpeedhack255, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 25.5;
	MoveSim_Run(&p, &res);

	MoveSim_Check("speedhack with 255ms commands is detected", res.info.detectSpeed > 0, &res);
}

TEST(MoveCmdTimeSlowmoCheat, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 0.4;
	MoveSim_Run(&p, &res);

	MoveSim_Check("slowmo x0.4 is measured", res.info.rate > 0.38 && res.info.rate < 0.42, &res);
	MoveSim_Check("slowmo x0.4 is detected", res.info.detectSlow > 0, &res);
	MoveSim_Check("slowmo commands are not dropped", res.dropped == 0, &res);
}

// lost-packet exclusion must not hide a real slowdown: the intervals that
// do get measured still show the client's speed
TEST(MoveCmdTimeSlowmoCheatWithLoss, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 90.0;
	p.speed = 0.4;
	p.lossPermille = 300;
	MoveSim_Run(&p, &res);

	MoveSim_Check("slowmo x0.4 under 30% loss is detected", res.info.detectSlow > 0, &res);
	MoveSim_Check("slowmo x0.4 under 30% loss is measured", res.info.rate > 0.35 && res.info.rate < 0.45, &res);
}

TEST(MoveCmdTimeSpeedhack, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 4.0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("speedhack x4 is clipped by the budget", res.info.budgetClamps > 0, &res);
	MoveSim_Check("speedhack x4 is detected", res.info.detectSpeed > 0, &res);
	MoveSim_Check("speedhack x4 commands are not dropped by default", res.dropped == 0, &res);
}

// everything switched off: the detector does nothing at all
TEST(MoveCmdTimeAllOff, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 4.0;
	p.debug = 0;
	p.speedhackBudget = 0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("all off: nothing is dropped, clipped or measured",
		res.dropped == 0 && res.info.budgetClamps == 0 && res.info.winSec == 0.0 && res.info.detectSpeed == 0, &res);
}

// the movement budget alone: clips, judges nothing
TEST(MoveCmdTimeBudgetOnly, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 4.0;
	p.debug = 0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("budget only: the speedhack is clipped, not judged, nothing dropped",
		res.info.budgetClamps > 0 && res.info.detectSpeed == 0 && res.dropped == 0, &res);
}

// air-stuck neither punished nor observed: not tracked
TEST(MoveCmdTimeAirstuckOff, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.stuckAt = 40.0;
	p.stuckLen = 6.0;
	p.nullStreak = 20;
	p.debug = 0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("air-stuck off: not tracked, the null streak limit still works",
		res.info.airstuckCount == 0 && res.info.nullDrops > 0, &res);
}

// observation switched on, off and on again mid-game on a lossy network:
// nothing gathered while off leaks into the measurement
TEST(MoveCmdTimeActivateMidgame, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 120.0;
	p.lossPermille = 150;
	p.activateAt = 30.0;
	p.deactivateAt = 50.0;
	p.reactivateAt = 60.0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("switched on mid-game: measured at real speed", res.info.rate > 0.98 && res.info.rate < 1.02, &res);
	MoveSim_Check("switched on mid-game: no verdict", res.info.detectSlow == 0 && res.info.detectSpeed == 0, &res);
}

// a speedhack measured, the detector switched off for a minute while the
// client plays fair (the budget keeps running), then on again: the old
// window is gone, not judged anew
TEST(MoveCmdTimeReactivateFresh, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 110.0;
	p.speed = 4.0;
	p.speedAt = 0.0;
	p.speedLen = 40.0;
	p.activateAt = 0.0;
	p.deactivateAt = 40.0;
	p.reactivateAt = 100.0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("switched on again: the window starts over", res.info.winSec < 15.0, &res);
}

// the measurement runs for the speedhack switch, slowmo accrues no warning
TEST(MoveCmdTimeVerdictsPerSwitch, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 0.4;
	p.debug = 0;
	p.speedhackWarnings = 3;
	MoveSim_Run(&p, &res);

	MoveSim_Check("slowmo is judged while the speedhack switch is on", res.info.detectSlow > 0, &res);
	MoveSim_Check("slowmo accrues no warning without its own switch", res.info.warnSlow == 0, &res);
}

// warnings accrue only while the cheat's punishment is switched on
TEST(MoveCmdTimeWarningsCount, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 4.0;
	MoveSim_Run(&p, &res);

	MoveSim_Check("a speedhack is judged without its punishment switch", res.info.detectSpeed > 0, &res);
	MoveSim_Check("no warning accrues without the punishment switch", res.info.warnSpeed == 0, &res);
}

// speedhack x8 for 30s, then 40s at real speed: the window average stays
// above max_scale (3.0) long after the cheat is off
TEST(MoveCmdTimeSpeedhackDropModes, MoveCmdTime, 5000)
{
	for (int mode = 0; mode <= 2; mode++)
	{
		movesim_params_t p;
		movesim_result_t res;
		MoveSim_Defaults(&p);
		p.seconds = 100.0;
		p.speed = 8.0;
		p.speedAt = 30.0;
		p.speedLen = 30.0;
		p.speedhackDrop = mode;
		MoveSim_Run(&p, &res);

		MoveSim_Check("speedhack is detected in every drop mode", res.info.detectSpeed > 0, &res);
		if (mode == 0) {
			MoveSim_Check("drop 0: no command is dropped", res.dropped == 0, &res);
		} else if (mode == 1) {
			MoveSim_Check("drop 1: commands are dropped while the speedhack is on", res.dropped > 0, &res);
			MoveSim_Check("drop 1: the client moves again within 3s after it is off", res.droppedAfter == 0, &res);
		} else {
			MoveSim_Check("drop 2: the client stays frozen after the speedhack is off", res.droppedAfter > 0, &res);
		}
	}
}

// a client faking command loss beyond the backups before every packet:
// the server reruns its two backups (time it really plays) and credits the
// rest at the client's own slowed command time - the window shows the speed
// the server ran, x0.4 slowmo doubled to x0.8 by the reruns
TEST(MoveCmdTimeFakedLoss, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 0.4;
	p.fakeSkip = 3;
	MoveSim_Run(&p, &res);

	MoveSim_Check("faked loss is estimated", res.info.lostEstMs > 0, &res);
	MoveSim_Check("faked loss is measured at the speed the server ran", res.info.rate > 0.77 && res.info.rate < 0.83, &res);
}

// an air-stuck held for half a minute accrues a warning per 5 seconds
TEST(MoveCmdTimeAirstuckLong, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.seconds = 80.0;
	p.stuckAt = 20.0;
	p.stuckLen = 36.0;
	p.nullStreak = 20;
	MoveSim_Run(&p, &res);

	MoveSim_Check("a 36s air-stuck accrues 7 warnings", res.info.airstuckCount == 7, &res);
}

// bad networks: holds below and above gap_reset, fast and slow release,
// random loss, with and without backups - an honest client is measured at
// its real speed in every combination
TEST(MoveCmdTimeBadNetworkSweep, MoveCmdTime, 20000)
{
	int cpps[] = { 1, 3 };
	unsigned int losses[] = { 0, 50, 150, 300 };
	double holds[] = { 0.0, 0.2, 0.4, 0.6, 0.9 };
	int releases[] = { 1, 10 };
	int backups[] = { 0, 2 };

	for (int a = 0; a < 2; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 5; c++)
	for (int d = 0; d < 2; d++) for (int e = 0; e < 2; e++)
	{
		movesim_params_t p;
		movesim_result_t res;
		MoveSim_Defaults(&p);
		p.seconds = 90.0;
		p.cmdsPerPacket = cpps[a];
		p.lossPermille = losses[b];
		p.holdEvery = (holds[c] > 0.0) ? 1.5 : 0.0;
		p.holdLen = holds[c];
		p.releasePerMs = releases[d];
		p.numBackup = backups[e];
		MoveSim_Run(&p, &res);

		char what[128];
		Q_snprintf(what, sizeof(what), "honest client on a bad network (cpp=%d loss=%u hold=%.1f release=%d backups=%d)",
			cpps[a], losses[b], holds[c], releases[d], backups[e]);
		MoveSim_Check(what, res.info.rate == 0.0 || (res.info.rate > 0.93 && res.info.rate < 1.07), &res);
		MoveSim_Check(what, res.info.detectSlow == 0 && res.info.detectSpeed == 0, &res);
	}
}

// a congested link: holds of 0.6-3s drained slowly (one packet per 1-20ms,
// still faster than the client sends) - the backlog is awaited as long as
// it keeps catching up, an honest client is measured at its real speed
TEST(MoveCmdTimeSlowDrainSweep, MoveCmdTime, 20000)
{
	int cpps[] = { 1, 3 };
	double holds[] = { 0.6, 1.0, 2.0, 3.0 };
	double intervals[] = { 1.0, 3.0, 5.0, 10.0, 20.0 };
	unsigned int losses[] = { 0, 150 };

	for (int a = 0; a < 2; a++) for (int c = 0; c < 4; c++) for (int d = 0; d < 5; d++) for (int b = 0; b < 2; b++)
	{
		// the link must drain faster than the client sends, or the queue
		// only grows
		if (intervals[d] >= 10.0 * cpps[a]) {
			continue;
		}

		movesim_params_t p;
		movesim_result_t res;
		MoveSim_Defaults(&p);
		p.seconds = 120.0;
		p.cmdsPerPacket = cpps[a];
		p.lossPermille = losses[b];
		p.holdEvery = holds[c] + 3.0;
		p.holdLen = holds[c];
		p.releaseIntervalMs = intervals[d];
		MoveSim_Run(&p, &res);

		char what[128];
		Q_snprintf(what, sizeof(what), "honest client on a congested link (cpp=%d hold=%.1f drain=%.0fms loss=%u)",
			cpps[a], holds[c], intervals[d], losses[b]);
		MoveSim_Check(what, res.info.rate == 0.0 || (res.info.rate > 0.93 && res.info.rate < 1.07), &res);
		MoveSim_Check(what, res.info.detectSlow == 0 && res.info.detectSpeed == 0, &res);
	}
}

#endif // REHLDS_FIXES
