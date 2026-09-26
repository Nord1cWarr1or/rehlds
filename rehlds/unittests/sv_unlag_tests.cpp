#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"
#include "sv_user.h"

const float UNLAG_LATENCY_EPS = 0.0001f;

TEST(UnlagLatency_EmptyInput, SvUnlag, 1000) {
	DOUBLES_EQUAL("no samples", 0.0f, SV_ComputeUnlagLatency(NULL, 0), UNLAG_LATENCY_EPS);
	DOUBLES_EQUAL("null pointer", 0.0f, SV_ComputeUnlagLatency(NULL, 5), UNLAG_LATENCY_EPS);
}

TEST(UnlagLatency_NoValidSamples, SvUnlag, 1001) {
	float samples[] = { 0.0f, -0.5f, 0.0f };
	DOUBLES_EQUAL("all invalid", 0.0f, SV_ComputeUnlagLatency(samples, 3), UNLAG_LATENCY_EPS);
}

TEST(UnlagLatency_SingleSample, SvUnlag, 1002) {
	float samples[] = { 0.123f };
	DOUBLES_EQUAL("single sample returned as is", 0.123f, SV_ComputeUnlagLatency(samples, 1), UNLAG_LATENCY_EPS);
}

TEST(UnlagLatency_MedianOddCount, SvUnlag, 1003) {
	float samples[] = { 0.05f, 0.2f, 0.07f };
	DOUBLES_EQUAL("odd count median", 0.07f, SV_ComputeUnlagLatency(samples, 3), UNLAG_LATENCY_EPS);
}

TEST(UnlagLatency_MedianEvenCount, SvUnlag, 1004) {
	float samples[] = { 0.05f, 0.2f, 0.07f, 0.09f };
	DOUBLES_EQUAL("even count median", 0.08f, SV_ComputeUnlagLatency(samples, 4), UNLAG_LATENCY_EPS);
}

TEST(UnlagLatency_OutlierRobustness, SvUnlag, 1005) {
	float samples[] = { 0.05f, 0.05f, 0.5f, 0.05f, 0.05f, 0.05f, 0.05f, 0.05f };
	DOUBLES_EQUAL("single spike does not move the median", 0.05f, SV_ComputeUnlagLatency(samples, 8), UNLAG_LATENCY_EPS);
}

TEST(UnlagLatency_InvalidSamplesSkipped, SvUnlag, 1006) {
	float samples[] = { 0.0f, 0.1f, -1.0f, 0.3f };
	DOUBLES_EQUAL("zeros and negatives skipped", 0.2f, SV_ComputeUnlagLatency(samples, 4), UNLAG_LATENCY_EPS);
}

TEST(UnlagLatency_InputCapped, SvUnlag, 1007) {
	float samples[20];
	for (int i = 0; i < 20; i++)
		samples[i] = 0.01f * (i + 1);

	DOUBLES_EQUAL("input capped at MAX_UNLAG_SAMPLES", 0.085f, SV_ComputeUnlagLatency(samples, 20), UNLAG_LATENCY_EPS);
}
