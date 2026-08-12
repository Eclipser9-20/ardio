// Blink, as an Arduino sketch.
//
// Compiled entirely by ardio: sketch preprocessing, C++ front-end, AVR code
// generation, assembly and Intel HEX, with the pin and delay routines coming
// from ardio's own runtime.
//
//     ardio push examples/blink.ino
//
// The runtime entry points are declared here rather than pulled in from a
// header, because ardio's compiler does not process #include yet.

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
void delay(int ms);

int led = 13;

void setup() {
    pinMode(led, 1);            // 1 = OUTPUT
}

void loop() {
    digitalWrite(led, 1);       // on
    delay(500);
    digitalWrite(led, 0);       // off
    delay(500);
}
