// IR Turret -- a pan/tilt/roll dart turret driven by an infrared remote.
//
// A rewrite of the CrunchLabs Hack Pack "IR Turret" sketch for ardio's own
// C++ compiler and runtime. Same behaviour, same remote codes, same servo
// geometry; the libraries are gone because ardio has no linker and no library
// manager yet, so everything the sketch needs is written out in the sketch.
//
//     ardio build examples/ir_turret/ir_turret.ino
//     ardio push  examples/ir_turret/ir_turret.ino
//
// See README.md next to this file for the full list of differences from the
// original and the ardio limitations behind each one.
//
// The runtime entry points are declared here rather than pulled in from a
// header, because ardio's compiler does not process #include yet.

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);
void delay(int ms);
void delayMicroseconds(int us);

// ----------------------------------------------------------------- pins ----

int YAW_PIN = 10;       // continuous rotation servo, spins the base
int PITCH_PIN = 11;     // positional servo, up/down tilt
int ROLL_PIN = 12;      // continuous rotation servo, spins the barrel to fire
int IR_PIN = 9;         // infrared receiver output, idles high, active low

int INPUT_MODE = 0;
int OUTPUT_MODE = 1;

// --------------------------------------------------------- remote codes ----
//
// NEC command byte for each button on the bundled remote. To add a button on
// another NEC remote, read the command byte it sends and add a case to
// handleCommand below.

int CMD_LEFT = 0x08;
int CMD_RIGHT = 0x5A;
int CMD_UP = 0x52;
int CMD_DOWN = 0x18;
int CMD_OK = 0x1C;
int CMD_STAR = 0x16;

// --------------------------------------------------------- servo params ----

int yawServoVal = 90;
int pitchServoVal = 100;
int rollServoVal = 90;

int pitchMoveSpeed = 8;   // degrees added to pitch per step, try 3..10
int yawMoveSpeed = 90;    // offset from yawStopSpeed, try 10..90
int yawStopSpeed = 90;    // value that stops the yaw motor, keep at 90
int rollMoveSpeed = 90;   // offset from rollStopSpeed, keep at 90
int rollStopSpeed = 90;   // value that stops the roll motor, keep at 90

int yawPrecision = 150;   // ms the yaw motor stays at speed per step
int rollPrecision = 158;  // ms of roll travel per dart, about 1/6 turn

int pitchMax = 175;       // upper travel limit, keeps the head off the frame
int pitchMin = 10;        // lower travel limit

// ---------------------------------------------------------------- servo ----
//
// One 50 Hz frame: a high pulse of 1000..2000 us, then the rest of the 20 ms
// period low.

void servoPulse(int pin, int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    // 1000 + angle * 1000 / 180 microseconds, reduced to * 50 / 9 so the
    // intermediate stays under 9000 and inside a signed 16-bit int.
    int width = 1000 + angle * 50 / 9;
    digitalWrite(pin, 1);
    delayMicroseconds(width);
    digitalWrite(pin, 0);
}

// Holds a servo at an angle for a span of milliseconds by refreshing it every
// frame. Continuous rotation servos only turn while they are being refreshed,
// so every timed movement in this sketch goes through here.
void servoHold(int pin, int angle, int ms) {
    int elapsed = 0;
    while (elapsed < ms) {
        servoPulse(pin, angle);
        delay(18);
        elapsed = elapsed + 20;
    }
}

// ------------------------------------------------------------------- IR ----
//
// A polling NEC receiver. The demodulated output of the receiver module idles
// high and pulls low for each burst. A frame is a 9 ms low leader, a 4.5 ms
// high space, then 32 bits, each a 562 us low burst followed by a space of
// 562 us for a zero or 1687 us for a one. The 32 bits are address, inverted
// address, command, inverted command, each sent least significant bit first.
//
// Widths are measured by counting 10 us polling steps rather than with a timer,
// because ardio has no timer runtime yet. The tolerances below are wide enough
// to absorb the resulting jitter.

int irCommand = 0;      // command byte of the last decoded frame

// Counts 10 us steps while the pin stays at level. Returns the count, or
// limit when it never changed.
int measure(int pin, int level, int limit) {
    int count = 0;
    while (count < limit) {
        if (digitalRead(pin) != level) return count;
        delayMicroseconds(10);
        count = count + 1;
    }
    return count;
}

// Reads one NEC bit: a 562 us low burst, then a space of 562 us for a zero or
// 1687 us for a one. Returns 1 for a one and 0 for anything else, including a
// timeout -- a truncated frame fails the complement check in ir_decode, so it
// costs nothing to report it as a zero here.
//
// This is a separate function only because ardio's conditional branches have a
// +-63 word reach and no long-branch expansion, so anything that has to sit
// inside a loop body must stay very small.
int ir_bit(int pin) {
    measure(pin, 0, 200);
    int gap = measure(pin, 1, 300);
    if (gap > 100) return 1;
    return 0;
}

// Reads eight bits, least significant first, into a byte.
int ir_byte(int pin) {
    // The loop counts by doubling weight rather than carrying a separate index,
    // which keeps the body inside the branch reach described above.
    int value = 0;
    for (int weight = 1; weight < 256; weight = weight + weight)
        if (ir_bit(pin) > 0) value = value + weight;
    return value;
}

void ir_begin(int pin) {
    pinMode(pin, INPUT_MODE);
}

// True when the receiver is pulling the line low, i.e. a frame may be starting.
int ir_available(int pin) {
    if (digitalRead(pin) == 0) return 1;
    return 0;
}

// Tries to decode one NEC frame that has just started. Returns 1 on success and
// leaves the command byte in irCommand.
int ir_decode(int pin) {
    // 9 ms leader: 900 steps, accept 600..1200.
    int leader = measure(pin, 0, 1400);
    if (leader < 600) return 0;
    if (leader > 1200) return 0;

    // 4.5 ms space: 450 steps. A repeat frame has a 2.25 ms space instead and
    // carries no command, so it is rejected here.
    int space = measure(pin, 1, 700);
    if (space < 300) return 0;
    if (space > 600) return 0;

    // The 32 bits are consumed a byte at a time rather than buffered, because
    // ardio's frame pointer only reaches 62 bytes of locals and an int[32]
    // alone is 64. The two address bytes are read and discarded.
    ir_byte(pin);                   // address
    ir_byte(pin);                   // inverted address
    int command = ir_byte(pin);
    int inverted = ir_byte(pin);

    // The complement byte is the frame's only integrity check.
    if ((command + inverted) != 255) return 0;

    irCommand = command;
    return 1;
}

// -------------------------------------------------------------- motions ----
//
// Each of these loops calls a single-step helper rather than inlining the step,
// because ardio's conditional branches only reach +-63 words and a loop body
// any larger than one call will not assemble.

// One yaw burst: turn at the given speed for yawPrecision milliseconds, then
// stop and settle.
void yawStep(int speed) {
    servoHold(YAW_PIN, speed, yawPrecision);
    servoPulse(YAW_PIN, yawStopSpeed);
    delay(5);
}

void leftMove(int moves) {
    for (int i = 0; i < moves; i = i + 1)
        yawStep(yawStopSpeed + yawMoveSpeed);
}

void rightMove(int moves) {
    for (int i = 0; i < moves; i = i + 1)
        yawStep(yawStopSpeed - yawMoveSpeed);
}

// One pitch step, clamped to the travel limits so the head cannot crash into
// the frame.
void pitchStep(int delta) {
    int target = pitchServoVal + delta;
    if (target < pitchMin) return;
    if (target > pitchMax) return;
    pitchServoVal = target;
    servoHold(PITCH_PIN, pitchServoVal, 50);
}

void upMove(int moves) {
    for (int i = 0; i < moves; i = i + 1)
        pitchStep(0 - pitchMoveSpeed);
}

void downMove(int moves) {
    for (int i = 0; i < moves; i = i + 1)
        pitchStep(pitchMoveSpeed);
}

// Spins the barrel for the given time, then stops it.
void rollBurst(int ms) {
    servoHold(ROLL_PIN, rollStopSpeed + rollMoveSpeed, ms);
    servoPulse(ROLL_PIN, rollStopSpeed);
    delay(5);
}

// Fires a single dart: about 60 degrees of barrel rotation.
void fire() {
    rollBurst(rollPrecision);
}

// Fires all six darts: a full turn of the barrel.
void fireAll() {
    rollBurst(rollPrecision * 6);
}

// Sweeps the pitch servo from one angle to another, one degree per frame.
void pitchSweep(int from, int to) {
    if (from <= to)
        for (int angle = from; angle <= to; angle = angle + 1) nodFrame(angle);
    else
        for (int angle = from; angle >= to; angle = angle - 1) nodFrame(angle);
}

void nodFrame(int angle) {
    servoPulse(PITCH_PIN, angle);
    delay(7);
}

void shakeHeadYes(int moves) {
    int startAngle = pitchServoVal;
    int nodAngle = startAngle + 20;
    for (int i = 0; i < moves; i = i + 1) nodOnce(startAngle, nodAngle);
}

void nodOnce(int startAngle, int nodAngle) {
    pitchSweep(startAngle, nodAngle);
    delay(50);
    pitchSweep(nodAngle, startAngle);
    delay(50);
}

void shakeHeadNo(int moves) {
    for (int i = 0; i < moves; i = i + 1) shakeOnce();
}

void shakeOnce() {
    servoHold(YAW_PIN, 140, 190);
    servoHold(YAW_PIN, yawStopSpeed, 50);
    servoHold(YAW_PIN, 40, 190);
    servoHold(YAW_PIN, yawStopSpeed, 50);
}

void homeServos() {
    servoHold(YAW_PIN, yawStopSpeed, 20);
    servoHold(ROLL_PIN, rollStopSpeed, 100);
    servoHold(PITCH_PIN, 100, 100);
    pitchServoVal = 100;
}

// ---------------------------------------------------------------- sketch ----

void setup() {
    pinMode(YAW_PIN, OUTPUT_MODE);
    pinMode(PITCH_PIN, OUTPUT_MODE);
    pinMode(ROLL_PIN, OUTPUT_MODE);
    ir_begin(IR_PIN);
    homeServos();
}

// Dispatches one decoded remote command.
void handleCommand(int command) {
    switch (command) {
        case CMD_UP: upMove(1); break;
        case CMD_DOWN: downMove(1); break;
        case CMD_LEFT: leftMove(1); break;
        case CMD_RIGHT: rightMove(1); break;
        case CMD_OK: fire(); break;
        case CMD_STAR: fireAllAndPause(); break;
    }
}

void fireAllAndPause() {
    fireAll();
    delay(50);
}

void loop() {
    if (ir_available(IR_PIN)) pollRemote();
    delay(5);
}

// Kept out of loop() because ardio's branches only reach +-63 words and the
// body of an if any larger than a single call will not assemble.
void pollRemote() {
    if (ir_decode(IR_PIN)) handleCommand(irCommand);
}
