// One-shot SPIFFS export for Mega Maid / Flock-You session recovery (USB serial).
#include <Arduino.h>
#include <SPIFFS.h>

static const char* FILES[] = {
    "/megamaid_session.json",
    "/megamaid_prev_session.json",
    "/megamaid_session.tmp",
    "/session.json",
    "/prev_session.json",
    "/session.tmp",
};

static void listAll() {
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while (file) {
        Serial.printf("LIST:%s %u\n", file.name(), (unsigned)file.size());
        file = root.openNextFile();
    }
}

static void dumpFile(const char* path) {
    Serial.printf("===FILE:%s===\n", path);
    if (!SPIFFS.exists(path)) {
        Serial.println("===MISSING===");
        return;
    }
    File f = SPIFFS.open(path, "r");
    if (!f) {
        Serial.println("===OPEN_FAIL===");
        return;
    }
    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        Serial.write(buf, n);
        delay(1);
    }
    f.close();
    Serial.println();
    Serial.println("===END===");
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("===SPIFFS_DUMP_START===");
    if (!SPIFFS.begin(false)) {
        Serial.println("===SPIFFS_FAIL===");
        return;
    }
    listAll();
    for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
        dumpFile(FILES[i]);
    }
    Serial.println("===SPIFFS_DUMP_DONE===");
}

void loop() {}
