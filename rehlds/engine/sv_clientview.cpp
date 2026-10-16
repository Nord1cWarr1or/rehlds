/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*/

#include "precompiled.h"
#include "sv_clientview.h"

#ifdef REHLDS_FIXES

// Client view emulation (sv_unlag_clientview 1).
//
// For every lag-compensated client the server replays, one client frame per
// usercmd, what that client's renderer drew for the other players: the origin
// CL_ComputePlayerOrigin interpolates at cl.time - ex_interp over the
// animtime-keyed position history, and the Counter-Strike player animation
// state (gait yaw, gait frame, yaw/pitch blends) that CGameStudioModelRenderer
// keeps per player. A lag-compensated trace of that client then hits bones
// built from this state instead of the server's own player animation.
//
// The client rules follow the stock 8684 hw.so/client.so, including where the
// client clock stood at each frame (CL_Parse_Time/CL_AdjustClock, replayed
// from the frame times of the commands). Assumptions: the client draws every
// player present in its snapshots (the CS renderer runs the gait before its
// frustum check), it renders the model the server uses (no cl_minmodels),
// it keeps the default cl_fixtimerate/cl_clockreset, and the emulation saw
// the connection from its start: the client starts the gait state of every
// player zeroed on connect and map load, so state from before the first
// emulated frame (enabling the cvar mid-map) stays off for standing players
// until the next map or reconnect.

cvar_t sv_unlag_clientview = { "sv_unlag_clientview", "0", 0, 0.0f, NULL };

namespace {

const int CV_QUERY_ID = 0x7E580000;		// low bits: client index
const double CV_QUERY_PERIOD = 5.0;
const double CV_REQUERY_DELAY = 1.0;	// earliest new query once lerp_msec has moved
const float CV_INTERP_TOLERANCE = 0.0015f;	// how far a queried ex_interp may be from lerp_msec
const int CV_HISTORY_DEPTH = 24;		// snapshots walked back for the interpolation pair

// CS player model sequences the client treats specially
const int ANIM_WALK_SEQUENCE = 3;
const int ANIM_JUMP_SEQUENCE = 6;
const int ANIM_SWIM_1 = 8;
const int ANIM_SWIM_2 = 9;
const int ANIM_FIRST_DEATH_SEQUENCE = 101;
const int ANIM_LAST_DEATH_SEQUENCE = 159;
const int ANIM_FIRST_EMOTION_SEQUENCE = 198;
const int ANIM_LAST_EMOTION_SEQUENCE = 207;


struct cv_observer_t
{
	int userid;
	int spawncount;
	double svtime[MULTIPLAYER_BACKUP];	// sv.time of each snapshot, indexed like client_t::frames
	unsigned char slot[MULTIPLAYER_BACKUP][MAX_CLIENTS + 1];	// player number -> index in that snapshot's packet, 0xFF when absent
	double timebase;					// local gametime base of the TIMEWINDOW_8 grid
	float exinterp;						// queried ex_interp, 0 until known
	double querytime;
	int querylerp;						// lerp_msec when the last query went out
	double lastcltime, lasttarget;		// the last replayed render, for sv_unlag_trace
	float lastinterp;
	cv_clock_t clock;
	double elapsed;						// client clock past the acked snapshot, newest command of the packet
	int packetack, prevpacketack;		// acks of this usercmd packet and of the previous one
	int prevmsec;
	cv_victim_t victim[MAX_CLIENTS];
};

cv_observer_t g_clientView[MAX_CLIENTS];

} // namespace

static cv_observer_t *CV_Observer(client_t *cl)
{
	cv_observer_t *cv = &g_clientView[cl - g_psvs.clients];

	if (cv->userid != cl->userid || cv->spawncount != g_psvs.spawncount)
	{
		Q_memset(cv, 0, sizeof(*cv));
		cv->userid = cl->userid;
		cv->spawncount = g_psvs.spawncount;
	}

	return cv;
}

// animtime the way the client decodes DT_TIMEWINDOW_8: whole centiseconds of
// the snapshot time base
double CV_ClientAnimtime(float animtime, double timebase)
{
	return timebase + (int)((animtime - timebase) * 100.0) / 100.0;
}

// what the client gets after the player delta encoding (delta.lst
// entity_state_player_t): origin 24 bits x32, angles 16 bit, frame 8 bits
float CV_WireCoord(float v)
{
	return (int)((double)v * 32.0) / 32.0f;
}

// fmod(a, 360) for the values entity angles take, without the x87 fprem loop
static inline double CV_AngleMod(double a)
{
	return (a > -360.0 && a < 360.0) ? a : fmod(a, 360.0);
}

// the client normalizes the decoded [0, 360) angles (COM_NormalizeAngles)
float CV_WireAngle(float a)
{
	int d = (int)(65536.0 * CV_AngleMod(a)) / 360;
	float decoded = (d & 0xFFFF) * (360.0f / 65536.0f);
	return (decoded > 180.0f) ? decoded - 360.0f : decoded;
}

float CV_WireFrame(float f)
{
	return (float)((unsigned int)(double)f & 0xFF);
}

// The renderer interpolates by the ex_interp cvar itself. lerp_msec is built
// from it (CL_ComputeClientInterpolationAmount) but truncated to whole ms
// twice, raised to 1000/cl_updaterate and drifting 50 ms/s behind a change,
// so it is about a millisecond off (ex_interp 0.01 goes out as 9). A queried
// value only refines lerp_msec within that error: both come from the client,
// and a reply can never move the rewind further than lerp_msec already does.
float CV_ClientInterp(int lerpmsec, float exinterp)
{
	float lerp = lerpmsec / 1000.0f;
	if (lerp > 0.1f)
		lerp = 0.1f;
	else if (lerp < 0.0f)
		lerp = 0.0f;

	if (exinterp > 0.0f && fabs(exinterp - lerp) <= CV_INTERP_TOLERANCE)
		return exinterp;

	return lerp;
}

const double CV_FIXTIMERATE = 7.5;		// cl_fixtimerate default, ms per frame
const double CV_CLOCKRESET = 0.1;		// cl_clockreset default

// CL_AdjustClock, once per client frame after the render: move the clock
// toward the snapshot time by the whole milliseconds of the offset, at most
// cl_fixtimerate per frame; under a millisecond is left alone
double CV_AdjustClock(double clock, double *delta)
{
	if (*delta == 0.0 || fabs(*delta) < 0.001)
		return clock;

	int msec = (int)(*delta * 1000.0);
	double adjust = abs(msec);
	if (adjust > CV_FIXTIMERATE)
		adjust = CV_FIXTIMERATE;

	adjust = (msec < 0 ? adjust : -adjust) / 1000.0;
	if (fabs(adjust) < fabs(*delta))
	{
		*delta += adjust;
		clock += adjust;
	}

	return clock;
}

// The client advances its clock by the frame time, reads the snapshots that
// arrived (CL_Parse_Time measures the clock against the newest one, or resets
// it when off by more than cl_clockreset), renders, and only then adjusts the
// clock (CL_AdjustClock). So the render of a frame that read a new snapshot
// still runs on the clock of the previous frame.
//
// A packet with one command follows the previous one by one frame, whose
// reads its ack tells exactly: the client clock is replayed. A packet with
// more commands read its ack somewhere in their frames, so its newest command
// was rendered 0 .. (msec of the older commands) after reading it, or later
// still with the same ack as the previous packet; inside that window the
// round trip says where: the packets sent right after reading their ack have
// the shortest one, the others waited the difference. The newest command's
// own frame is not part of the wait (fitted on stock 8684 clients at 100 and
// 500 fps, see sv_unlag_trace).
double CV_ClockElapsed(cv_clock_t *c, int ack, double acktime, double rtt, int newestmsec, int oldermsec)
{
	qboolean moved = (!c->valid || ack != c->prevack) ? TRUE : FALSE;
	qboolean replayed = (c->valid && oldermsec == 0) ? TRUE : FALSE;
	double elapsed = 0.0, lo, hi;

	if (replayed)
	{
		double render = c->clock + c->prevnewestmsec / 1000.0;
		elapsed = lo = hi = render - acktime;

		if (moved)
		{
			c->delta = render - acktime;
			if (fabs(c->delta) > CV_CLOCKRESET)
			{
				render = acktime;
				c->delta = 0.0;
			}
		}

		c->clock = CV_AdjustClock(render, &c->delta);
	}
	else if (!moved)
	{
		double since = (c->prevnewestmsec + oldermsec) / 1000.0;
		lo = c->lo + since;
		hi = c->hi + since;
	}
	else
	{
		lo = 0.0;
		hi = oldermsec / 1000.0;
	}

	c->base[c->pos] = (float)(rtt - lo);
	c->pos = (c->pos + 1) % CV_RTT_WINDOW;
	if (c->count < CV_RTT_WINDOW)
		c->count++;

	if (!replayed)
	{
		float base = c->base[0];
		for (int i = 1; i < c->count; i++)
		{
			if (c->base[i] < base)
				base = c->base[i];
		}

		elapsed = rtt - base - newestmsec / 1000.0;
		if (elapsed < lo)
			elapsed = lo;
		else if (elapsed > hi)
			elapsed = hi;

		// the read frame is unknown: carry on with a synced clock
		c->clock = acktime + elapsed;
		c->delta = 0.0;
	}

	// the next packet with the same ack counts on from the clock this frame ended with
	c->lo = replayed ? c->clock - acktime : lo;
	c->hi = replayed ? c->clock - acktime : hi;
	c->valid = TRUE;
	c->prevack = ack;
	c->prevnewestmsec = newestmsec;
	return elapsed;
}

// Whether the client had snapshot seq (built at svtime) at the replayed
// render. The newest command of a packet was built after the frame that read
// everything up to its ack, an older one at least had what the previous
// packet acked; the snapshots in between, and the ones a moved view sees
// past the ack (movedview: sv_unlagpush/sv_maxunlag), had arrived once the
// client clock reached them. The frame
// that reads a snapshot renders before syncing its clock, so the clock alone
// would miss it.
qboolean CV_SnapshotKnown(int seq, double svtime, int ack, int prevack, double cmdage, double cltime, double frametime, qboolean movedview)
{
	// the replayed clock of an older command is off by about a frame: half a
	// frame splits the misses (fitted on stock clients at 100, 300 and 500 fps)
	double tolerance = 0.0005 + 0.5 * frametime;

	if (seq > ack)
		return (movedview && svtime <= cltime + tolerance) ? TRUE : FALSE;

	if (cmdage <= 0.0 || seq <= prevack)
		return TRUE;

	return (svtime <= cltime + tolerance) ? TRUE : FALSE;
}

// Client clock of the replayed render and the time it interpolated players
// at, both on the sv.time scale. clock is the client clock at the render of
// the newest command of the packet, cmdage how much earlier the command was
// built. sv_maxunlag and sv_unlagpush move the view the way they move the
// stock rewind target: back no further than maxunlag, push seconds later,
// a push never past the newest snapshot sent (the client's own clock may
// run past it).
void CV_ViewTimes(double clock, double cmdage, double now, double newest, float interp, float maxunlag, float push, double *cltime, double *target)
{
	double c = clock - cmdage;
	if (maxunlag > 0.0f && now - c > maxunlag)
		c = now - maxunlag;

	double t = c - interp;
	if (push > 0.0f)
	{
		double limit = (c > newest) ? c : newest;
		c = (c + push > limit) ? limit : c + push;
		t = (t + push > limit) ? limit : t + push;
	}
	else
	{
		c += push;
		t += push;
	}

	*cltime = c;
	*target = t;
}

static entity_state_t *CV_FindState(cv_observer_t *cv, client_frame_t *frame, int index, int number)
{
	int i = cv->slot[index][number];
	if (i == 0xFF || i >= frame->entities.num_entities)
		return NULL;

	entity_state_t *s = &frame->entities.entities[i];
	return (s->number == number) ? s : NULL;
}

// CL_PureOrigin: lerp between the two history entries around t, players are
// allowed to extrapolate up to 20%
void CV_PureOrigin(const double *at, entity_state_t **st, int count, double t, vec_t *origin, vec_t *angles)
{
	vec3_t org[2], ang[2];

	// entries are newest first
	int i0 = 0, i1 = 1;
	if (t <= at[0])
	{
		for (int i = 1; i < count; i++)
		{
			if (t > at[i])
			{
				i0 = i - 1;
				i1 = i;
				break;
			}
		}
	}

	if (count < 2)
		i1 = i0;

	for (int k = 0; k < 3; k++)
	{
		org[0][k] = CV_WireCoord(st[i0]->origin[k]);
		org[1][k] = CV_WireCoord(st[i1]->origin[k]);
		ang[0][k] = CV_WireAngle(st[i0]->angles[k]);
		ang[1][k] = CV_WireAngle(st[i1]->angles[k]);
	}

	double t0 = at[i0], t1 = at[i1];
	float frac = (t0 != t1) ? (float)((t - t1) / (t0 - t1)) : 1.0f;
	if (frac < 0.0f)
		frac = 0.0f;
	else if (frac > 1.2f)
		frac = 1.2f;

	for (int k = 0; k < 3; k++)
	{
		origin[k] = org[1][k] + frac * (org[0][k] - org[1][k]);

		float d = ang[0][k] - ang[1][k];
		if (d > 180.0f)
			d -= 360.0f;
		else if (d < -180.0f)
			d += 360.0f;
		angles[k] = ang[1][k] + d * frac;
	}
}

// CGameStudioModelRenderer::StudioEstimateGait + CalculateYawBlend +
// CalculatePitchBlend + StudioProcessGait for one drawn frame
void CV_ProcessGait(cv_victim_t *v, studiohdr_t *phdr, float dt)
{
	if (dt < 0.0f)
		dt = 0.0f;
	else if (dt > 1.0f)
		dt = 1.0f;

	qboolean restartjump = (v->gaitsequence == ANIM_JUMP_SEQUENCE && v->lastgaitsequence != ANIM_JUMP_SEQUENCE);
	v->lastgaitsequence = v->gaitsequence;

	float movement = 0.0f;
	if (dt != 0.0f)
	{
		vec3_t est;
		VectorSubtract(v->origin, v->prevgaitorigin, est);
		VectorCopy(v->origin, v->prevgaitorigin);
		movement = Length(est);

		if (dt <= 0.0f || movement / dt < 5.0f)
		{
			movement = 0.0f;
			est[0] = est[1] = 0.0f;
		}

		float flYaw = v->angles[YAW] - v->gaityaw;
		if (v->sequence > 100)
		{
			v->gaityaw += flYaw;
		}
		else if (est[0] == 0.0f && est[1] == 0.0f)
		{
			float flYawDiff = flYaw - (int)(flYaw / 360.0f) * 360.0f;
			if (flYawDiff > 180.0f)
				flYawDiff -= 360.0f;
			if (flYawDiff < -180.0f)
				flYawDiff += 360.0f;

			flYaw = (float)CV_AngleMod(flYaw);
			if (flYaw < -180.0f)
				flYaw += 360.0f;
			else if (flYaw > 180.0f)
				flYaw -= 360.0f;

			if (flYaw > -5.0f && flYaw < 5.0f)
				v->yawmodifier = 0.05f;
			if (flYaw < -90.0f || flYaw > 90.0f)
				v->yawmodifier = 3.5f;

			if (dt < 0.25f)
				flYawDiff *= dt * v->yawmodifier;
			else
				flYawDiff *= dt;

			// the client compares the truncated step: under one degree it is dropped
			if (abs((int)flYawDiff) < 0.1f)
				flYawDiff = 0.0f;

			v->gaityaw += flYawDiff;
			v->gaityaw -= (int)(v->gaityaw / 360.0f) * 360.0f;
			movement = 0.0f;
		}
		else
		{
			v->gaityaw = (float)(atan2(est[1], est[0]) * 180.0 / M_PI);
			if (v->gaityaw > 180.0f)
				v->gaityaw = 180.0f;
			if (v->gaityaw < -180.0f)
				v->gaityaw = -180.0f;
		}
	}

	// CalculateYawBlend
	float flYaw = (float)CV_AngleMod(v->angles[YAW] - v->gaityaw);
	if (flYaw < -180.0f)
		flYaw += 360.0f;
	else if (flYaw > 180.0f)
		flYaw -= 360.0f;

	if (movement != 0.0f)
	{
		if (flYaw > 120.0f)
		{
			v->gaityaw -= 180.0f;
			movement = -movement;
			flYaw -= 180.0f;
		}
		else if (flYaw < -120.0f)
		{
			v->gaityaw += 180.0f;
			movement = -movement;
			flYaw += 180.0f;
		}
	}

	float blendyaw = (flYaw / 90.0f) * 128.0f + 127.0f;
	if (blendyaw > 255.0f)
		blendyaw = 255.0f;
	else if (blendyaw < 0.0f)
		blendyaw = 0.0f;
	v->blending[0] = (unsigned char)(int)(255.0f - blendyaw);

	// CalculatePitchBlend (StudioPlayerBlend of the CS client)
	int iBlend = (int)(v->angles[PITCH] * 3.0f);
	if (iBlend <= -45)
		iBlend = 255;
	else if (iBlend >= 45)
		iBlend = 0;
	else
		iBlend = 255 * (45 - iBlend) / 90;
	v->blending[1] = (unsigned char)iBlend;

	// gait frame
	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)phdr + phdr->seqindex) + v->gaitsequence;
	if (pseqdesc->linearmovement[0] > 0.0f)
		v->gaitframe += (movement / pseqdesc->linearmovement[0]) * pseqdesc->numframes;
	else
		v->gaitframe += pseqdesc->fps * dt * v->framerate;

	v->gaitframe -= (int)(v->gaitframe / pseqdesc->numframes) * pseqdesc->numframes;
	if (v->gaitframe < 0.0f)
		v->gaitframe += pseqdesc->numframes;

	// the client starts the jump gait over (after this frame's advance)
	if (restartjump)
		v->gaitframe = 0.0f;
}

// CStudioModelRenderer::StudioEstimateFrame
double CV_EstimateFrame(const cv_victim_t *v, mstudioseqdesc_t *pseqdesc)
{
	double dfdt = (v->cltime >= v->animtime) ? (v->cltime - v->animtime) * v->framerate * pseqdesc->fps : 0.0;
	double f = (pseqdesc->numframes > 1) ? (v->frame * (pseqdesc->numframes - 1)) / 256.0 : 0.0;
	f += dfdt;

	if (pseqdesc->flags & STUDIO_LOOPING)
	{
		if (pseqdesc->numframes > 1)
			f -= (int)(f / (pseqdesc->numframes - 1)) * (pseqdesc->numframes - 1);
		if (f < 0.0)
			f += (pseqdesc->numframes - 1);
	}
	else
	{
		if (f >= pseqdesc->numframes - 1.001)
			f = pseqdesc->numframes - 1.001;
		if (f < 0.0)
			f = 0.0;
	}

	return f;
}

static void CV_CalcRotations(mstudiobone_t *pbones, int numbones, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, const unsigned char *controller, float f, float pos[][3], vec4_t *q)
{
	if (f > pseqdesc->numframes - 1)
		f = 0.0f;
	else if (f < -0.01f)
		f = -0.01f;

	int frame = (int)f;
	float s = f - frame;

	float adj[MAXSTUDIOCONTROLLERS];
	R_StudioCalcBoneAdj(1.0f, adj, controller, controller, 0);

	for (int i = 0; i < numbones; i++)
	{
		R_StudioCalcBoneQuaterion(frame, s, &pbones[i], &panim[i], adj, q[i]);
		R_StudioCalcBonePosition(frame, s, &pbones[i], &panim[i], adj, pos[i]);
	}

	if (pseqdesc->motiontype & STUDIO_X)
		pos[pseqdesc->motionbone][0] = 0.0f;
	if (pseqdesc->motiontype & STUDIO_Y)
		pos[pseqdesc->motionbone][1] = 0.0f;
	if (pseqdesc->motiontype & STUDIO_Z)
		pos[pseqdesc->motionbone][2] = 0.0f;
}

// the nine-way aim blend of CGameStudioModelRenderer::StudioSetupBones
static void CV_CalcPlayerSequence(mstudiobone_t *pbones, int numbones, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, const unsigned char *controller, const unsigned char *blending, float f, float pos[][3], vec4_t *q)
{
	static float pos2[MAXSTUDIOBONES][3], pos3[MAXSTUDIOBONES][3], pos4[MAXSTUDIOBONES][3];
	static vec4_t q2[MAXSTUDIOBONES], q3[MAXSTUDIOBONES], q4[MAXSTUDIOBONES];

	if (pseqdesc->numblends != 9)
	{
		CV_CalcRotations(pbones, numbones, pseqdesc, panim, controller, f, pos, q);
		return;
	}

	static const int quadrant[4][4] = { { 0, 1, 3, 4 }, { 3, 4, 6, 7 }, { 1, 2, 4, 5 }, { 4, 5, 7, 8 } };

	float s = blending[0];
	float t = blending[1];
	int quad;

	if (s <= 127.0f)
	{
		s = s * 2.0f;
		if (t <= 127.0f)
		{
			t = t * 2.0f;
			quad = 0;
		}
		else
		{
			t = 2.0f * (t - 127.0f);
			quad = 1;
		}
	}
	else
	{
		s = 2.0f * (s - 127.0f);
		if (t <= 127.0f)
		{
			t = t * 2.0f;
			quad = 2;
		}
		else
		{
			t = 2.0f * (t - 127.0f);
			quad = 3;
		}
	}

	CV_CalcRotations(pbones, numbones, pseqdesc, panim + quadrant[quad][0] * numbones, controller, f, pos, q);
	CV_CalcRotations(pbones, numbones, pseqdesc, panim + quadrant[quad][1] * numbones, controller, f, pos2, q2);
	CV_CalcRotations(pbones, numbones, pseqdesc, panim + quadrant[quad][2] * numbones, controller, f, pos3, q3);
	CV_CalcRotations(pbones, numbones, pseqdesc, panim + quadrant[quad][3] * numbones, controller, f, pos4, q4);

	s /= 255.0f;
	t /= 255.0f;

	R_StudioSlerpBones(q, pos, q2, pos2, s);
	R_StudioSlerpBones(q3, pos3, q4, pos4, s);
	R_StudioSlerpBones(q, pos, q3, pos3, t);
}

void SV_ClientViewRegister()
{
	Cvar_RegisterVariable(&sv_unlag_clientview);
}

qboolean SV_ClientViewActive(client_t *cl)
{
	if (sv_unlag_clientview.value == 0.0f || sv_unlag.value == 0.0f || cl->fakeclient || !SV_UnlagForClient(cl))
		return FALSE;

	return (g_eGameType == GT_CStrike || g_eGameType == GT_CZero) ? TRUE : FALSE;
}

void SV_ClientViewSnapshot(client_t *cl, client_frame_t *frame)
{
	if (sv_unlag_clientview.value == 0.0f || cl->fakeclient)
		return;

	cv_observer_t *cv = CV_Observer(cl);
	int index = (frame - cl->frames) & SV_UPDATE_MASK;
	cv->svtime[index] = g_psv.time;
	cv->timebase = (sv_rehlds_local_gametime.value != 0.0f) ? g_GameClients[cl - g_psvs.clients]->GetLocalGameTimeBase() : 0.0;

	Q_memset(cv->slot[index], 0xFF, sizeof(cv->slot[index]));
	for (int i = 0; i < frame->entities.num_entities && i < 0xFF; i++)
	{
		// players are packed first, in entity order
		int number = frame->entities.entities[i].number;
		if (number > g_psvs.maxclients)
			break;

		if (number > 0)
			cv->slot[index][number] = (unsigned char)i;
	}
}

void SV_ClientViewCvarValue(client_t *cl, int requestID, const char *value)
{
	if ((requestID & ~0xFF) != CV_QUERY_ID)
		return;

	cv_observer_t *cv = CV_Observer(cl);
	float interp = Q_atof(value);
	cv->exinterp = (interp > 0.0f && interp <= 0.5f) ? interp : 0.0f;
}

qboolean SV_ClientViewIsQuery(int requestID)
{
	return ((requestID & ~0xFF) == CV_QUERY_ID) ? TRUE : FALSE;
}

// realtime only moves once per host frame: when reading the packets of the
// frame takes long, the later ones would get a round trip too short
static double g_cvReadReal, g_cvReadSys;

void SV_ClientViewReadPackets()
{
	if (sv_unlag_clientview.value == 0.0f)
	{
		g_cvReadSys = 0.0;
		return;
	}

	g_cvReadReal = realtime;
	g_cvReadSys = Sys_FloatTime();
}

// once per usercmd packet, before its commands run: newestmsec is the frame
// time of its newest command, oldermsec the frame time of all the others the
// client ran since its previous packet
void SV_ClientViewPacket(client_t *cl, int newestmsec, int oldermsec)
{
	if (!SV_ClientViewActive(cl) || !cl->active)
		return;

	cv_observer_t *cv = CV_Observer(cl);
	int ack = cl->netchan.incoming_acknowledged;
	cv->prevpacketack = cv->packetack;
	cv->packetack = ack;

	double now = realtime;
	if (g_cvReadSys > 0.0)
		now = g_cvReadReal + (Sys_FloatTime() - g_cvReadSys) * sys_timescale.value;

	// an ack older than the frame ring points at a reused frame
	client_frame_t *ackframe = &cl->frames[ack & SV_UPDATE_MASK];
	if (cl->netchan.outgoing_sequence - ack >= SV_UPDATE_BACKUP || ackframe->senttime <= 0.0 || ackframe->senttime > now)
	{
		cv->elapsed = 0.0;
		return;
	}

	double acktime = cv->svtime[ack & SV_UPDATE_MASK];
	if (acktime <= 0.0)
	{
		cv->elapsed = 0.0;
		return;
	}

	cv->elapsed = CV_ClockElapsed(&cv->clock, ack, acktime, now - ackframe->senttime, newestmsec, oldermsec);
}

void SV_ClientViewFrame(client_t *cl, double cmdage, const usercmd_t *cmd)
{
	if (!SV_ClientViewActive(cl) || !cl->active)
		return;

	cv_observer_t *cv = CV_Observer(cl);
	int observer = cl - g_psvs.clients;

	// refresh the ex_interp answer periodically, and soon after lerp_msec moved
	qboolean querydue = (cv->querytime + CV_QUERY_PERIOD < realtime)
		|| (cmd->lerp_msec != cv->querylerp && cv->querytime + CV_REQUERY_DELAY < realtime);

	if (querydue && cl->netchan.message.cursize + 32 < cl->netchan.message.maxsize)
	{
		MSG_WriteChar(&cl->netchan.message, svc_sendcvarvalue2);
		MSG_WriteLong(&cl->netchan.message, CV_QUERY_ID | observer);
		MSG_WriteString(&cl->netchan.message, "ex_interp");
		cv->querytime = realtime;
		cv->querylerp = cmd->lerp_msec;
	}

	// this command was built right after the render being replayed, whose
	// frame time is the one of the previous command
	float dt = cv->prevmsec / 1000.0f;
	cv->prevmsec = cmd->msec;

	int ack = cl->netchan.incoming_acknowledged;
	client_frame_t *ackframe = &cl->frames[ack & SV_UPDATE_MASK];
	double acktime = cv->svtime[ack & SV_UPDATE_MASK];
	if (cl->netchan.outgoing_sequence - ack >= SV_UPDATE_BACKUP || ackframe->senttime == 0.0 || acktime <= 0.0)
	{
		// nothing to replay: this command falls back to the stock rewind
		for (int i = 0; i < g_psvs.maxclients; i++)
			cv->victim[i].drawn = FALSE;

		return;
	}

	int newestseq = cl->netchan.outgoing_sequence - 1;
	double newest = cv->svtime[newestseq & SV_UPDATE_MASK];
	if (newest < acktime)
		newest = acktime;

	// the client clock had run on past the newest snapshot it had
	double clock = acktime + cv->elapsed;
	double cltime, target;
	float interp = CV_ClientInterp(cmd->lerp_msec, cv->exinterp);
	CV_ViewTimes(clock, cmdage, g_psv.time, newest, interp, sv_maxunlag.value, sv_unlagpush.value, &cltime, &target);
	cv->lastcltime = cltime;
	cv->lasttarget = target;
	cv->lastinterp = interp;

	// the client had snapshots up to the ack, a view moved later by
	// sv_unlagpush/sv_maxunlag also sees the ones sent after it
	qboolean movedview = (cltime > clock - cmdage + 0.0005) ? TRUE : FALSE;
	int startseq = movedview ? newestseq : ack;
	double starttime = cv->svtime[startseq & SV_UPDATE_MASK];

	for (int number = 1; number <= g_psvs.maxclients; number++)
	{
		cv_victim_t *v = &cv->victim[number - 1];
		v->drawn = FALSE;

		if (number == observer + 1)
			continue;

		// history known to the client at cltime: snapshots that had arrived
		// by then, newest first, one entry per animtime change
		double at[CV_HISTORY_DEPTH];
		entity_state_t *st[CV_HISTORY_DEPTH];
		entity_state_t *latest = NULL;
		int count = 0;

		for (int i = 0; i < CV_HISTORY_DEPTH * 2 && count < CV_HISTORY_DEPTH; i++)
		{
			int seq = startseq - i;
			client_frame_t *frame = &cl->frames[seq & SV_UPDATE_MASK];
			double svtime = cv->svtime[seq & SV_UPDATE_MASK];
			if (frame->senttime == 0.0 || svtime <= 0.0 || svtime > starttime)
				break;

			if (!CV_SnapshotKnown(seq, svtime, ack, cv->prevpacketack, cmdage, cltime, cmd->msec / 1000.0, movedview))
				continue;

			entity_state_t *s = CV_FindState(cv, frame, seq & SV_UPDATE_MASK, number);
			if (!s)
				break;

			if (!latest)
				latest = s;

			double t = CV_ClientAnimtime(s->animtime, cv->timebase);
			if (count && (int)(at[count - 1] * 1000.0) - (int)(t * 1000.0) <= 1)
			{
				// same history entry, the client recorded its first packet
				st[count - 1] = s;
				continue;
			}

			at[count] = t;
			st[count] = s;
			count++;

			if (count >= 3 && t < target)
				break;
		}

		if (!latest || latest->modelindex <= 0 || latest->modelindex >= MAX_MODELS)
			continue;

		model_t *pModel = g_psv.models[latest->modelindex];
		if (!pModel || pModel->type != mod_studio)
			continue;

		studiohdr_t *phdr = (studiohdr_t *)Mod_Extradata(pModel);
		if (!phdr || latest->sequence < 0 || latest->sequence >= phdr->numseq || latest->gaitsequence < 0 || latest->gaitsequence >= phdr->numseq)
			continue;

		CV_PureOrigin(at, st, count, target, v->origin, v->angles);

		if (v->gaitvalid && latest->sequence != v->sequence)
		{
			v->prevsequence = v->sequence;
			v->sequencetime = CV_ClientAnimtime(latest->animtime, cv->timebase);
		}

		v->cltime = cltime;
		v->sequence = latest->sequence;
		v->gaitsequence = latest->gaitsequence;
		v->frame = CV_WireFrame(latest->frame);
		v->framerate = latest->framerate;
		v->animtime = CV_ClientAnimtime(latest->animtime, cv->timebase);
		v->usehull = latest->usehull;
		Q_memcpy(v->controller, latest->controller, sizeof(v->controller));

		// the client's player_info_t starts zeroed on every connection and map
		// load (CL_ClearState) and so does this state: the first drawn frame
		// takes the gait yaw from the direction of the world origin, and a
		// standing player keeps it until it moves
		v->gaitvalid = TRUE;

		if (v->gaitsequence)
			CV_ProcessGait(v, phdr, dt);

		// StudioSetupBones keeps the frame of the current sequence for the
		// next transition while none is being blended
		if (!(v->sequencetime && v->sequencetime + 0.2 > v->cltime && v->prevsequence < phdr->numseq))
			v->prevframe = (float)CV_EstimateFrame(v, (mstudioseqdesc_t *)((byte *)phdr + phdr->seqindex) + v->sequence);

		v->drawn = TRUE;
	}
}

qboolean SV_ClientViewOrigin(client_t *cl, int number, vec_t *origin, int *usehull)
{
	if (!SV_ClientViewActive(cl) || number < 1 || number > g_psvs.maxclients)
		return FALSE;

	cv_victim_t *v = &CV_Observer(cl)->victim[number - 1];
	if (!v->drawn)
		return FALSE;

	VectorCopy(v->origin, origin);
	*usehull = v->usehull ? 1 : 0;
	return TRUE;
}

// sv_unlag_trace: the last replayed render of this client
qboolean SV_ClientViewTraceTimes(client_t *cl, double *cltime, double *target, float *interp)
{
	if (!SV_ClientViewActive(cl))
		return FALSE;

	cv_observer_t *cv = CV_Observer(cl);
	*cltime = cv->lastcltime;
	*target = cv->lasttarget;
	*interp = cv->lastinterp;
	return TRUE;
}

// sv_unlag_trace: the emulated state of one drawn player
const cv_victim_t *SV_ClientViewTraceVictim(client_t *cl, int number)
{
	if (!SV_ClientViewActive(cl) || number < 1 || number > g_psvs.maxclients)
		return NULL;

	const cv_victim_t *v = &CV_Observer(cl)->victim[number - 1];
	return v->drawn ? v : NULL;
}

qboolean SV_ClientViewHasPose(client_t *cl, const edict_t *edict)
{
	if (!SV_ClientViewActive(cl) || !edict)
		return FALSE;

	int number = NUM_FOR_EDICT(edict);
	if (number < 1 || number > g_psvs.maxclients)
		return FALSE;

	cv_victim_t *v = &CV_Observer(cl)->victim[number - 1];
	return (v->drawn && v->gaitsequence) ? TRUE : FALSE;
}

// CGameStudioModelRenderer::StudioSetupBones for the pose the client drew
// (no sequence transition blending yet), result in bonetransform
qboolean SV_ClientViewSetupBones(client_t *cl, const edict_t *edict, model_t *pModel, const vec_t *origin)
{
	if (!SV_ClientViewActive(cl) || !edict)
		return FALSE;

	int number = NUM_FOR_EDICT(edict);
	if (number < 1 || number > g_psvs.maxclients)
		return FALSE;

	cv_victim_t *v = &CV_Observer(cl)->victim[number - 1];
	if (!v->drawn || !v->gaitsequence)
		return FALSE;

	static float pos[MAXSTUDIOBONES][3], pos2[MAXSTUDIOBONES][3];
	static vec4_t q[MAXSTUDIOBONES], q2[MAXSTUDIOBONES];

	pstudiohdr = (studiohdr_t *)Mod_Extradata(pModel);
	if (!pstudiohdr || v->sequence >= pstudiohdr->numseq || v->gaitsequence >= pstudiohdr->numseq)
		return FALSE;

	int numbones = pstudiohdr->numbones;
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)pstudiohdr + pstudiohdr->boneindex);
	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)pstudiohdr + pstudiohdr->seqindex) + v->sequence;
	mstudioanim_t *panim = R_GetAnim(pModel, pseqdesc);

	double f = CV_EstimateFrame(v, pseqdesc);

	unsigned char blending[2] = { v->blending[0], v->blending[1] };
	if (v->gaitsequence == ANIM_WALK_SEQUENCE)
		blending[0] = (blending[0] <= 26) ? 0 : blending[0] - 26;

	CV_CalcPlayerSequence(pbones, numbones, pseqdesc, panim, v->controller, blending, (float)f, pos, q);

	// blend out of the previous sequence for 0.2s, with the current blends
	// (the client overwrites prevseqblending on every render)
	if (v->sequencetime && v->sequencetime + 0.2 > v->cltime && v->prevsequence < pstudiohdr->numseq)
	{
		mstudioseqdesc_t *pprevdesc = (mstudioseqdesc_t *)((byte *)pstudiohdr + pstudiohdr->seqindex) + v->prevsequence;
		CV_CalcPlayerSequence(pbones, numbones, pprevdesc, R_GetAnim(pModel, pprevdesc), v->controller, blending, v->prevframe, pos2, q2);

		float s = 1.0f - (float)((v->cltime - v->sequencetime) / 0.2);
		R_StudioSlerpBones(q, pos, q2, pos2, s);
	}

	// legs and pelvis come from the gait sequence at the client's gait frame
	if ((v->sequence < ANIM_FIRST_DEATH_SEQUENCE || v->sequence > ANIM_LAST_DEATH_SEQUENCE)
		&& (v->sequence < ANIM_FIRST_EMOTION_SEQUENCE || v->sequence > ANIM_LAST_EMOTION_SEQUENCE)
		&& v->sequence != ANIM_SWIM_1 && v->sequence != ANIM_SWIM_2)
	{
		mstudioseqdesc_t *pgaitdesc = (mstudioseqdesc_t *)((byte *)pstudiohdr + pstudiohdr->seqindex) + v->gaitsequence;
		CV_CalcRotations(pbones, numbones, pgaitdesc, R_GetAnim(pModel, pgaitdesc), v->controller, v->gaitframe, pos2, q2);

		qboolean copy = TRUE;
		for (int i = 0; i < numbones; i++)
		{
			if (!Q_strcmp(pbones[i].name, "Bip01 Spine"))
				copy = FALSE;
			else if (pbones[i].parent >= 0 && !Q_strcmp(pbones[pbones[i].parent].name, "Bip01 Pelvis"))
				copy = TRUE;

			if (copy)
			{
				VectorCopy(pos2[i], pos[i]);
				Q_memcpy(q[i], q2[i], sizeof(q[i]));
			}
		}
	}

	// StudioSetUpTransform: yaw follows the gait, the pitch was consumed by the blend
	vec3_t angles = { 0.0f, v->gaityaw, v->angles[ROLL] };
	if (angles[YAW] < 0.0f)
		angles[YAW] += 360.0f;

	AngleMatrix(angles, rotationmatrix);
	rotationmatrix[0][3] = origin[0];
	rotationmatrix[1][3] = origin[1];
	rotationmatrix[2][3] = origin[2];

	float bonematrix[3][4];
	for (int i = 0; i < numbones; i++)
	{
		QuaternionMatrix(q[i], bonematrix);
		bonematrix[0][3] = pos[i][0];
		bonematrix[1][3] = pos[i][1];
		bonematrix[2][3] = pos[i][2];

		R_ConcatTransforms((pbones[i].parent == -1) ? rotationmatrix : bonetransform[pbones[i].parent], bonematrix, bonetransform[i]);
	}

	return TRUE;
}

#endif // REHLDS_FIXES
