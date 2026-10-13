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

	// AFK: the client generates no commands at all
	double silenceAt;
	double silenceLen;

	// level change: the client is inactive, its signon packets carry no
	// moves but take sequence numbers the server's netchan does see
	double levelChangeAt;
	double levelChangeLen;
	int signonPackets;
};

struct movesim_result_t
{
	CUserCmdTimeLimiter::measure_info_t info;
	unsigned int judged;  // commands that reached CheckLimits
	unsigned int dropped; // commands CheckLimits refused
};

struct movesim_packet_t
{
	int seq;
	int numCmds;
	byte msec[32];
	byte backup[2]; // [0] older, [1] newer
};

static client_t s_simClients[MAX_CLIENTS];
static edict_t s_simPlayer;

static void MoveSim_Defaults(movesim_params_t *p)
{
	Q_memset(p, 0, sizeof(*p));
	p->seconds = 60.0;
	p->fps = 100;
	p->cmdsPerPacket = 3;
	p->speed = 1.0;
	p->releasePerMs = 10;
	p->silenceAt = -1.0;
	p->levelChangeAt = -1.0;
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
	}
}

static void MoveSim_Deliver(const movesim_packet_t *pkt, double atSec, int *lastSeq, movesim_result_t *res)
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
	g_UserCmdTimeLimiter.OnMoveParsed(0, netDrop, 2);

	if (netDrop > 0 && netDrop < 24) {
		int backups = (netDrop < 2) ? netDrop : 2;
		for (int i = 2 - backups; i < 2; i++) {
			MoveSim_RunCmd(pkt->backup[i], res);
		}
	}

	for (int i = 0; i < pkt->numCmds; i++) {
		MoveSim_RunCmd(pkt->msec[i], res);
	}
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

	Q_memset(res, 0, sizeof(*res));
	Q_memset(s_simClients, 0, sizeof(s_simClients));
	Q_memset(&s_simPlayer, 0, sizeof(s_simPlayer));
	s_simClients[0].active = TRUE;
	s_simClients[0].connected = TRUE;
	g_psvs.clients = s_simClients;
	g_psvs.maxclientslimit = MAX_CLIENTS;
	sv_player = &s_simPlayer;
	g_psv.paused = FALSE;
	realtime = 1000.0;
	g_UserCmdTimeLimiter.ClientConnected(0);

	movesim_packet_t held[256];
	int numHeld = 0;
	movesim_packet_t pkt;
	byte prev[2] = { 0, 0 };
	unsigned int lcg = 12345;
	double frameSec = 1.0 / p->fps;
	double msecCarry = 0.0;
	int seq = 0;
	int lastSeq = 0;
	bool inLevelChange = false;

	Q_memset(&pkt, 0, sizeof(pkt));

	for (double t = frameSec; t < p->seconds; t += frameSec)
	{
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

		msecCarry += frameSec * 1000.0 * p->speed;
		int msec = (int)msecCarry;
		if (msec > 255) msec = 255;
		msecCarry -= msec;
		pkt.msec[pkt.numCmds++] = (byte)msec;

		if (pkt.numCmds < p->cmdsPerPacket) {
			continue;
		}

		seq += pkt.numCmds + p->fakeSkip;
		pkt.seq = seq;
		pkt.backup[0] = prev[0];
		pkt.backup[1] = prev[1];
		prev[0] = (pkt.numCmds >= 2) ? pkt.msec[pkt.numCmds - 2] : prev[1];
		prev[1] = pkt.msec[pkt.numCmds - 1];

		lcg = lcg * 1103515245 + 12345;
		bool lost = ((lcg >> 16) % 1000) < p->lossPermille
			|| MoveSim_InWindow(t, p->outageEvery, p->outageLen);

		if (!lost)
		{
			if (MoveSim_InWindow(t, p->holdEvery, p->holdLen)) {
				if (numHeld < 256) {
					held[numHeld++] = pkt;
				}
			}
			else {
				// a released hold arrives in a burst: releasePerMs packets
				// per server frame, then the current packet
				for (int i = 0; i < numHeld; i++) {
					MoveSim_Deliver(&held[i], t + (i / p->releasePerMs) * 0.001, &lastSeq, res);
				}
				MoveSim_Deliver(&pkt, t + (numHeld / p->releasePerMs) * 0.001, &lastSeq, res);
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
}

static void MoveSim_Check(const char *what, bool cond, const movesim_result_t *res)
{
	char msg[512];
	Q_snprintf(msg, sizeof(msg), "%s (rate=%.3f win=%.1fs slow=%u speed=%u clamps=%u dropped=%u/%u exclGap=%llums exclLoss=%llums)",
		what, res->info.rate, res->info.winSec, res->info.detectSlow, res->info.detectSpeed,
		res->info.budgetClamps, res->dropped, res->judged,
		(unsigned long long)res->info.exclGapMs, (unsigned long long)res->info.exclLossMs);
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
	MoveSim_Check("held-back commands are not loss", res.info.exclLossMs == 0, &res);
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

	MoveSim_Check("outages are seen as lost commands", res.info.exclLossMs > 0, &res);
	MoveSim_Check("outages are not slowmo", res.info.detectSlow == 0, &res);
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

	MoveSim_Check("signon packets are not counted as lost", res.info.exclLossMs == 0, &res);
	MoveSim_Check("level change is not slowmo", res.info.detectSlow == 0, &res);
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
	MoveSim_Check("speedhack x4 commands are dropped while detected", res.dropped > 0, &res);
}

// Known limitation: a client faking command loss beyond the backups before
// every packet keeps its whole session out of the measurement. It cannot
// hide that: the time shows up as excluded for loss (LOSS-HEAVY in the
// telemetry).
TEST(MoveCmdTimeFakedLoss, MoveCmdTime, 5000)
{
	movesim_params_t p;
	movesim_result_t res;
	MoveSim_Defaults(&p);
	p.speed = 0.4;
	p.fakeSkip = 3;
	MoveSim_Run(&p, &res);

	MoveSim_Check("faked loss is visible as excluded time", res.info.exclLossMs >= 50000, &res);
	MoveSim_Check("faked loss leaves nothing to judge", res.info.detectSlow == 0, &res);
}
