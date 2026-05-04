// Arduino UNO sketch for Radiation King Radio
// Replaces Pi Pico functionality via USB serial protocol
// Communicates with main.py using comma-separated serial messages

#include <Arduino.h>
#include <Encoder.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ============ PIN DEFINITIONS ============

// Motor Control Pins (SIN coil: PWM + direction, COS coil: PWM + direction)
#define SIN_PWM_PIN 6        // PWM on pin 6 (moved from 3 to free up pins 2-3 for encoder interrupts)
#define SIN_POS_PIN 7        // Direction positive
#define SIN_NEG_PIN 9        // Direction negative
#define COS_PWM_PIN 5        // PWM on pin 5
#define COS_POS_PIN 10       // Direction positive
#define COS_NEG_PIN 11       // Direction negative

// Rotary Encoder Pins (Volume and Tuning encoders)
#define VOLUME_ENCODER_CLK_PIN 2     // Volume encoder CLK (hardware interrupt INT0)
#define VOLUME_ENCODER_DT_PIN 3      // Volume encoder DT (hardware interrupt INT1)
#define TUNING_ENCODER_CLK_PIN 4     // Tuning encoder CLK (polling mode)
#define TUNING_ENCODER_DT_PIN 8      // Tuning encoder DT (polling mode)

// Button Pins (5 buttons)
#define BUTTON_1_PIN A0      // Using analog pin as digital
#define BUTTON_2_PIN A1      // Using analog pin as digital
#define BUTTON_3_PIN A2      // Using analog pin as digital
#define BUTTON_4_PIN A3      // Using analog pin as digital
#define BUTTON_5_PIN 12
#define BUTTON_PINS {BUTTON_1_PIN, BUTTON_2_PIN, BUTTON_3_PIN, BUTTON_4_PIN, BUTTON_5_PIN}
#define BUTTON_COUNT 5

// NeoPixel Pins
#define GAUGE_NEOPIXEL_PIN 13   // Gauge LEDs
#define GAUGE_LED_COUNT 8
#define AUX_NEOPIXEL_PIN A5     // Aux LEDs (using A5 as digital output)
#define AUX_LED_COUNT 5

// ============ SETTINGS ============

// Serial
#define BAUD_RATE 115200
#define UART_TIMEOUT 100        // milliseconds
#define UART_SEND_INTERVAL 50   // milliseconds

// Motor
#define MOTOR_ANGLE_MIN 14
#define MOTOR_ANGLE_MAX 168
#define PWM_MAX_VALUE 255       // 8-bit PWM on Arduino (vs 65535 on Pico)
#define MOTOR_REF_VOLTAGE 5.0
#define PWM_FREQUENCY 490       // Arduino default

// Encoder Settings
#define ENCODER_RESOLUTION 4    // Encoder pulses per click (typical mechanical encoders)
#define VOLUME_ENCODER_RANGE 100  // Number of steps for volume (0-100)
#define TUNING_ENCODER_RANGE 168  // Number of steps for tuning (14-168 degrees)
#define VOLUME_ENCODER_DEAD_ZONE 2  // Steps before volume change is sent
#define TUNING_ENCODER_DEAD_ZONE 1  // Steps before tuning change is sent

// Buttons
#define BUTTON_SHORT_PRESS 60   // milliseconds
#define BUTTON_LONG_PRESS 1500  // milliseconds

// NeoPixels
#define GAUGE_COLOR_R 160
#define GAUGE_COLOR_G 32
#define GAUGE_COLOR_B 0
#define GAUGE_COLOR_W 38
#define GAUGE_MAX_BRIGHTNESS 0.3

#define AUX_COLOR_R 255
#define AUX_COLOR_G 20
#define AUX_COLOR_B 0
#define AUX_COLOR_W 0
#define AUX_MAX_BRIGHTNESS 1.0

// ============ GLOBAL VARIABLES ============

// NeoPixel objects
Adafruit_NeoPixel gauge_pixels(GAUGE_LED_COUNT, GAUGE_NEOPIXEL_PIN, NEO_GRBW + NEO_KHZ800);
Adafruit_NeoPixel aux_pixels(AUX_LED_COUNT, AUX_NEOPIXEL_PIN, NEO_GRBW + NEO_KHZ800);

// Encoder objects (Volume and Tuning)
Encoder volume_encoder(VOLUME_ENCODER_CLK_PIN, VOLUME_ENCODER_DT_PIN);
Encoder tuning_encoder(TUNING_ENCODER_CLK_PIN, TUNING_ENCODER_DT_PIN);

// Motor state
float motor_angle = 0;
float motor_angle_prev = 0;

// Encoder state (in steps/pulses)
long volume_encoder_pos = 0;
long volume_encoder_pos_prev = 0;
long tuning_encoder_pos = 0;
long tuning_encoder_pos_prev = 0;

// Scaled values (0-1 for volume, 14-168 for tuning angle)
float volume_value = 0.5;
float tuning_angle = 90;

// Button state
int button_pins[] = BUTTON_PINS;
unsigned long button_press_time[BUTTON_COUNT] = {0};
unsigned long button_release_time[BUTTON_COUNT] = {0};
bool button_state[BUTTON_COUNT] = {false};
bool button_held_state[BUTTON_COUNT] = {false};

// Serial communication
unsigned long last_send_time = 0;
char serial_buffer[256];
int serial_buffer_idx = 0;
bool on_off_state = false;

// Button event types (matches Pico)
#define BUTTON_RELEASED_EVENT 0
#define BUTTON_PRESSED_EVENT 1
#define BUTTON_HELD_EVENT 2

// ============ SETUP ============

void setup() {
  // Initialize serial
  Serial.begin(BAUD_RATE);
  Serial.setTimeout(UART_TIMEOUT);
  
  // Initialize motor control pins
  pinMode(SIN_PWM_PIN, OUTPUT);
  pinMode(SIN_POS_PIN, OUTPUT);
  pinMode(SIN_NEG_PIN, OUTPUT);
  pinMode(COS_PWM_PIN, OUTPUT);
  pinMode(COS_POS_PIN, OUTPUT);
  pinMode(COS_NEG_PIN, OUTPUT);
  
  // Initialize button pins
  for (int i = 0; i < BUTTON_COUNT; i++) {
    pinMode(button_pins[i], INPUT_PULLUP);
  }
  
  // Initialize NeoPixels
  gauge_pixels.begin();
  gauge_pixels.setBrightness(255 * GAUGE_MAX_BRIGHTNESS);
  gauge_pixels.clear();
  gauge_pixels.show();
  
  aux_pixels.begin();
  aux_pixels.setBrightness(255 * AUX_MAX_BRIGHTNESS);
  aux_pixels.clear();
  aux_pixels.show();
  
  // Send startup message
  send_uart("I", "Arduino initialized");
}

// ============ MAIN LOOP ============

void loop() {
  // Process serial input
  if (Serial.available()) {
    receive_uart();
  }
  
  // Read button inputs
  read_buttons();
  
  // Read encoder inputs (volume and tuning)
  read_encoders();
  
  // Send periodic updates
  if (millis() - last_send_time > UART_SEND_INTERVAL) {
    last_send_time = millis();
  }
}

// ============ SERIAL COMMUNICATION ============

void receive_uart() {
  if (Serial.available()) {
    char c = Serial.read();
    
    if (c == '\n') {
      // Process complete message
      process_uart_message(serial_buffer, serial_buffer_idx);
      serial_buffer_idx = 0;
    } else if (c != '\r') {
      // Add to buffer
      if (serial_buffer_idx < sizeof(serial_buffer) - 1) {
        serial_buffer[serial_buffer_idx++] = c;
      }
    }
  }
}

void process_uart_message(char* buffer, int length) {
  if (length == 0) return;
  
  // Parse comma-separated values
  char temp[256];
  strncpy(temp, buffer, length);
  temp[length] = '\0';
  
  // Get command type (first character)
  char command_type = temp[0];
  
  // Parse remaining fields
  char* data1 = NULL;
  char* data2 = NULL;
  char* data3 = NULL;
  char* data4 = NULL;
  
  // Split by commas
  char* token = strtok(temp, ",");
  if (token) token = strtok(NULL, ","); // Skip command, get data1
  if (token) data1 = token;
  
  if (data1) token = strtok(NULL, ","); 
  if (token) data2 = token;
  
  if (data2) token = strtok(NULL, ",");
  if (token) data3 = token;
  
  if (data3) token = strtok(NULL, ",");
  if (token) data4 = token;
  
  // Process commands
  switch (command_type) {
    case 'M': // Motor control
      if (data1) {
        float angle = atof(data1);
        set_motor(angle);
      }
      break;
      
    case 'V': // Volume control
      if (data1) {
        float volume_level = atof(data1);
        // Store for LED control if needed
        set_gauge_brightness(volume_level);
      }
      break;
      
    case 'P': // Power state
      if (data1) {
        int power_state = atoi(data1);
        on_off_state = (power_state == 1);
        if (on_off_state) {
          send_uart("I", "Power on");
        } else {
          send_uart("I", "Power off");
          // Turn off LEDs
          gauge_pixels.clear();
          gauge_pixels.show();
          aux_pixels.clear();
          aux_pixels.show();
        }
      }
      break;
      
    case 'C': // Control/sweep animation
      // data1 would be "Sweep" or "NoSweep"
      // For now, just acknowledge
      break;
      
    case 'I': // Info message
      // Main code is sending info, just acknowledge
      break;
  }
}

void send_uart(const char* command_type, const char* data1, 
               const char* data2, const char* data3, const char* data4) {
  Serial.print(command_type);
  Serial.print(",");
  Serial.print(data1);
  Serial.print(",");
  Serial.print(data2);
  Serial.print(",");
  Serial.print(data3);
  Serial.print(",");
  Serial.print(data4);
  Serial.println(",");
}

void send_uart_float(const char* command_type, const char* data1, float value) {
  char buffer[32];
  dtostrf(value, 6, 3, buffer);
  send_uart(command_type, data1, buffer, "", "");
}

// ============ MOTOR CONTROL ============

void set_motor(float angle) {
  // Constrain angle to valid range
  angle = constrain(angle, MOTOR_ANGLE_MIN, MOTOR_ANGLE_MAX);
  
  if (angle == motor_angle) return; // No change
  
  motor_angle = angle;
  
  // Convert angle to radians
  float radian = angle * 0.0174533;  // PI/180
  float sin_radian = sin(radian);
  float cos_radian = cos(radian);
  
  // Calculate voltages (normalized to 0-1)
  float sin_voltage = abs(sin_radian);
  float cos_voltage = abs(cos_radian);
  
  // Map to PWM values (0-255 on Arduino)
  int sin_pwm = map(sin_voltage * 100, 0, 100, 0, PWM_MAX_VALUE);
  int cos_pwm = map(cos_voltage * 100, 0, 100, 0, PWM_MAX_VALUE);
  
  // Set PWM duty cycles
  analogWrite(SIN_PWM_PIN, sin_pwm);
  analogWrite(COS_PWM_PIN, cos_pwm);
  
  // Set direction based on sign
  if (sin_radian <= 0) {
    digitalWrite(SIN_POS_PIN, LOW);
    digitalWrite(SIN_NEG_PIN, HIGH);
  } else {
    digitalWrite(SIN_POS_PIN, HIGH);
    digitalWrite(SIN_NEG_PIN, LOW);
  }
  
  if (cos_radian <= 0) {
    digitalWrite(COS_POS_PIN, LOW);
    digitalWrite(COS_NEG_PIN, HIGH);
  } else {
    digitalWrite(COS_POS_PIN, HIGH);
    digitalWrite(COS_NEG_PIN, LOW);
  }
}

// ============ BUTTON READING ============

void read_buttons() {
  for (int i = 0; i < BUTTON_COUNT; i++) {
    bool current_state = digitalRead(button_pins[i]) == LOW; // LOW = pressed (pullup)
    
    if (current_state && !button_state[i]) {
      // Button just pressed
      button_press_time[i] = millis();
      button_state[i] = true;
      button_held_state[i] = false;
      
      // Send button pressed event
      char data1[5];
      itoa(i, data1, 10);
      char data2[5];
      itoa(BUTTON_PRESSED_EVENT, data2, 10);
      send_uart("B", data1, data2, "", "");
      
    } else if (!current_state && button_state[i]) {
      // Button just released
      unsigned long press_duration = millis() - button_press_time[i];
      button_state[i] = false;
      button_held_state[i] = false;
      
      // Send button released event
      char data1[5];
      itoa(i, data1, 10);
      char data2[5];
      itoa(BUTTON_RELEASED_EVENT, data2, 10);
      send_uart("B", data1, data2, "", "");
      
    } else if (current_state && button_state[i] && !button_held_state[i]) {
      // Button is still pressed, check for hold
      unsigned long press_duration = millis() - button_press_time[i];
      if (press_duration > BUTTON_LONG_PRESS) {
        button_held_state[i] = true;
        
        // Send button held event
        char data1[5];
        itoa(i, data1, 10);
        char data2[5];
        itoa(BUTTON_HELD_EVENT, data2, 10);
        send_uart("B", data1, data2, "", "");
      }
    }
  }
}

// ============ ENCODER READING ============

void read_encoders() {
  // Read volume encoder position (hardware interrupt)
  long volume_pos = volume_encoder.read();
  
  // Calculate volume value (0-1 range) from encoder steps
  // Encoder steps divided by resolution, then mapped to 0-VOLUME_ENCODER_RANGE
  long volume_steps = volume_pos / ENCODER_RESOLUTION;
  float new_volume = constrain((float)volume_steps / VOLUME_ENCODER_RANGE, 0.0, 1.0);
  
  // Check if volume changed beyond dead zone
  if (abs(volume_encoder_pos - volume_encoder_pos_prev) >= VOLUME_ENCODER_DEAD_ZONE) {
    volume_encoder_pos_prev = volume_encoder_pos;
    volume_value = new_volume;
    
    // Send volume update
    char buffer[32];
    dtostrf(volume_value, 6, 3, buffer);
    send_uart("V", buffer, "", "", "");
  }
  volume_encoder_pos = volume_pos;
  
  // Read tuning encoder position (polling mode)
  long tuning_pos = tuning_encoder.read();
  
  // Calculate tuning angle (14-168 degrees) from encoder steps
  long tuning_steps = tuning_pos / ENCODER_RESOLUTION;
  float new_angle = map(tuning_steps, 0, TUNING_ENCODER_RANGE, 
                        MOTOR_ANGLE_MIN * 100, MOTOR_ANGLE_MAX * 100) / 100.0;
  new_angle = constrain(new_angle, MOTOR_ANGLE_MIN, MOTOR_ANGLE_MAX);
  
  // Check if tuning changed beyond dead zone
  if (abs(tuning_encoder_pos - tuning_encoder_pos_prev) >= TUNING_ENCODER_DEAD_ZONE) {
    tuning_encoder_pos_prev = tuning_encoder_pos;
    tuning_angle = new_angle;
    
    // Send tuning update
    char buffer[32];
    dtostrf(tuning_angle, 6, 1, buffer);
    send_uart("M", buffer, "", "", "");
  }
  tuning_encoder_pos = tuning_pos;
}

// ============ NEOPIXEL CONTROL ============

void set_gauge_brightness(float volume_level) {
  // Scale brightness based on volume (0-1)
  int brightness = (int)(volume_level * 255 * GAUGE_MAX_BRIGHTNESS);
  brightness = constrain(brightness, 0, 255);
  
  gauge_pixels.setBrightness(brightness);
  
  // Fill gauge with color
  for (int i = 0; i < GAUGE_LED_COUNT; i++) {
    gauge_pixels.setPixelColor(i, gauge_pixels.Color(GAUGE_COLOR_G, GAUGE_COLOR_R, 
                                                       GAUGE_COLOR_B, GAUGE_COLOR_W));
  }
  gauge_pixels.show();
}

void set_aux_brightness(float level) {
  int brightness = (int)(level * 255 * AUX_MAX_BRIGHTNESS);
  brightness = constrain(brightness, 0, 255);
  
  aux_pixels.setBrightness(brightness);
  
  for (int i = 0; i < AUX_LED_COUNT; i++) {
    aux_pixels.setPixelColor(i, aux_pixels.Color(AUX_COLOR_G, AUX_COLOR_R, 
                                                   AUX_COLOR_B, AUX_COLOR_W));
  }
  aux_pixels.show();
}
