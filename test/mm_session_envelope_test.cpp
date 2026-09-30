// Host-side unit tests for Mega-Maid session envelope version validation.
// Build: c++ -std=c++17 -Wall -Wextra -o test/mm_session_envelope_test test/mm_session_envelope_test.cpp && ./test/mm_session_envelope_test
#include <cstdio>
#include <cstring>

static int g_failures = 0;

static void expectTrue(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

// Mirrors mmValidateSessionFile version gate (megamaid.cpp).
static bool mmEnvelopeVersionOk(const char* hdrJson) {
    int ver = 0;
    const char* vp = strstr(hdrJson, "\"v\":");
    if (!vp) {
        return false;
    }
    vp += 4;
    while (*vp == ' ') {
        vp++;
    }
    if (sscanf(vp, "%d", &ver) != 1) {
        return false;
    }
    return ver >= 1 && ver <= 2;
}

int main() {
    expectTrue(mmEnvelopeVersionOk("{\"v\":1,\"count\":10,\"bytes\":100,\"crc\":\"0xDEADBEEF\"}"),
               "v1 envelope accepted");
    expectTrue(mmEnvelopeVersionOk("{\"v\":2,\"count\":10,\"bytes\":100,\"crc\":\"0xDEADBEEF\"}"),
               "v2 envelope accepted");
    expectTrue(!mmEnvelopeVersionOk("{\"v\":0,\"count\":10,\"bytes\":100,\"crc\":\"0xDEADBEEF\"}"),
               "v0 envelope rejected");
    expectTrue(!mmEnvelopeVersionOk("{\"v\":3,\"count\":10,\"bytes\":100,\"crc\":\"0xDEADBEEF\"}"),
               "v3 envelope rejected");
    expectTrue(!mmEnvelopeVersionOk("{\"count\":10,\"bytes\":100,\"crc\":\"0xDEADBEEF\"}"),
               "missing v rejected");

    if (g_failures == 0) {
        printf("PASS mm_session_envelope_test (%d checks)\n", 5);
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
