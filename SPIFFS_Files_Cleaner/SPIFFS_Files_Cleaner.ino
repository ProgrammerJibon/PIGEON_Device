#include <SPIFFS.h>

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (!SPIFFS.begin(false)) {
        Serial.println("SPIFFS mount failed!");
        return;
    }

    Serial.println("SPIFFS mounted.");
    Serial.println("Type 'y' and press Enter to delete all files.");

    while (true) {
        if (Serial.available()) {
            char c = Serial.read();

            if (c == 'y' || c == 'Y') {
                break;
            }
        }
    }

    Serial.println("Starting SPIFFS file deletion...");

    String files[100];
    int fileCount = 0;

    File root = SPIFFS.open("/");

    if (!root) {
        Serial.println("Failed to open SPIFFS root!");
        return;
    }

    File file = root.openNextFile();

    while (file && fileCount < 100) {
        if (!file.isDirectory()) {
            String name = file.name();

            if (!name.startsWith("/")) {
                name = "/" + name;
            }

            files[fileCount++] = name;
        }

        file.close();
        file = root.openNextFile();
    }

    root.close();

    Serial.print("Found ");
    Serial.print(fileCount);
    Serial.println(" files.");

    for (int i = 0; i < fileCount; i++) {
        Serial.print("Deleting: ");
        Serial.println(files[i]);

        bool result = SPIFFS.remove(files[i]);

        if (result) {
            Serial.println("Deleted successfully.");
        } else {
            Serial.println("Failed to delete.");
        }
    }

    Serial.println("All SPIFFS files processed.");
}

void loop() {
}