// Firmware de référence pour l'appareil Miwer (Arduino Leonardo / Pro Micro / Uno / Nano...).
// 5 potentiomètres sur A0..A4, 5 boutons mute sur D2..D6 (vers GND, pull-up interne).
//
// Envoie toutes les 20 ms : v1|v2|v3|v4|v5|m1|m2|m3|m4|m5\n
// v = 0..1023, m = 1 si le bouton est appuyé (mode « toggle » côté PC).

const int kNumChannels = 5;
const uint8_t kSliderPins[kNumChannels] = {A0, A1, A2, A3, A4};
const uint8_t kButtonPins[kNumChannels] = {2, 3, 4, 5, 6};

const unsigned long kSendIntervalMs = 20;
const unsigned long kDebounceMs = 25;

bool buttonState[kNumChannels];
bool lastReading[kNumChannels];
unsigned long lastChange[kNumChannels];

void setup()
{
    Serial.begin(115200);
    for (int i = 0; i < kNumChannels; ++i) {
        pinMode(kButtonPins[i], INPUT_PULLUP);
        buttonState[i] = false;
        lastReading[i] = false;
        lastChange[i] = 0;
    }
}

int readSlider(uint8_t pin)
{
    // Moyenne de 4 lectures pour réduire le bruit
    long sum = 0;
    for (int i = 0; i < 4; ++i)
        sum += analogRead(pin);
    return sum / 4;
}

void updateButtons()
{
    const unsigned long now = millis();
    for (int i = 0; i < kNumChannels; ++i) {
        const bool reading = digitalRead(kButtonPins[i]) == LOW;
        if (reading != lastReading[i]) {
            lastReading[i] = reading;
            lastChange[i] = now;
        }
        if (now - lastChange[i] >= kDebounceMs)
            buttonState[i] = reading;
    }
}

void loop()
{
    static unsigned long lastSend = 0;

    updateButtons();

    if (millis() - lastSend < kSendIntervalMs)
        return;
    lastSend = millis();

    for (int i = 0; i < kNumChannels; ++i) {
        Serial.print(readSlider(kSliderPins[i]));
        Serial.print('|');
    }
    for (int i = 0; i < kNumChannels; ++i) {
        Serial.print(buttonState[i] ? 1 : 0);
        Serial.print(i < kNumChannels - 1 ? '|' : '\n');
    }
}
