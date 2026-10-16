#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"
#include "sv_clientview.h"

TEST(ClientAnimtime, ClientView, 1000) {
	DOUBLES_EQUAL("whole centiseconds", 229.69, CV_ClientAnimtime(229.6937f, 0.0), 0.0001);
	// the local gametime base shifts the centisecond grid
	DOUBLES_EQUAL("grid of a local time base", 229.685, CV_ClientAnimtime(229.6937f, 129.435), 0.0001);
}

TEST(WireQuantization, ClientView, 1000) {
	DOUBLES_EQUAL("coord to 1/32", 10.03125, CV_WireCoord(10.04f), 0.000001);
	DOUBLES_EQUAL("negative coord toward zero", -10.03125, CV_WireCoord(-10.04f), 0.000001);
	DOUBLES_EQUAL("frame is a byte", 127.0, CV_WireFrame(127.9f), 0.000001);
	DOUBLES_EQUAL("frame wraps like the 8 bit field", 1.0, CV_WireFrame(257.5f), 0.000001);
	DOUBLES_EQUAL("negative angle comes back normalized", -61.25, CV_WireAngle(-61.25f), 0.006);
	DOUBLES_EQUAL("angle past 180 comes back negative", -90.0, CV_WireAngle(270.0f), 0.006);
	DOUBLES_EQUAL("16 bit angle", 118.75, CV_WireAngle(118.75f), 0.006);
}

TEST(ClientInterp, ClientView, 1000) {
	// ex_interp 0.01 goes out as lerp_msec 9: 0.01f * 1000 truncates
	DOUBLES_EQUAL("queried value refines lerp_msec", 0.01, CV_ClientInterp(9, 0.01f), 0.000001);
	// cl_updaterate 60 raises lerp_msec to 16 but leaves ex_interp 0.015 alone
	DOUBLES_EQUAL("queried value below lerp_msec", 0.015, CV_ClientInterp(16, 0.015f), 0.000001);
	DOUBLES_EQUAL("no answer keeps lerp_msec", 0.009, CV_ClientInterp(9, 0.0f), 0.000001);
	DOUBLES_EQUAL("an answer far from lerp_msec is ignored", 0.008, CV_ClientInterp(8, 0.5f), 0.000001);
	DOUBLES_EQUAL("lerp_msec is capped like the stock rewind", 0.1, CV_ClientInterp(150, 0.0f), 0.000001);
}

TEST(SnapshotKnown, ClientView, 1000) {
	// the newest command: everything up to the ack, even past its clock (the
	// frame read two snapshots and rendered before syncing)
	CHECK("newest command, acked snapshot ahead of the clock", CV_SnapshotKnown(50, 10.030, 50, 48, 0.0, 10.026, 0.002, FALSE));
	// a clock run past the ack (high fps, low ping) does not see unacked snapshots
	CHECK("newest command, never past the ack", !CV_SnapshotKnown(51, 10.040, 50, 48, 0.0, 10.050, 0.002, FALSE));

	// an older command of the packet surely had what the previous packet acked
	CHECK("older command, previous ack", CV_SnapshotKnown(48, 10.010, 50, 48, 0.004, 10.000, 0.002, FALSE));
	// newer ones once its clock had reached them
	CHECK("older command, arrived by its clock", CV_SnapshotKnown(49, 10.020, 50, 48, 0.004, 10.0198, 0.002, FALSE));
	// within half a frame (plus 0.5 ms) of the clock counts as arrived
	CHECK("older command, inside the tolerance", CV_SnapshotKnown(50, 10.030, 50, 48, 0.004, 10.0286, 0.002, FALSE));
	CHECK("older command, not yet arrived", !CV_SnapshotKnown(50, 10.030, 50, 48, 0.004, 10.0280, 0.002, FALSE));
	// a slower frame rate widens it
	CHECK("older command, 10 ms frames", CV_SnapshotKnown(50, 10.030, 50, 48, 0.010, 10.0260, 0.010, FALSE));

	// a view moved later by sv_unlagpush sees snapshots past the ack by its clock
	CHECK("pushed view, reached", CV_SnapshotKnown(52, 10.050, 50, 48, 0.0, 10.051, 0.002, TRUE));
	CHECK("pushed view, not reached", !CV_SnapshotKnown(53, 10.060, 50, 48, 0.0, 10.051, 0.002, TRUE));

	// no previous packet yet
	CHECK("first packet, older command", !CV_SnapshotKnown(50, 10.030, 50, 0, 0.004, 10.020, 0.002, FALSE));
}

TEST(ViewTimes, ClientView, 1000) {
	double cltime, target;

	// stock settings: the newest command sees the acknowledged snapshot
	CV_ViewTimes(10.0, 0.0, 10.1, 10.08, 0.01f, 0.5f, 0.0f, &cltime, &target);
	DOUBLES_EQUAL("clock is the acked snapshot", 10.0, cltime, 0.000001);
	DOUBLES_EQUAL("target is interp behind it", 9.99, target, 0.000001);

	// an older command of the packet was built (and rendered) earlier
	CV_ViewTimes(10.0, 0.02, 10.1, 10.08, 0.01f, 0.5f, 0.0f, &cltime, &target);
	DOUBLES_EQUAL("older command clock", 9.98, cltime, 0.000001);
	DOUBLES_EQUAL("older command target", 9.97, target, 0.000001);

	// sv_maxunlag caps how far back the view goes, interp is not part of the cap
	CV_ViewTimes(10.0, 0.0, 11.0, 10.98, 0.01f, 0.5f, 0.0f, &cltime, &target);
	DOUBLES_EQUAL("maxunlag caps the clock", 10.5, cltime, 0.000001);
	DOUBLES_EQUAL("maxunlag keeps interp", 10.49, target, 0.000001);

	CV_ViewTimes(10.0, 0.0, 11.0, 10.98, 0.01f, 0.0f, 0.0f, &cltime, &target);
	DOUBLES_EQUAL("maxunlag 0 means no cap", 10.0, cltime, 0.000001);

	// the cap applies to the command age as well
	CV_ViewTimes(10.0, 0.2, 10.4, 10.38, 0.01f, 0.5f, 0.0f, &cltime, &target);
	DOUBLES_EQUAL("old command capped", 9.9, cltime, 0.000001);

	// sv_unlagpush moves the view later, up to the newest snapshot
	CV_ViewTimes(10.0, 0.0, 10.1, 10.08, 0.01f, 0.5f, 0.03f, &cltime, &target);
	DOUBLES_EQUAL("push moves the clock", 10.03, cltime, 0.000001);
	DOUBLES_EQUAL("push moves the target", 10.02, target, 0.000001);

	CV_ViewTimes(10.0, 0.0, 10.1, 10.08, 0.01f, 0.5f, 0.5f, &cltime, &target);
	DOUBLES_EQUAL("push stops at the newest snapshot", 10.08, cltime, 0.000001);
	DOUBLES_EQUAL("no rewind past the newest snapshot", 10.08, target, 0.000001);

	// a push between interp and the full rewind eats the interp last
	CV_ViewTimes(10.0, 0.0, 10.1, 10.08, 0.01f, 0.5f, 0.085f, &cltime, &target);
	DOUBLES_EQUAL("clock capped first", 10.08, cltime, 0.000001);
	DOUBLES_EQUAL("target still interp behind the pushed clock", 10.075, target, 0.000001);

	// negative push rewinds further back than the client saw
	CV_ViewTimes(10.0, 0.0, 10.1, 10.08, 0.01f, 0.5f, -0.02f, &cltime, &target);
	DOUBLES_EQUAL("negative push clock", 9.98, cltime, 0.000001);
	DOUBLES_EQUAL("negative push target", 9.97, target, 0.000001);

	// a LAN client at high fps: its clock ran past the newest snapshot sent,
	// which only limits a push
	CV_ViewTimes(10.006, 0.0, 10.0011, 10.001, 0.01f, 0.5f, 0.0f, &cltime, &target);
	DOUBLES_EQUAL("own clock past the newest snapshot", 10.006, cltime, 0.000001);
	DOUBLES_EQUAL("target follows the own clock", 9.996, target, 0.000001);

	CV_ViewTimes(10.006, 0.0, 10.0011, 10.001, 0.01f, 0.5f, 0.03f, &cltime, &target);
	DOUBLES_EQUAL("push keeps the own clock", 10.006, cltime, 0.000001);
	DOUBLES_EQUAL("pushed target stops at the own clock", 10.006, target, 0.000001);
}

TEST(ClockReplay, ClientView, 1000) {
	cv_clock_t c;
	double clock, delta;

	// CL_AdjustClock: whole milliseconds, at most 7.5 per frame, nothing under 1 ms
	delta = 0.0303;
	clock = CV_AdjustClock(10.0, &delta);
	DOUBLES_EQUAL("capped step", 10.0 - 0.0075, clock, 0.000001);
	DOUBLES_EQUAL("rest left", 0.0228, delta, 0.000001);
	delta = -0.0049;
	clock = CV_AdjustClock(10.0, &delta);
	DOUBLES_EQUAL("whole ms only", 10.004, clock, 0.000001);
	DOUBLES_EQUAL("fraction stays", -0.0009, delta, 0.000001);
	delta = 0.0009;
	DOUBLES_EQUAL("under a millisecond", 10.0, CV_AdjustClock(10.0, &delta), 0.000001);

	// fps = updaterate, one command per packet: in sync
	Q_memset(&c, 0, sizeof(c));
	for (int i = 0; i < 10; i++)
		DOUBLES_EQUAL("in sync", 0.0, CV_ClockElapsed(&c, 100 + i, 10.0 + 0.010 * i, 0.100, 10, 0), 0.000001);

	// fps 60 against 100 snapshots a second: the frame that read new
	// snapshots rendered on the previous frame's clock plus its own 16 ms
	Q_memset(&c, 0, sizeof(c));
	CV_ClockElapsed(&c, 1, 10.0, 0.100, 16, 0);
	DOUBLES_EQUAL("one snapshot read, clock ahead", 0.0062, CV_ClockElapsed(&c, 2, 10.0098, 0.100, 16, 0), 0.000001);
	DOUBLES_EQUAL("two snapshots read, clock behind", -0.0034, CV_ClockElapsed(&c, 4, 10.0294, 0.100, 16, 0), 0.000001);

	// a burst after a stall: the render before the read lags 30 ms, then the
	// client catches up 7.5 ms a frame
	Q_memset(&c, 0, sizeof(c));
	CV_ClockElapsed(&c, 1, 10.0, 0.100, 10, 0);
	static const double catchup[] = { -0.0303, -0.0228, -0.0153, -0.0078, -0.0008 };
	for (int i = 0; i < ARRAYSIZE(catchup); i++)
		DOUBLES_EQUAL("catching up", catchup[i], CV_ClockElapsed(&c, 5 + i, 10.0403 + 0.010 * i, 0.100, 10, 0), 0.000001);

	// off by more than cl_clockreset: the client snaps to the snapshot time
	Q_memset(&c, 0, sizeof(c));
	CV_ClockElapsed(&c, 1, 10.000, 0.100, 10, 0);
	DOUBLES_EQUAL("render before the reset", -0.2903, CV_ClockElapsed(&c, 31, 10.3003, 0.100, 10, 0), 0.000001);
	DOUBLES_EQUAL("synced after the reset", 0.0, CV_ClockElapsed(&c, 32, 10.3103, 0.100, 10, 0), 0.000001);

	// no new snapshot: the clock runs on frame by frame
	DOUBLES_EQUAL("same ack, one frame later", 0.010, CV_ClockElapsed(&c, 32, 10.3103, 0.110, 10, 0), 0.000001);
	DOUBLES_EQUAL("same ack, two frames later", 0.020, CV_ClockElapsed(&c, 32, 10.3103, 0.120, 10, 0), 0.000001);

	// and a packet of several commands with that same ack keeps counting
	DOUBLES_EQUAL("several commands, same ack", 0.040, CV_ClockElapsed(&c, 32, 10.3103, 0.140, 10, 10), 0.000001);
}

TEST(ClockElapsedWindow, ClientView, 1000) {
	cv_clock_t c;
	Q_memset(&c, 0, sizeof(c));

	// five 2 ms frames per packet: the snapshot was read 0..8 ms before the
	// newest render, the round trip above the shortest one places it
	for (int i = 0; i < 20; i++)
		CV_ClockElapsed(&c, 200 + i, 10.0 + 0.01 * i, 0.100 + 0.002 * (i % 5), 2, 8);

	// shortest round trip 100 ms: 106 ms waited 6 ms, the newest frame (2 ms) not included
	DOUBLES_EQUAL("round trip places the read", 0.004, CV_ClockElapsed(&c, 300, 11.0, 0.106, 2, 8), 0.000001);
	DOUBLES_EQUAL("never before the window", 0.0, CV_ClockElapsed(&c, 301, 11.01, 0.088, 2, 8), 0.000001);
	DOUBLES_EQUAL("never past the window", 0.008, CV_ClockElapsed(&c, 302, 11.02, 0.148, 2, 8), 0.000001);

	// the same ack again: the window moves on by the frames since
	DOUBLES_EQUAL("same ack moves the window", 0.018, CV_ClockElapsed(&c, 302, 11.02, 0.300, 2, 8), 0.000001);

	// a lag spike can only push the estimate to the end of the window, and a
	// lasting higher ping is learned once the old round trips leave the window
	for (int i = 0; i < CV_RTT_WINDOW; i++)
		CV_ClockElapsed(&c, 400 + i, 12.0 + 0.01 * i, 0.128 + 0.002 * (i % 5), 2, 8);

	DOUBLES_EQUAL("higher ping learned", 0.004, CV_ClockElapsed(&c, 999, 15.0, 0.134, 2, 8), 0.000001);
}

// Recorded from stock 8684 clients at ping 100 and cl_cmdrate 105, per
// usercmd packet { ack moved, acked snapshot time ms, round trip ms, newest
// msec, older msec, render clock past the acked snapshot ms, or -1000 when
// the client did not render the victim right before the newest command }.
struct clock_row_t { int moved; double acktime; float rtt; int newest; int older; float actual; };

static const clock_row_t g_clockFps500[] = {
	{ 1, 0.000, 103.98f, 2, 10, 5.44f }, { 1, 14.799, 99.19f, 2, 8, 0.64f },
	{ 1, 20.957, 103.03f, 2, 8, 4.49f }, { 1, 34.778, 99.36f, 2, 8, 0.67f },
	{ 1, 44.608, 99.55f, 2, 8, 0.98f }, { 1, 51.595, 102.53f, 2, 8, 3.99f },
	{ 1, 64.210, 99.94f, 2, 8, 1.41f }, { 0, 64.210, 109.94f, 2, 8, 10.42f },
	{ 1, 78.278, 105.94f, 2, 8, 6.35f }, { 1, 88.093, 105.94f, 2, 8, 6.54f },
	{ 1, 97.946, 108.21f, 2, 10, 8.68f }, { 1, 113.706, 102.44f, 2, 8, 3.92f },
	{ 1, 123.549, 102.75f, 2, 8, 4.08f }, { 1, 133.403, 103.13f, 2, 8, 4.53f },
	{ 1, 139.477, 108.16f, 2, 10, 7.58f }, { 1, 159.043, 100.59f, 2, 10, 1.01f },
	{ 1, 173.720, 97.86f, 2, 10, -1.66f }, { 1, 179.021, 102.56f, 2, 8, 4.04f },
	{ 1, 193.655, 98.40f, 2, 8, -0.60f }, { 1, 198.850, 105.17f, 2, 10, 6.63f },
	{ 1, 210.326, 103.16f, 2, 8, 4.27f }, { 1, 224.302, 101.83f, 2, 10, 2.30f },
	{ 1, 230.363, 105.79f, 2, 8, 6.24f }, { 1, 244.216, 101.92f, 2, 8, 2.38f },
	{ 1, 251.221, 106.92f, 2, 10, 7.37f }, { 1, 265.137, 102.98f, 2, 8, 3.46f },
	{ 1, 277.402, 100.75f, 2, 8, 1.19f }, { 0, 277.402, 112.83f, 2, 10, 12.18f },
	{ 1, 296.956, 103.46f, 2, 8, 3.74f }, { 1, 308.161, 102.18f, 2, 8, 2.66f },
	{ 1, 319.336, 101.30f, 2, 9, 1.73f }, { 1, 330.817, 99.76f, 2, 8, 0.25f },
	{ 0, 330.817, 110.86f, 3, 8, 10.59f }, { 1, 345.284, 107.54f, 3, 8, 8.65f },
	{ 1, 362.278, 102.58f, 3, 9, 2.64f }, { 1, 374.812, 100.89f, 3, 8, 0.76f },
	{ 1, 386.146, 100.01f, 2, 8, 0.20f }, { 1, 396.946, 99.12f, 2, 8, -0.38f },
	{ 0, 396.946, 109.17f, 2, 8, 9.62f }, { 1, 415.066, 101.06f, 2, 8, 1.50f },
	{ 0, 415.066, 111.15f, 2, 8, 11.50f }, { 1, 435.250, 102.97f, 2, 10, 3.40f },
	{ 1, 449.701, 98.70f, 2, 8, -1.01f }, { 1, 457.602, 100.73f, 2, 8, 2.18f },
	{ 0, 457.602, 110.76f, 2, 8, 10.18f }, { 1, 479.280, 99.30f, 3, 8, 0.50f },
	{ 1, 490.302, 100.27f, 2, 10, 1.67f }, { 0, 490.302, 110.27f, 2, 8, 10.72f },
	{ 1, 504.215, 106.38f, 2, 8, 6.81f }, { 1, 513.921, 106.63f, 2, 8, 6.10f },
	{ 1, 525.102, 105.55f, 2, 8, 5.97f }, { 1, 533.832, 106.76f, 2, 8, 7.24f },
	{ 1, 542.500, 108.15f, 2, 8, 8.57f }, { 1, 556.579, 104.08f, 2, 8, 4.49f },
	{ 1, 566.405, 104.25f, 2, 8, 4.69f }, { 1, 574.161, 106.56f, 2, 8, 6.98f },
	{ 1, 589.743, 101.92f, 2, 10, 3.40f }, { 1, 596.857, 107.81f, 2, 10, 8.28f },
	{ 1, 615.770, 98.94f, 2, 8, -0.63f }, { 0, 615.770, 110.87f, 2, 10, 11.36f },
	{ 1, 635.643, 101.06f, 2, 8, 1.49f }, { 0, 635.643, 111.11f, 2, 8, 11.49f },
	{ 1, 650.407, 106.49f, 2, 8, 6.92f }, { 1, 670.024, 98.91f, 2, 10, -0.66f },
	{ 1, 676.675, 102.23f, 2, 8, 2.68f }, { 0, 676.675, 112.22f, 2, 8, 12.68f },
	{ 1, 698.814, 102.06f, 2, 10, 2.54f }, { 0, 698.814, 112.06f, 2, 8, 12.54f },
	{ 1, 718.328, 102.55f, 2, 8, 3.03f }, { 1, 730.673, 100.17f, 2, 8, 0.69f },
	{ 0, 730.673, 110.24f, 2, 8, 10.69f }, { 1, 745.534, 105.51f, 4, 8, 5.87f },
	{ 1, 756.453, 106.52f, 2, 8, 7.99f }, { 1, 774.813, 100.15f, 2, 10, 1.64f },
	{ 0, 774.813, 110.19f, 2, 8, 10.63f }, { 1, 793.779, 101.17f, 2, 8, 1.67f },
	{ 1, 805.510, 99.43f, 2, 8, -0.07f }, { 1, 815.826, 101.15f, 2, 10, 1.61f },
	{ 0, 815.826, 111.18f, 2, 8, 11.61f }, { 1, 830.521, 106.55f, 2, 8, 7.96f },
	{ 1, 840.333, 106.82f, 2, 8, 8.21f }, { 1, 851.044, 108.03f, 2, 10, -1000.00f },
	{ 1, 860.977, 108.09f, 2, 8, 7.06f }, { 1, 879.060, 100.01f, 2, 8, -1.02f },
	{ 1, 892.117, 98.12f, 2, 10, -1.08f }, { 0, 892.117, 108.99f, 2, 8, 9.92f },
	{ 1, 911.970, 98.16f, 2, 8, 0.06f }, { 1, 921.084, 100.03f, 2, 8, 0.95f },
	{ 1, 932.105, 97.96f, 2, 8, -0.03f }, { 0, 932.105, 108.99f, 2, 8, 9.97f },
	{ 1, 952.252, 97.65f, 2, 8, -1.18f }, { 0, 952.252, 108.89f, 2, 8, 9.82f },
	{ 1, 963.156, 106.70f, 2, 8, 8.91f }, { 1, 974.156, 108.98f, 2, 10, 9.92f },
	{ 1, 989.020, 104.13f, 2, 8, 4.05f }, { 1, 995.959, 107.17f, 2, 8, 7.10f },
	{ 0, 995.959, 117.13f, 2, 8, 17.10f }, { 1, 1015.356, 108.63f, 2, 8, 7.75f },
	{ 0, 1015.356, 117.86f, 2, 8, 17.78f }, { 1, 1035.655, 107.56f, 2, 8, 7.48f },
	{ 1, 1054.585, 98.57f, 2, 8, -1.45f }, { 0, 1054.585, 108.57f, 2, 8, 9.54f },
	{ 1, 1074.556, 98.60f, 2, 8, -1.43f }, { 0, 1074.556, 110.60f, 2, 10, 11.57f },
	{ 1, 1087.451, 107.68f, 2, 8, 8.67f }, { 1, 1097.382, 107.92f, 2, 8, 8.78f },
	{ 1, 1111.337, 103.88f, 2, 8, 4.83f }, { 1, 1122.766, 102.52f, 2, 8, 2.41f },
	{ 1, 1131.161, 104.05f, 2, 8, 4.01f }, { 1, 1142.473, 102.74f, 2, 8, 3.70f },
	{ 1, 1152.337, 102.88f, 2, 8, 3.83f }, { 1, 1162.216, 103.00f, 2, 8, 3.96f },
	{ 1, 1170.814, 104.46f, 2, 8, 5.44f }, { 1, 1182.008, 105.32f, 2, 10, 6.24f },
	{ 1, 1190.832, 108.46f, 2, 10, 9.41f }, { 0, 1190.832, 117.85f, 2, 8, 19.45f },
	{ 1, 1211.297, 108.05f, 2, 8, 8.98f }, { 0, 1211.297, 117.28f, 2, 8, 18.98f },
	{ 1, 1231.565, 107.83f, 2, 8, 8.77f }, { 1, 1248.559, 102.84f, 2, 10, 3.77f },
	{ 1, 1258.602, 104.80f, 2, 10, 5.73f }, { 1, 1268.712, 104.69f, 2, 8, 5.61f },
	{ 1, 1280.286, 105.05f, 2, 10, 6.03f }, { 1, 1293.020, 102.36f, 2, 8, 3.30f },
	{ 1, 1304.719, 100.92f, 2, 9, 1.84f }, { 0, 1304.719, 110.89f, 2, 8, 11.84f },
	{ 1, 1322.865, 104.72f, 2, 10, 5.70f }, { 1, 1333.511, 104.13f, 2, 8, 4.09f },
	{ 1, 1348.216, 101.52f, 2, 10, 1.38f }, { 1, 1355.725, 103.98f, 2, 8, 3.93f },
	{ 1, 1367.716, 102.80f, 2, 8, 2.03f }, { 1, 1381.827, 99.93f, 3, 9, -0.74f },
	{ 1, 1391.546, 100.30f, 2, 8, 0.20f }, { 0, 1391.546, 110.79f, 2, 8, 10.47f },
	{ 1, 1410.731, 101.85f, 2, 9, 1.80f }, { 0, 1410.731, 111.87f, 2, 8, 11.83f },
	{ 1, 1433.309, 101.46f, 2, 10, 2.25f }, { 0, 1433.309, 111.65f, 2, 8, 12.57f },
	{ 1, 1449.517, 105.52f, 2, 8, 6.42f }, { 1, 1463.007, 103.53f, 2, 9, 4.13f },
	{ 1, 1473.770, 103.05f, 2, 9, 4.01f }, { 1, 1485.159, 102.20f, 2, 10, 5.65f },
	{ 1, 1495.408, 105.11f, 2, 9, 6.95f }, { 1, 1507.022, 103.50f, 2, 9, 4.47f },
	{ 1, 1521.457, 99.12f, 2, 8, 0.07f }, { 1, 1527.747, 102.81f, 2, 8, 2.78f },
	{ 1, 1539.421, 102.20f, 3, 8, 1.68f }, { 1, 1549.926, 103.70f, 2, 10, 3.70f },
	{ 1, 1565.015, 98.66f, 2, 8, -1.39f }, { 1, 1572.749, 100.91f, 2, 8, 1.87f },
	{ 0, 1572.749, 111.21f, 2, 8, 11.13f }, { 1, 1594.090, 99.83f, 2, 8, -0.21f },
	{ 0, 1594.090, 111.86f, 2, 10, 11.79f }, { 1, 1614.733, 101.20f, 2, 8, 1.14f },
	{ 0, 1614.733, 111.22f, 2, 8, 11.14f }, { 1, 1628.582, 107.37f, 2, 8, 7.29f },
	{ 1, 1648.493, 98.09f, 2, 10, -0.62f }, { 1, 1658.142, 101.81f, 2, 10, 1.76f },
	{ 1, 1669.684, 102.25f, 2, 10, 2.21f }, { 0, 1669.684, 112.38f, 2, 8, 12.21f },
	{ 1, 1688.810, 103.34f, 2, 8, 3.23f }, { 0, 1688.810, 113.33f, 2, 8, 13.23f },
	{ 1, 1711.733, 100.40f, 2, 8, 1.37f }, { 0, 1711.733, 110.42f, 2, 8, 11.37f },
	{ 1, 1731.525, 100.69f, 2, 8, 1.58f }, { 0, 1731.525, 110.63f, 2, 8, 11.58f },
	{ 1, 1752.737, 99.42f, 2, 8, 0.37f }, { 0, 1752.737, 109.42f, 2, 8, 10.37f },
	{ 1, 1764.600, 107.56f, 2, 8, 8.51f }, { 1, 1776.327, 105.83f, 2, 8, 6.78f },
	{ 1, 1790.502, 101.71f, 2, 8, 2.65f }, { 1, 1799.112, 103.20f, 2, 8, 4.09f },
	{ 1, 1815.707, 98.51f, 2, 10, -0.51f }, { 1, 1825.683, 100.02f, 2, 10, 1.51f },
	{ 1, 1835.801, 100.48f, 2, 8, 1.39f }, { 1, 1847.979, 100.30f, 2, 10, 1.21f },
	{ 1, 1859.278, 98.94f, 2, 8, -1.09f }, { 1, 1865.884, 102.39f, 2, 8, 3.31f },
	{ 0, 1865.884, 112.40f, 2, 8, 13.31f }, { 1, 1880.157, 108.30f, 2, 8, 8.23f },
	{ 1, 1890.818, 109.70f, 2, 10, 9.61f }, { 1, 1907.999, 102.52f, 2, 8, 3.44f },
	{ 0, 1907.999, 112.54f, 2, 8, 13.45f }, { 1, 1931.387, 99.11f, 2, 8, 0.06f },
	{ 0, 1931.387, 109.14f, 2, 8, 10.06f }, { 1, 1947.158, 103.36f, 2, 9, 4.29f },
	{ 1, 1961.741, 100.81f, 2, 9, 1.71f }, { 1, 1971.795, 100.82f, 2, 9, 1.78f },
	{ 1, 1982.211, 101.51f, 2, 8, 1.36f }, { 1, 1995.936, 98.95f, 2, 10, -1.26f },
	{ 0, 1995.936, 108.87f, 2, 8, 9.75f }, { 1, 2006.974, 107.97f, 2, 8, 8.88f },
	{ 1, 2024.138, 102.79f, 2, 10, 2.71f }, { 1, 2034.019, 104.87f, 2, 10, 5.83f },
	{ 1, 2044.023, 104.96f, 2, 8, 5.87f }, { 1, 2054.077, 105.02f, 2, 8, 5.92f },
	{ 1, 2068.532, 100.59f, 2, 8, 1.46f }, { 0, 2068.532, 110.54f, 2, 8, 10.49f },
	{ 1, 2083.718, 105.36f, 2, 8, -1000.00f }, { 1, 2100.438, 98.61f, 2, 8, -2.41f },
	{ 0, 2100.438, 108.62f, 2, 8, 9.58f }, { 1, 2119.496, 99.55f, 2, 8, 0.55f },
	{ 0, 2119.496, 109.61f, 2, 8, 10.55f }, { 1, 2132.096, 107.06f, 2, 8, 7.95f },
	{ 1, 2143.216, 107.92f, 2, 10, 8.83f }, { 1, 2153.388, 107.83f, 2, 8, 8.81f },
	{ 1, 2167.585, 103.70f, 2, 8, 4.62f }, { 1, 2177.401, 103.88f, 2, 8, 4.83f },
	{ 1, 2185.927, 107.35f, 2, 10, 8.30f }, { 1, 2205.883, 99.42f, 2, 10, -0.65f },
	{ 0, 2205.883, 109.39f, 2, 8, 9.34f }, { 1, 2225.816, 99.54f, 2, 8, -0.49f },
	{ 0, 2225.816, 109.62f, 2, 8, 9.55f }, { 1, 2237.059, 108.55f, 2, 9, 8.42f },
	{ 1, 2255.695, 99.89f, 2, 8, -0.18f }, { 1, 2266.720, 98.80f, 2, 8, -1.21f },
	{ 0, 2266.720, 110.85f, 2, 10, 11.79f }, { 1, 2286.104, 101.51f, 2, 8, 2.40f },
	{ 1, 2297.737, 99.85f, 2, 8, 0.77f }, { 0, 2297.737, 109.87f, 2, 8, 10.80f },
	{ 1, 2309.440, 108.22f, 2, 8, 8.10f }, { 1, 2323.645, 104.60f, 2, 8, 3.94f },
	{ 1, 2334.113, 103.56f, 2, 8, 3.49f }, { 1, 2345.293, 104.40f, 2, 10, 4.30f },
	{ 1, 2360.523, 99.16f, 2, 8, -0.90f }, { 0, 2360.523, 111.19f, 2, 10, 11.10f },
	{ 1, 2382.182, 99.47f, 2, 8, 0.44f }, { 0, 2382.182, 109.49f, 2, 8, 10.43f },
	{ 1, 2396.583, 105.12f, 2, 8, 6.03f }, { 1, 2405.027, 106.74f, 2, 8, 7.66f },
	{ 1, 2416.408, 105.55f, 2, 8, 6.39f }, { 1, 2425.218, 107.44f, 2, 8, 7.71f },
	{ 1, 2436.645, 105.38f, 2, 8, 6.29f }, { 1, 2446.461, 105.55f, 2, 8, 6.51f },
	{ 1, 2454.581, 107.43f, 2, 8, 8.39f }, { 1, 2465.006, 107.00f, 2, 8, 7.97f },
	{ 1, 2476.134, 105.98f, 2, 8, 6.84f }, { 1, 2488.876, 103.14f, 2, 8, 4.10f },
	{ 0, 2488.876, 113.17f, 2, 8, 14.10f }, { 1, 2506.753, 105.84f, 2, 8, 6.38f },
	{ 1, 2521.596, 100.63f, 2, 8, 1.55f }, { 1, 2527.303, 105.24f, 2, 8, 5.99f },
	{ 1, 2537.676, 104.60f, 3, 9, 5.98f }, { 1, 2555.520, 100.10f, 2, 10, 1.02f },
	{ 1, 2567.077, 100.63f, 2, 10, 1.47f }, { 0, 2567.077, 110.56f, 2, 8, 11.48f },
	{ 1, 2587.157, 100.56f, 2, 8, 1.53f }, { 0, 2587.157, 110.59f, 2, 8, 11.53f },
	{ 1, 2602.047, 105.73f, 2, 8, 2.68f }, { 1, 2612.593, 105.26f, 2, 8, 2.14f },
	{ 1, 2627.394, 100.94f, 2, 8, 1.72f }, { 0, 2627.394, 111.21f, 2, 9, 12.07f },
	{ 1, 2646.992, 102.55f, 2, 8, 2.37f }, { 1, 2659.882, 99.82f, 2, 9, -0.26f },
	{ 0, 2659.882, 109.94f, 2, 8, 9.92f }, { 1, 2671.729, 109.86f, 2, 10, 10.07f },
	{ 1, 2685.436, 106.78f, 2, 8, 6.68f }, { 1, 2695.346, 106.11f, 2, 8, 7.84f },
	{ 1, 2715.093, 99.76f, 4, 9, -1.03f }, { 1, 2721.451, 103.83f, 3, 7, 4.60f },
	{ 1, 2732.107, 104.61f, 3, 9, 6.06f }, { 1, 2743.798, 103.90f, 2, 9, 4.88f },
	{ 1, 2754.507, 103.26f, 2, 8, 4.24f }, { 1, 2764.572, 103.20f, 2, 8, 4.17f },
	{ 1, 2779.220, 98.55f, 2, 8, -0.48f }, { 1, 2787.156, 100.66f, 2, 8, 1.58f },
	{ 0, 2787.156, 110.69f, 2, 8, 11.59f }, { 1, 2806.266, 101.52f, 2, 8, 2.48f },
	{ 1, 2818.583, 101.20f, 2, 10, 1.16f }, { 1, 2829.093, 100.67f, 2, 8, 0.66f },
	{ 0, 2829.093, 110.72f, 2, 8, 10.67f }, { 1, 2851.348, 98.81f, 2, 8, -1.57f },
	{ 1, 2862.773, 97.91f, 2, 10, 0.32f }, { 1, 2873.225, 98.93f, 2, 8, -0.13f },
	{ 0, 2873.225, 109.25f, 2, 8, 10.19f }, { 1, 2891.616, 102.85f, 2, 10, 2.80f },
	{ 1, 2903.695, 101.35f, 2, 9, 1.29f }, { 1, 2919.338, 97.70f, 2, 10, -1000.00f },
	{ 1, 2925.156, 103.85f, 2, 10, 5.83f }, { 0, 2925.156, 113.85f, 2, 8, 15.83f },
	{ 1, 2946.101, 103.91f, 2, 9, 4.86f }, { 0, 2946.101, 115.90f, 2, 10, 16.85f },
	{ 1, 2967.537, 104.41f, 2, 8, 5.42f }, { 0, 2967.537, 114.58f, 2, 8, 15.42f },
	{ 1, 2985.934, 106.08f, 2, 8, 7.02f }, { 1, 2998.579, 101.80f, 2, 8, 4.54f },
	{ 1, 3012.471, 99.83f, 2, 8, 1.76f }, { 1, 3023.833, 98.49f, 2, 8, 0.39f },
	{ 0, 3023.833, 110.83f, 2, 10, 12.39f }, { 1, 3043.887, 100.39f, 2, 8, 2.33f },
	{ 0, 3043.887, 110.40f, 2, 8, 10.33f }, { 1, 3058.528, 105.75f, 2, 8, 5.69f },
	{ 1, 3067.386, 106.92f, 2, 8, 7.84f }, { 1, 3081.900, 102.53f, 2, 8, 2.47f },
	{ 1, 3095.630, 98.84f, 3, 8, -1.26f }, { 1, 3106.153, 100.38f, 2, 10, 1.33f },
	{ 0, 3106.153, 110.38f, 2, 8, 10.33f }, { 1, 3121.486, 105.04f, 2, 8, 5.99f },
	{ 1, 3138.894, 99.62f, 2, 10, 0.58f }, { 1, 3149.976, 98.54f, 2, 8, -0.51f },
};

static const clock_row_t g_clockFps60[] = {
	{ 1, 0.000, 118.38f, 17, 0, -3.99f }, { 1, 9.826, 125.09f, 16, 0, 6.71f },
	{ 1, 31.486, 119.95f, 17, 0, -4.42f }, { 1, 51.168, 116.86f, 16, 0, -3.57f },
	{ 1, 61.029, 123.51f, 17, 0, 6.10f }, { 1, 80.828, 120.22f, 16, 0, -3.17f },
	{ 1, 100.517, 117.10f, 17, 0, -3.33f }, { 1, 110.376, 123.80f, 16, 0, 6.34f },
	{ 1, 130.531, 120.07f, 17, 0, -3.28f }, { 1, 150.829, 116.38f, 16, 0, -4.05f },
	{ 1, 160.686, 121.00f, 17, 0, 6.63f }, { 1, 179.832, 120.39f, 16, 0, -1.99f },
	{ 1, 200.766, 116.01f, 17, 0, -5.39f }, { 1, 210.660, 122.65f, 16, 0, 6.24f },
	{ 1, 230.364, 119.67f, 17, 0, -2.93f }, { 1, 250.017, 116.36f, 17, 0, -4.05f },
	{ 1, 259.836, 121.32f, 16, 0, 6.66f }, { 1, 279.601, 119.77f, 17, 0, -2.58f },
	{ 1, 290.387, 125.52f, 16, 0, 5.17f }, { 1, 310.047, 122.42f, 17, 0, -2.96f },
	{ 1, 329.862, 119.10f, 16, 0, -4.25f }, { 1, 339.912, 125.63f, 17, 0, 6.24f },
	{ 1, 359.608, 122.44f, 16, 0, -2.93f }, { 1, 380.348, 118.27f, 17, 0, -5.14f },
	{ 1, 391.694, 123.43f, 16, 0, 5.04f }, { 1, 411.981, 118.48f, 17, 0, -3.71f },
	{ 1, 422.547, 125.65f, 16, 0, 5.25f }, { 1, 443.284, 121.47f, 17, 0, -3.95f },
	{ 1, 465.039, 116.19f, 16, 0, -6.18f }, { 1, 475.343, 122.43f, 17, 0, 6.05f },
	{ 1, 492.915, 121.37f, 16, 0, -0.99f }, { 1, 512.811, 117.15f, 17, 0, -4.35f },
	{ 1, 524.898, 123.00f, 17, 0, 4.09f }, { 1, 547.032, 116.89f, 16, 0, -5.51f },
	{ 1, 557.549, 122.15f, 17, 0, 5.50f }, { 1, 577.263, 119.70f, 16, 0, -2.68f },
	{ 1, 596.935, 116.60f, 17, 0, -3.82f }, { 1, 606.752, 123.29f, 16, 0, 5.89f },
	{ 1, 626.864, 119.75f, 17, 0, -2.69f }, { 1, 633.408, 129.66f, 16, 0, 9.30f },
	{ 1, 659.141, 120.13f, 17, 0, -7.40f }, { 1, 678.824, 117.36f, 16, 0, -3.56f },
	{ 1, 688.625, 124.13f, 17, 0, 6.17f }, { 1, 709.125, 120.10f, 16, 0, -3.79f },
	{ 1, 728.829, 116.90f, 17, 0, -3.97f }, { 1, 738.686, 123.57f, 16, 0, 5.70f },
	{ 1, 758.893, 119.93f, 17, 0, -2.97f }, { 1, 778.616, 116.71f, 16, 0, -4.16f },
	{ 1, 788.460, 123.44f, 17, 0, 6.52f }, { 1, 808.154, 120.25f, 17, 0, -2.64f },
	{ 1, 827.788, 117.19f, 16, 0, -3.74f }, { 1, 837.608, 123.86f, 17, 0, 5.97f },
	{ 1, 857.248, 120.79f, 16, 0, -2.14f }, { 1, 876.913, 117.64f, 17, 0, -3.28f },
	{ 1, 886.728, 124.32f, 16, 0, 6.44f }, { 1, 907.550, 120.07f, 17, 0, -3.85f },
	{ 1, 927.352, 116.82f, 16, 0, -4.12f }, { 1, 937.119, 123.50f, 17, 0, 6.64f },
	{ 1, 957.188, 119.99f, 16, 0, -2.90f }, { 1, 976.964, 116.78f, 17, 0, -4.14f },
	{ 1, 985.787, 124.48f, 16, 0, 7.56f }, { 1, 1007.109, 119.70f, 17, 0, -4.23f },
	{ 1, 1026.822, 116.50f, 16, 0, -3.41f }, { 1, 1036.682, 123.16f, 17, 0, 6.26f },
	{ 1, 1056.920, 119.48f, 17, 0, -3.45f }, { 1, 1076.618, 116.28f, 16, 0, -3.61f },
	{ 1, 1085.488, 124.01f, 17, 0, 7.05f }, { 1, 1106.349, 119.93f, 16, 0, -4.28f },
	{ 1, 1126.046, 116.56f, 17, 0, -3.45f }, { 1, 1135.909, 123.15f, 16, 0, 6.22f },
	{ 1, 1155.619, 119.93f, 17, 0, -2.96f }, { 1, 1175.298, 116.82f, 16, 0, -4.11f },
	{ 1, 1186.103, 120.59f, 17, 0, 5.62f }, { 1, 1199.521, 125.65f, 16, 0, 3.74f },
	{ 1, 1221.413, 120.27f, 17, 0, -4.62f }, { 1, 1241.100, 117.12f, 16, 0, -3.77f },
	{ 1, 1250.921, 123.86f, 17, 0, 5.94f }, { 1, 1273.153, 118.15f, 16, 0, -4.76f },
	{ 1, 1283.016, 124.83f, 17, 0, 5.90f }, { 1, 1304.386, 119.97f, 16, 0, -3.93f },
	{ 1, 1324.124, 116.76f, 17, 0, -4.14f }, { 1, 1333.960, 122.08f, 17, 0, 6.55f },
	{ 1, 1353.969, 120.03f, 16, 0, -2.93f }, { 1, 1373.614, 116.92f, 17, 0, -4.04f },
	{ 1, 1383.489, 123.64f, 16, 0, 6.61f }, { 1, 1403.839, 119.78f, 17, 0, -3.11f },
	{ 1, 1423.533, 116.65f, 16, 0, -3.27f }, { 1, 1433.358, 123.42f, 17, 0, 6.43f },
	{ 1, 1453.529, 119.70f, 16, 0, -3.21f }, { 1, 1464.807, 124.96f, 17, 0, 5.04f },
	{ 1, 1486.783, 118.51f, 16, 0, -5.40f }, { 1, 1498.259, 124.61f, 17, 0, 4.66f },
	{ 1, 1518.126, 121.23f, 16, 0, -2.68f }, { 1, 1530.318, 125.65f, 17, 0, 3.66f },
	{ 1, 1548.072, 125.32f, 17, 0, -0.56f }, { 1, 1567.993, 121.22f, 16, 0, -3.95f },
	{ 1, 1589.471, 115.30f, 17, 0, -5.90f }, { 1, 1600.979, 121.04f, 16, 0, 4.12f },
	{ 1, 1622.306, 116.24f, 17, 0, -4.68f }, { 1, 1633.181, 121.37f, 16, 0, 4.98f },
	{ 1, 1652.902, 118.72f, 17, 0, -2.21f }, { 1, 1672.759, 115.35f, 16, 0, -3.54f },
	{ 1, 1682.673, 122.07f, 17, 0, 6.08f }, { 1, 1702.453, 118.70f, 16, 0, -3.17f },
	{ 1, 1712.319, 125.38f, 17, 0, 6.50f }, { 1, 1734.292, 120.00f, 16, 0, -4.94f },
	{ 1, 1753.982, 116.81f, 17, 0, -4.10f }, { 1, 1763.841, 123.50f, 16, 0, 6.57f },
	{ 1, 1783.833, 119.99f, 17, 0, -2.89f }, { 1, 1804.471, 115.95f, 17, 0, -5.00f },
	{ 1, 1814.322, 122.58f, 16, 0, 5.68f }, { 1, 1834.020, 119.43f, 17, 0, -2.49f },
	{ 1, 1844.115, 125.90f, 16, 0, 5.95f }, { 1, 1857.694, 128.79f, 17, 0, 3.90f },
	{ 1, 1882.870, 120.25f, 16, 0, -7.75f }, { 1, 1903.188, 116.42f, 17, 0, -4.53f },
	{ 1, 1913.004, 123.07f, 16, 0, 6.18f }, { 1, 1932.743, 119.86f, 17, 0, -3.03f },
	{ 1, 1952.481, 116.67f, 16, 0, -3.23f }, { 1, 1962.297, 123.50f, 17, 0, 6.48f },
	{ 1, 1981.981, 120.24f, 16, 0, -2.67f }, { 1, 1993.311, 126.23f, 17, 0, 4.53f },
	{ 1, 2015.819, 119.49f, 16, 0, -5.45f }, { 1, 2035.679, 116.14f, 17, 0, -3.78f },
	{ 1, 2047.760, 120.63f, 16, 0, 3.67f }, { 1, 2065.513, 117.20f, 17, 0, -0.52f },
	{ 1, 2077.517, 123.95f, 17, 0, 4.01f }, { 1, 2097.631, 120.72f, 16, 0, -3.58f },
	{ 1, 2117.676, 117.10f, 17, 0, -3.85f }, { 1, 2125.640, 125.63f, 16, 0, 7.72f },
	{ 1, 2147.688, 120.16f, 17, 0, -4.80f }, { 1, 2167.343, 116.96f, 16, 0, -3.92f },
	{ 1, 2177.212, 123.67f, 17, 0, 5.74f }, { 1, 2199.525, 117.86f, 17, 0, -5.04f },
	{ 1, 2209.381, 122.53f, 16, 0, 6.63f }, { 1, 2230.498, 119.95f, 17, 0, -3.95f },
	{ 1, 2250.189, 117.17f, 16, 0, -4.11f }, { 1, 2261.329, 122.15f, 17, 0, 5.28f },
	{ 1, 2274.552, 125.53f, 16, 0, 3.59f }, { 1, 2294.444, 122.17f, 17, 0, -2.77f },
	{ 1, 2314.332, 117.13f, 16, 0, -4.13f }, { 1, 2326.389, 123.22f, 17, 0, 4.34f },
	{ 1, 2348.970, 117.19f, 16, 0, -5.71f }, { 1, 2358.836, 122.37f, 17, 0, 5.96f },
	{ 1, 2379.521, 119.77f, 16, 0, -3.20f }, { 1, 2399.172, 116.58f, 17, 0, -3.32f },
	{ 1, 2409.038, 123.22f, 16, 0, 6.34f }, { 1, 2428.843, 120.00f, 17, 0, -2.93f },
	{ 1, 2449.039, 116.28f, 16, 0, -4.60f }, { 1, 2458.911, 121.85f, 17, 0, 6.06f },
	{ 1, 2478.600, 120.05f, 17, 0, -3.10f }, { 1, 2498.274, 116.64f, 16, 0, -3.24f },
	{ 1, 2508.119, 122.91f, 17, 0, 6.44f }, { 1, 2527.889, 120.07f, 16, 0, -2.79f },
	{ 1, 2547.906, 116.64f, 17, 0, -4.28f }, { 1, 2557.758, 123.28f, 16, 0, 6.40f },
	{ 1, 2577.504, 120.12f, 17, 0, -2.82f }, { 1, 2584.354, 129.74f, 16, 0, 8.86f },
	{ 1, 2609.754, 120.26f, 17, 0, -7.51f }, { 1, 2629.428, 117.75f, 16, 0, -3.65f },
	{ 1, 2639.276, 124.41f, 17, 0, 6.03f }, { 1, 2659.524, 120.44f, 16, 0, -3.68f },
	{ 1, 2679.252, 117.51f, 17, 0, -3.88f }, { 1, 2689.107, 124.16f, 16, 0, 5.79f },
	{ 1, 2709.815, 120.02f, 17, 0, -3.38f }, { 1, 2729.547, 116.89f, 16, 0, -3.59f },
	{ 1, 2739.378, 123.46f, 17, 0, 6.11f }, { 1, 2759.967, 119.33f, 17, 0, -3.94f },
	{ 1, 2779.694, 116.28f, 16, 0, -4.14f }, { 1, 2789.547, 122.96f, 17, 0, 6.54f },
	{ 1, 2809.192, 119.85f, 16, 0, -2.58f }, { 1, 2828.973, 116.56f, 17, 0, -3.83f },
	{ 1, 2838.829, 123.27f, 16, 0, 5.85f }, { 1, 2858.690, 119.93f, 17, 0, -2.49f },
	{ 1, 2878.404, 116.78f, 16, 0, -3.67f }, { 1, 2888.259, 123.44f, 17, 0, 6.01f },
	{ 1, 2908.401, 119.79f, 16, 0, -3.60f }, { 1, 2928.109, 116.65f, 17, 0, -3.78f },
	{ 1, 2937.973, 123.28f, 16, 0, 5.88f }, { 1, 2957.832, 119.93f, 17, 0, -2.44f },
	{ 1, 2977.652, 116.66f, 16, 0, -3.73f }, { 1, 2987.634, 123.18f, 17, 0, 5.82f },
	{ 1, 3007.419, 120.01f, 17, 0, -2.44f }, { 1, 3027.092, 116.82f, 16, 0, -3.58f },
	{ 1, 3036.965, 123.44f, 17, 0, 6.08f }, { 1, 3057.205, 119.77f, 16, 0, -3.63f },
	{ 1, 3076.856, 116.68f, 17, 0, -3.75f }, { 1, 3086.556, 123.45f, 16, 0, 6.08f },
	{ 1, 3106.768, 119.77f, 17, 0, -3.60f }, { 1, 3126.463, 116.63f, 16, 0, -3.76f },
	{ 1, 3136.317, 123.29f, 17, 0, 5.91f }, { 1, 3156.187, 119.96f, 16, 0, -2.43f },
	{ 1, 3175.916, 116.78f, 17, 0, -3.62f }, { 1, 3185.779, 123.41f, 16, 0, 6.05f },
	{ 1, 3205.820, 119.95f, 17, 0, -3.47f }, { 1, 3217.184, 125.15f, 16, 0, 4.70f },
	{ 1, 3240.611, 118.23f, 17, 0, -6.19f }, { 1, 3253.395, 121.94f, 16, 0, 3.55f },
	{ 1, 3272.362, 119.47f, 17, 0, -1.88f }, { 1, 3283.514, 123.25f, 17, 0, 4.50f },
	{ 1, 3305.276, 119.66f, 16, 0, -4.74f }, { 1, 3315.952, 125.51f, 17, 0, 5.12f },
	{ 1, 3335.622, 122.35f, 16, 0, -3.02f }, { 1, 3355.312, 119.23f, 17, 0, -3.18f },
	{ 1, 3374.945, 116.13f, 16, 0, -3.28f }, { 1, 3384.767, 121.54f, 17, 0, 6.43f },
	{ 1, 3404.547, 119.60f, 16, 0, -2.82f }, { 1, 3424.313, 116.31f, 17, 0, -4.06f },
	{ 1, 3434.184, 121.93f, 16, 0, 6.60f }, { 1, 3453.830, 119.84f, 17, 0, -2.51f },
	{ 1, 3472.679, 117.57f, 16, 0, -2.83f }, { 1, 3483.702, 123.28f, 17, 0, 4.68f },
	{ 1, 3503.542, 119.74f, 16, 0, -2.63f }, { 1, 3523.206, 116.59f, 17, 0, -3.77f },
	{ 1, 3533.031, 122.52f, 17, 0, 5.94f }, { 1, 3552.851, 120.76f, 16, 0, -2.35f },
	{ 1, 3572.538, 116.85f, 17, 0, -3.51f }, { 1, 3582.859, 123.32f, 16, 0, 5.70f },
	{ 1, 3602.642, 119.84f, 17, 0, -2.55f }, { 1, 3622.348, 116.78f, 16, 0, -3.72f },
	{ 1, 3632.195, 123.33f, 17, 0, 5.96f }, { 1, 3651.845, 120.38f, 16, 0, -2.16f },
	{ 1, 3659.099, 129.52f, 17, 0, 9.12f }, { 1, 3684.815, 120.01f, 16, 0, -7.57f },
	{ 1, 3704.458, 117.22f, 17, 0, -3.68f }, { 1, 3714.321, 123.91f, 16, 0, 5.99f },
	{ 1, 3734.695, 120.31f, 17, 0, -2.85f }, { 1, 3754.391, 116.84f, 16, 0, -4.02f },
	{ 1, 3764.248, 123.52f, 17, 0, 6.65f }, { 1, 3784.620, 119.71f, 16, 0, -3.19f },
	{ 1, 3804.279, 116.63f, 17, 0, -3.31f }, { 1, 3814.112, 123.27f, 17, 0, 6.38f },
	{ 1, 3834.475, 119.42f, 16, 0, -3.45f }, { 1, 3854.159, 116.28f, 17, 0, -3.60f },
	{ 1, 3863.982, 123.08f, 16, 0, 6.10f }, { 1, 3883.687, 119.81f, 17, 0, -3.07f },
	{ 1, 3903.363, 116.71f, 16, 0, -3.22f }, { 1, 3913.170, 123.46f, 17, 0, 6.51f },
	{ 1, 3933.114, 120.02f, 16, 0, -2.90f }, { 1, 3953.034, 116.58f, 17, 0, -4.29f },
	{ 1, 3962.897, 123.28f, 16, 0, 6.37f }, { 1, 3982.657, 120.09f, 17, 0, -2.85f },
	{ 1, 4002.529, 116.68f, 16, 0, -4.19f }, { 1, 4011.516, 124.25f, 17, 0, 7.35f },
	{ 1, 4032.684, 119.64f, 16, 0, -4.29f }, { 1, 4052.336, 116.50f, 17, 0, -3.41f },
	{ 1, 4062.149, 123.20f, 16, 0, 6.31f }, { 1, 4081.905, 120.00f, 17, 0, -2.92f },
	{ 1, 4101.860, 116.55f, 17, 0, -4.34f }, { 1, 4111.097, 123.88f, 16, 0, 6.95f },
	{ 1, 4131.608, 119.91f, 17, 0, -3.03f }, { 1, 4151.251, 116.75f, 16, 0, -3.14f },
	{ 1, 4161.118, 121.39f, 17, 0, 6.52f }, { 1, 4180.073, 120.99f, 16, 0, -1.90f },
	{ 1, 4200.833, 116.79f, 17, 0, -5.13f }, { 1, 4210.677, 121.65f, 16, 0, 6.55f },
	{ 1, 4230.342, 120.41f, 17, 0, -2.58f }, { 1, 4249.991, 117.17f, 16, 0, -3.69f },
	{ 1, 4259.783, 123.98f, 17, 0, 6.05f }, { 1, 4280.432, 119.81f, 16, 0, -4.07f },
	{ 1, 4291.619, 125.14f, 17, 0, 5.27f }, { 1, 4314.298, 117.50f, 16, 0, -5.88f },
	{ 1, 4325.987, 123.83f, 17, 0, 3.96f }, { 1, 4349.029, 117.34f, 17, 0, -5.55f },
	{ 1, 4358.797, 124.03f, 16, 0, 6.22f }, { 1, 4380.351, 119.11f, 17, 0, -4.81f },
	{ 1, 4391.357, 124.65f, 16, 0, 4.72f }, { 1, 4414.332, 118.19f, 17, 0, -5.73f },
	{ 1, 4426.506, 122.56f, 16, 0, 3.63f }, { 1, 4448.663, 116.94f, 17, 0, -4.99f },
	{ 1, 4459.066, 122.04f, 16, 0, 5.13f }, { 1, 4479.170, 119.45f, 17, 0, -3.44f },
	{ 1, 4498.864, 116.33f, 16, 0, -3.60f }, { 1, 4508.691, 123.02f, 17, 0, 6.10f },
	{ 1, 4528.389, 119.83f, 16, 0, -3.07f }, { 1, 4548.071, 116.70f, 17, 0, -3.22f },
	{ 1, 4557.884, 123.39f, 16, 0, 6.50f }, { 1, 4577.884, 119.94f, 17, 0, -2.97f },
	{ 1, 4597.617, 116.76f, 16, 0, -4.17f }, { 1, 4607.432, 122.98f, 17, 0, 6.55f },
	{ 1, 4627.130, 120.28f, 17, 0, -2.62f }, { 1, 4647.229, 116.74f, 16, 0, -4.19f },
	{ 1, 4657.097, 123.33f, 17, 0, 6.47f }, { 1, 4676.869, 120.18f, 16, 0, -2.77f },
	{ 1, 4696.958, 116.55f, 17, 0, -4.33f }, { 1, 4707.315, 122.58f, 16, 0, 5.85f },
	{ 1, 4726.955, 119.66f, 17, 0, -2.26f }, { 1, 4746.825, 116.35f, 16, 0, -3.60f },
	{ 1, 4756.688, 122.99f, 17, 0, 6.10f }, { 1, 4776.352, 119.89f, 16, 0, -3.03f },
	{ 1, 4786.944, 125.76f, 17, 0, 5.91f }, { 1, 4809.143, 120.21f, 16, 0, -4.76f },
	{ 1, 4828.822, 117.00f, 17, 0, -3.91f }, { 1, 4838.686, 123.64f, 16, 0, 5.75f },
	{ 1, 4858.694, 120.17f, 17, 0, -2.72f }, { 1, 4878.372, 117.00f, 17, 0, -3.87f },
	{ 1, 4887.321, 124.86f, 16, 0, 6.71f }, { 1, 4908.825, 119.61f, 17, 0, -4.27f },
	{ 1, 4928.462, 116.55f, 16, 0, -3.37f }, { 1, 4938.316, 123.22f, 17, 0, 6.30f },
};

static void ClientView_CheckClock(const char *name, const clock_row_t *rows, int count, double maxmean, double maxworst, double minstock)
{
	cv_clock_t c;
	Q_memset(&c, 0, sizeof(c));

	int ack = 1000, n = 0;
	double sum = 0.0, worst = 0.0, stock = 0.0;
	for (int i = 0; i < count; i++)
	{
		if (rows[i].moved)
			ack++;

		double e = CV_ClockElapsed(&c, ack, rows[i].acktime / 1000.0, rows[i].rtt / 1000.0, rows[i].newest, rows[i].older);

		// judge once the round trip window is full
		if (i < CV_RTT_WINDOW || rows[i].actual < -999.0f)
			continue;

		double err = fabs(1000.0 * e - rows[i].actual);
		sum += err;
		stock += fabs(rows[i].actual);
		if (err > worst)
			worst = err;
		n++;
	}

	char msg[128];
	Q_snprintf(msg, sizeof(msg), "%s: enough rendered samples", name);
	CHECK(msg, n > 50);
	Q_snprintf(msg, sizeof(msg), "%s: mean error %.2f ms", name, sum / n);
	CHECK(msg, sum / n < maxmean);
	Q_snprintf(msg, sizeof(msg), "%s: worst error %.2f ms", name, worst);
	CHECK(msg, worst < maxworst);
	Q_snprintf(msg, sizeof(msg), "%s: the ack alone is %.2f ms off", name, stock / n);
	CHECK(msg, stock / n > minstock);
}

TEST(ClockElapsedRecorded, ClientView, 1000) {
	// fps 500: five commands per packet, placed by the round trip
	ClientView_CheckClock("fps 500", g_clockFps500, ARRAYSIZE(g_clockFps500), 1.0, 4.0, 5.0);
	// fps 60: one command per packet, the client clock replayed
	ClientView_CheckClock("fps 60", g_clockFps60, ARRAYSIZE(g_clockFps60), 1.0, 1.5, 4.0);
}

TEST(PureOrigin, ClientView, 1000) {
	entity_state_t s0, s1, s2;
	Q_memset(&s0, 0, sizeof(s0));
	Q_memset(&s1, 0, sizeof(s1));
	Q_memset(&s2, 0, sizeof(s2));
	s0.origin[0] = 20.0f;
	s1.origin[0] = 10.0f;
	s2.origin[0] = 0.0f;
	s0.angles[1] = 170.0f;
	s1.angles[1] = -170.0f;

	// newest first
	double at[3] = { 1.02, 1.01, 1.00 };
	entity_state_t *st[3] = { &s0, &s1, &s2 };
	vec3_t org, ang;

	CV_PureOrigin(at, st, 3, 1.015, org, ang);
	DOUBLES_EQUAL("lerp inside the newest pair", 15.0, org[0], 0.001);
	DOUBLES_EQUAL("angles lerp across the wrap", 180.0, fabs(ang[1]), 0.01);

	CV_PureOrigin(at, st, 3, 1.005, org, ang);
	DOUBLES_EQUAL("lerp inside an older pair", 5.0, org[0], 0.001);

	CV_PureOrigin(at, st, 3, 1.03, org, ang);
	DOUBLES_EQUAL("players extrapolate at most 20%", 22.0, org[0], 0.001);

	CV_PureOrigin(at, st, 3, 0.99, org, ang);
	DOUBLES_EQUAL("older than the history holds the second entry", 10.0, org[0], 0.001);
}

// Gait sequences of the CS player models used below
static studiohdr_t *ClientView_GaitModel()
{
	static byte buffer[sizeof(studiohdr_t) + 7 * sizeof(mstudioseqdesc_t)];
	Q_memset(buffer, 0, sizeof(buffer));

	studiohdr_t *phdr = (studiohdr_t *)buffer;
	phdr->numseq = 7;
	phdr->seqindex = sizeof(studiohdr_t);

	mstudioseqdesc_t *seq = (mstudioseqdesc_t *)(buffer + phdr->seqindex);
	seq[1].fps = 15.0f; seq[1].numframes = 61;		// idle1
	seq[3].fps = 30.0f; seq[3].numframes = 33;		// walk
	seq[3].linearmovement[0] = 64.0f;
	seq[6].fps = 36.0f; seq[6].numframes = 61;		// jump

	return phdr;
}

struct clientview_gait_row_t
{
	float origin[3];
	float yaw;
	int gaitsequence;
	float dt;
	float gaityaw;		// what the stock 8684 client had after this render
	float gaitframe;
};

static void ClientView_CheckGait(cv_victim_t *v, const clientview_gait_row_t *rows, int count)
{
	studiohdr_t *phdr = ClientView_GaitModel();

	for (int i = 0; i < count; i++)
	{
		const clientview_gait_row_t &row = rows[i];
		VectorCopy(row.origin, v->origin);
		v->angles[0] = 0.0f;
		v->angles[1] = row.yaw;
		v->angles[2] = 0.0f;
		v->sequence = 75;
		v->gaitsequence = row.gaitsequence;

		CV_ProcessGait(v, phdr, row.dt);

		char msg[64];
		Q_snprintf(msg, sizeof(msg), "row %d gaityaw", i);
		DOUBLES_EQUAL(msg, row.gaityaw, v->gaityaw, 0.03);
		Q_snprintf(msg, sizeof(msg), "row %d gaitframe", i);
		DOUBLES_EQUAL(msg, row.gaitframe, v->gaitframe, 0.03);
	}
}

static void ClientView_InitVictim(cv_victim_t *v, float gaityaw, float gaitframe, float x, float y, float z, int gait)
{
	Q_memset(v, 0, sizeof(*v));
	v->gaitvalid = TRUE;
	v->gaityaw = gaityaw;
	v->gaitframe = gaitframe;
	v->prevgaitorigin[0] = x;
	v->prevgaitorigin[1] = y;
	v->prevgaitorigin[2] = z;
	v->lastgaitsequence = gait;
	v->yawmodifier = 3.5f;
	v->framerate = 1.0f;
}

// Recorded from a stock client watching a strafing player jump: the yaw
// steps when it stops for a frame (truncated drift), the jump gait restarts
// from frame 0 after the frame's advance.
TEST(GaitJumpRestart, ClientView, 1000) {
	static const clientview_gait_row_t rows[] = {
		{ { -525.650f, 1013.688f, 548.031f }, -61.25f, 3, 0.009953f, 29.24f, 29.17f },
		{ { -525.200f, 1013.938f, 548.031f }, -61.25f, 3, 0.009950f, 29.05f, 29.44f },
		{ { -525.200f, 1013.938f, 548.031f }, -61.25f, 3, 0.009952f, 25.91f, 29.44f },
		{ { -524.075f, 1014.550f, 550.581f }, -61.25f, 6, 0.009954f, 28.57f, 0.00f },
		{ { -524.075f, 1014.550f, 550.581f }, -61.25f, 6, 0.009951f, 25.44f, 0.36f },
		{ { -523.794f, 1014.700f, 555.556f }, -61.25f, 6, 0.009954f, 28.08f, 0.72f },
		{ { -523.794f, 1014.700f, 555.556f }, -61.25f, 6, 0.009952f, 24.96f, 1.07f },
	};

	cv_victim_t v;
	ClientView_InitVictim(&v, 24.70f, 28.49f, -526.800f, 1013.044f, 548.031f, 3);
	ClientView_CheckGait(&v, rows, ARRAYSIZE(rows));
}

// The client starts every player zeroed (CL_ClearState): the first drawn
// frame sees a jump from the world origin and takes that direction as the
// gait yaw, flipped by 180 when the body faces more than 120 away from it.
// Stock clients drawing standing bots, recorded (no rounding by the client):
// spawn (775, -1001) body yaw 2.0 -> -52.25, body yaw 118.4 -> -232.25.
TEST(GaitFirstSight, ClientView, 1000) {
	studiohdr_t *phdr = ClientView_GaitModel();
	cv_victim_t v;

	for (int flip = 0; flip < 2; flip++)
	{
		Q_memset(&v, 0, sizeof(v));
		v.framerate = 1.0f;
		v.origin[0] = 775.0f;
		v.origin[1] = -1001.0f;
		v.origin[2] = 548.0f;
		v.angles[1] = flip ? 118.4f : 2.0f;
		v.sequence = 75;
		v.gaitsequence = 1;

		// a standing player keeps the yaw: the zero yaw modifier blocks the drift
		for (int frame = 0; frame < 50; frame++)
			CV_ProcessGait(&v, phdr, 0.01f);

		DOUBLES_EQUAL(flip ? "flipped first-frame gait yaw" : "first-frame gait yaw", flip ? -232.25 : -52.25, v.gaityaw, 0.01);
		DOUBLES_EQUAL("idle gait advances by fps", 50 * 0.15, v.gaitframe, 0.01);
	}

	// facing within 5 degrees of the legs arms the slow drift
	Q_memset(&v, 0, sizeof(v));
	v.framerate = 1.0f;
	v.origin[0] = 1000.0f;
	v.origin[1] = 1000.0f;
	v.angles[1] = 48.0f;
	v.sequence = 75;
	v.gaitsequence = 1;
	CV_ProcessGait(&v, phdr, 0.01f);
	DOUBLES_EQUAL("first frame", 45.0, v.gaityaw, 0.01);
	CV_ProcessGait(&v, phdr, 0.01f);
	DOUBLES_EQUAL("modifier armed", 0.05, v.yawmodifier, 0.0001);
}

// A standing player 28 degrees off its body yaw: the per-frame step stays
// under one degree, so the client never turns the legs.
TEST(GaitIdleYawStuck, ClientView, 1000) {
	static const clientview_gait_row_t rows[] = {
		{ { -549.781f, 1000.438f, 548.031f }, -61.25f, 1, 0.009951f, -89.42f, 43.61f },
		{ { -549.781f, 1000.438f, 548.031f }, -61.25f, 1, 0.009953f, -89.42f, 43.76f },
		{ { -549.781f, 1000.438f, 548.031f }, -61.25f, 1, 0.009953f, -89.42f, 43.91f },
		{ { -549.781f, 1000.438f, 548.031f }, -61.25f, 1, 0.009952f, -89.42f, 44.06f },
	};

	cv_victim_t v;
	ClientView_InitVictim(&v, -89.42f, 43.46f, -549.781f, 1000.438f, 548.031f, 1);
	ClientView_CheckGait(&v, rows, ARRAYSIZE(rows));
}
