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

#pragma once

#include "maintypes.h"
#include "studio.h"
#include "entity_state.h"

#ifdef REHLDS_FIXES

// Client view emulation (sv_unlag_clientview), the parts that only replay
// client-side rules and can be tested without a running server.

// One other player as a client renderer sees it: CGameStudioModelRenderer's
// per-player gait state plus what the last emulated frame drew.
struct cv_victim_t
{
	// player_info_t gait fields of the client, zero until the first drawn frame
	qboolean gaitvalid;		// drawn before: a sequence change starts a transition
	float gaityaw;
	float gaitframe;
	vec3_t prevgaitorigin;
	int lastgaitsequence;
	float yawmodifier;		// the client keeps it per entity (baseline.fuser1), 0 until first set

	// latched sequence transition (R_UpdateLatchedVars)
	int prevsequence;
	double sequencetime;
	float prevframe;

	// the last emulated render
	qboolean drawn;
	double cltime;
	vec3_t origin;
	vec3_t angles;
	int sequence;
	int gaitsequence;
	float frame;
	float framerate;
	double animtime;
	unsigned char controller[4];
	unsigned char blending[2];
	int usehull;
};

// How long the client clock ran past the newest snapshot it had when it
// rendered the frame the newest command of a packet was built after.
const int CV_RTT_WINDOW = 200;			// packets the shortest round trip is taken from

struct cv_clock_t
{
	qboolean valid;
	int prevack;
	double clock;						// client clock after the frame of the previous packet's newest command
	double delta;						// clock offset the client still has to adjust (g_clockdelta)
	double lo, hi;						// window of the previous packet
	int prevnewestmsec;
	float base[CV_RTT_WINDOW];			// round trip minus the window start, recent packets
	int count, pos;
};

double CV_ClockElapsed(cv_clock_t *c, int ack, double acktime, double rtt, int newestmsec, int oldermsec);

// CL_AdjustClock: one frame of the client pulling its clock toward the snapshot time
double CV_AdjustClock(double clock, double *delta);

// animtime as the client decodes DT_TIMEWINDOW_8 (whole centiseconds of the time base)
double CV_ClientAnimtime(float animtime, double timebase);

// values after the entity_state_player_t delta encoding
float CV_WireCoord(float v);
float CV_WireAngle(float a);
float CV_WireFrame(float f);

// interpolation the client renders with: lerp_msec, refined by a queried ex_interp close to it
float CV_ClientInterp(int lerpmsec, float exinterp);

// whether the client had a snapshot at the replayed render of a command
qboolean CV_SnapshotKnown(int seq, double svtime, int ack, int prevack, double cmdage, double cltime, double frametime, qboolean movedview);

// client clock of the replayed render and its interpolation time, after sv_maxunlag/sv_unlagpush
void CV_ViewTimes(double clock, double cmdage, double now, double newest, float interp, float maxunlag, float push, double *cltime, double *target);

// CL_PureOrigin over history entries ordered newest first
void CV_PureOrigin(const double *at, entity_state_t **st, int count, double t, vec_t *origin, vec_t *angles);

// one drawn frame of StudioEstimateGait/CalculateYawBlend/CalculatePitchBlend/StudioProcessGait
void CV_ProcessGait(cv_victim_t *v, studiohdr_t *phdr, float dt);

// CStudioModelRenderer::StudioEstimateFrame
double CV_EstimateFrame(const cv_victim_t *v, mstudioseqdesc_t *pseqdesc);

#endif // REHLDS_FIXES
