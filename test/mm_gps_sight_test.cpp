// Host-side unit tests for Mega-Maid GPS sight dedup (haversine threshold).
// Build: c++ -std=c++17 -Wall -Wextra -o test/mm_gps_sight_test test/mm_gps_sight_test.cpp && ./test/mm_gps_sight_test
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr float MM_GPS_SIGHT_MIN_M = 50.0f;
static constexpr size_t MM_SERIALIZE_LINE_BYTES = 3072;

static float mmHaversineM(float lat1, float lon1, float lat2, float lon2) {
    const float kRad = 0.01745329252f;
    const float kEarthR = 6371000.0f;
    float dLat = (lat2 - lat1) * kRad;
    float dLon = (lon2 - lon1) * kRad;
    float rLat1 = lat1 * kRad;
    float rLat2 = lat2 * kRad;
    float a = sinf(dLat * 0.5f) * sinf(dLat * 0.5f)
            + cosf(rLat1) * cosf(rLat2) * sinf(dLon * 0.5f) * sinf(dLon * 0.5f);
    return kEarthR * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

static bool mmGpsTooClose(float lat, float lon,
                          const float* sightLats, const float* sightLons, int sightCount) {
    for (int i = 0; i < sightCount; i++) {
        if (mmHaversineM(lat, lon, sightLats[i], sightLons[i]) < MM_GPS_SIGHT_MIN_M) {
            return true;
        }
    }
    return false;
}

static int g_failures = 0;

static void expectTrue(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static void expectNear(float a, float b, float eps, const char* msg) {
    if (fabsf(a - b) > eps) {
        fprintf(stderr, "FAIL: %s (got %.2f expected %.2f)\n", msg, a, b);
        g_failures++;
    }
}

int main() {
    // Same point — zero metres
    expectNear(mmHaversineM(45.0f, -93.0f, 45.0f, -93.0f), 0.0f, 0.01f,
               "identical coordinates");

    // ~111 km per degree latitude at equator; 0.001 deg ~ 111 m
    float dist = mmHaversineM(45.0f, -93.0f, 45.001f, -93.0f);
    expectTrue(dist > 100.0f && dist < 120.0f, "0.001 deg latitude ~111 m");

    // ~40 m north from 45N (1 deg lat ~ 111 km)
    float latNear = 45.0f + (40.0f / 111000.0f);
    expectTrue(mmHaversineM(45.0f, -93.0f, latNear, -93.0f) < MM_GPS_SIGHT_MIN_M,
               "40 m offset should be within dedup threshold");

    float latFar = 45.0f + (60.0f / 111000.0f);
    expectTrue(mmHaversineM(45.0f, -93.0f, latFar, -93.0f) >= MM_GPS_SIGHT_MIN_M,
               "60 m offset should exceed dedup threshold");

    // Dedup against existing sights
    float lats[] = {45.0f, 45.01f};
    float lons[] = {-93.0f, -93.0f};
    expectTrue(!mmGpsTooClose(45.01f, -93.0f, lats, lons, 1), "second city block is new sight");
    expectTrue(mmGpsTooClose(45.0001f, -93.0f, lats, lons, 1), "jitter near first sight is deduped");

    // Worst-case JSON payload size estimate (8 gps_sights, long SSID, 3 probed SSIDs).
    char line[MM_SERIALIZE_LINE_BYTES];
    char gpsBuf[1536] = "";
    size_t gpsOff = 0;
    gpsOff += (size_t)snprintf(gpsBuf + gpsOff, sizeof(gpsBuf) - gpsOff, ",\"gps_sights\":[");
    for (int i = 0; i < 8; i++) {
        if (i > 0) {
            gpsBuf[gpsOff++] = ',';
            gpsBuf[gpsOff] = '\0';
        }
        gpsOff += (size_t)snprintf(gpsBuf + gpsOff, sizeof(gpsBuf) - gpsOff,
                                   "{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f,\"t\":%u}",
                                   45.12345678 + i * 0.001, -93.12345678 - i * 0.001,
                                   12.5f, 1700000000u + (unsigned)i);
    }
    gpsOff += (size_t)snprintf(gpsBuf + gpsOff, sizeof(gpsBuf) - gpsOff, "]");
    const char* wifiBuf =
        ",\"ssid\":\"VeryLongNetworkNameForSerializeTest\""
        ",\"probed_ssids\":[\"ProbeA\",\"ProbeB\",\"ProbeC\"]";
    int n = snprintf(line, sizeof(line),
                     "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"name\":\"LongVendorNameExample\","
                     "\"rssi\":-55,\"method\":\"wifi_probe\","
                     "\"first\":1700000000000,\"last\":1700003600000,\"count\":42,"
                     "\"channel\":6%s%s,"
                     "\"gps\":{\"lat\":45.12345678,\"lon\":-93.12345678,\"acc\":8.5}}",
                     wifiBuf, gpsBuf);
    expectTrue(n > 0 && (size_t)n < sizeof(line),
               "worst-case detection JSON fits MM_SERIALIZE_LINE_BYTES");

    if (g_failures == 0) {
        printf("PASS mm_gps_sight_test (%d checks)\n", 7);
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
