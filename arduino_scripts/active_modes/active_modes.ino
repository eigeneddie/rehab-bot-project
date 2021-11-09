#include <AccelStepper.h>
#include <HX711_ADC.h>
#include <VL53L0X.h>
#include <Wire.h>

/* Full-active and assistive control functionality
 * --------------------------------------------------
 * for all active exercises, there are two operation running
 * within internal interrupt:
 *  1. haptic rendering for active mode (activeGo)
 *  2. knee angle proporsional control (pidGo)
 * 
 * Active exercise split in two ways:
 *  1. assistive exercise
 *  2. full-active exercise (2 subprograms)
 *      - isotonic
 *      - isometric
 */

// 1. Defining pins
#define stepperPulse 7
#define stepperDirection 8
#define stepperEnable 9
#define HX711_dout 4 //mcu > HX711 dout pi
#define HX711_sck 5  //mcu > HX711 sck pin


// 2. initiate objects 
AccelStepper motor_actuator(1, stepperPulse, stepperDirection);
HX711_ADC LoadCell(HX711_dout, HX711_sck);
VL53L0X distance_sensor;


//HX711 constructor:

// 3. universal string command
String stringCommand;

// 4. PID variable computation 
// (although we only use the proportional term)
float e2 = 0, e1 = 0, e0 =0;
float u2 = 0, u1 = 0, u0 = 0; 
float a0, a1, a2, b0, b1, b2;
float ku1, ku2, ke0, ke1, ke2;

float r; // reference command
float y; // plant output

float Kp = 70; // proportional
float Ki = 0; // integral
float Kd = 0; // derivative

float N = 0; // filter coeff
float Ts = 0.02; // 50 Hz sample frequency

// 5. Reading angle sensor and offseting
const int angleSensorPin = A0; // pot at knee mechanism
int sensorValue = 0;
const int offsetAngle = 64; // systematic offset from absolute potensiometer reading

// 6. Haptic rendering terms
float y_n, y_n_1; // position term
float a_coef_1; // position coef
float b_coef_0, b_coef_1; // force coefficient
float f_n, f_n_1; // force term
long step_target_n; // position converted to steps

// 7. Proportional control input paramters (for angle tracking)
float targetAngle; // target angle value (for single/continuous command)
float measuredAngle = 0; // value read from pot
long maxMotorSpeed;
float max_angle; // maximum angle limit
float min_angle; // minimum angle limit

// 8. activation parameters for ISR
int count_pidGo = 0; // to down sample the proportional control
bool pidGo = false; // Go-NoGo for controller (angle tracking)
bool activeGo = false; // Go-NoGo for active admittance
bool led_state = LOW; // LED indicator

// 9. moving motor
volatile long assignedSpeed;

// 10. Max speeds for functions in ISR
long max_active_mode = 1000; // [step/s]
long max_proportional_mode = 1500; // [step/s]

// 11. Knee angle monitoring 
// (undersampled and printed to serial monitor)
unsigned long startTime_dur;
unsigned long startTime; // start time
unsigned long currentTime; // current time
const unsigned long period = 2000; //undersampling data period

// 12. HX711 utilities
unsigned long t = 0;
static boolean newDataReady = 0;
float measuredForce = 0;

void setup() {
  Serial.begin(115200); //Serial.setTimeout(500);
  Wire.begin();

  //=== I. Basic system functionality ===
  //  - Stepper motor setup
  //  - Limit switch 
  //  - 

  // -> Stepper motor pin setup and init
  pinMode(stepperPulse, OUTPUT);
  pinMode(stepperDirection, OUTPUT);
  pinMode(stepperEnable, OUTPUT);

  digitalWrite(stepperPulse, LOW);
  digitalWrite(stepperDirection, HIGH);
  digitalWrite(stepperEnable, HIGH);

  // -> Limit switch Interrupt
  /*pinMode(interruptPin1, INPUT_PULLUP);
  pinMode(interruptPin2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(interruptPin1), front_limit_switch, CHANGE);
  attachInterrupt(digitalPinToInterrupt(interruptPin2), rear_limit_switch, CHANGE);
  */
  Serial.println("INIT_1: Stepper motor setup complete");
  
  //=== II. Load cell config ===
  LoadCell.begin();
  float calibrationValue; // calibration value (see example file "Calibration.ino")
  calibrationValue = 219.0;
  unsigned long stabilizingtime = 2000;
  boolean _tare = true;
  LoadCell.start(stabilizingtime, _tare);
  
  while(LoadCell.getTareTimeoutFlag()){
    Serial.println("Timeout, check MCU>HX711 wiring and pin designations");
    Serial.println("Retrying...")
  }
  
  LoadCell.setCalFactor(calibrationValue);
  Serial.println("INIT_2: Load cell setup complete");
  
  //=== III. Distance Sensor config ===
  distance_sensor.setTimeout(500);
  
  while (!distance_sensor.init()) {
    Serial.println("Failed to detect and initialize sensor!");
    Serial.println("Retrying...");
    delay(500);
  }
  Serial.println("distance sensor detected");
  delay(500);

  // Set for high accuracy, timing budget 200 ms
  distance_sensor.setMeasurementTimingBudget(200000);

  //Note: to get distance, use:
  //distance_sensor.readRangeSingleMillimeters();
   
  //=== IV. Internal Interrupt and Closed-loop Control Init ===
  // -> init PID and internal interrupt setting

  
  // ->Enable PID and internal interrupt
  init_pid(Kp, Ki, Kd, N, Ts); 
  TimerInit();
}

void loop() {

  // 1. Null: Loop level, set motor speed to zero
  motor_actuator.setSpeed(0); 
  motor_actuator.runSpeed(); // making sure the motor stops when command stops
  
  // 2. program functionality (mode '2' and '3')
  check_serial();
  
  // 3. checking knee angle
  read_angle();
  print_data();
}

void check_serial(){
  if (Serial.available() > 0){
    stringCommand = Serial.readStringUntil('\n');
    // if and else if statement to see if string command
    // is either '2', '3', or "-s"

    /* OPTION '2'
     * Command usage:
     * "2;ka;an;bn;bn1;max_angle;min_angle\n"
     */ 
    if (stringCommand.charAt(0) == '2'){
      pidGo = true;
      startTime = millis();
      Serial.println("Entering 2 mode");
      delay(2000);
      
      // a. Update max speed  
      //maxMotorSpeed = maxSpeed_contCommand;  
      motor_actuator.setMaxSpeed(maxMotorSpeed);
      zero_everything();
      while(pidGo){
        
        // b. Keep chacking for Reference angle 
        targetAngle = float(getValue(stringCommand,';',1).toInt());
      
        if (Serial.available() > 0){
          stringCommand = Serial.readStringUntil('\n');

          if (stringCommand == "-s"){
            pidGo = false;
            break;
          }

          else if (stringCommand.charAt(0) == '0'){
            targetAngle = float(getValue(stringCommand,';',1).toInt());
          }
        }

        // c. Measured angle
        sensorValue = analogRead(angleSensorPin);
        measuredAngle = float(map(sensorValue, 0, 1023, 0, 333)-offsetAngle); // map it from 0 to 333 degrees  

        //Note: update the reference and measured angle @ t = n for 
        // control loop @ t = n.
        // e. run motor
        motor_actuator.setSpeed(assignedSpeed);
        motor_actuator.runSpeed(); 

        // d. undersampling serial print
        currentTime = millis();
        if (currentTime-startTime >= period){
          Serial.print("'0' ");
          Serial.print(measuredAngle); Serial.print(" "); //print to raspberry pi system
          Serial.print(u0); Serial.print(" ");
          //Serial.print(u1); Serial.print(" ");
          //Serial.print(u2); Serial.print(" ");
          Serial.print(e0); Serial.println(" ");
          //Serial.print(e1); Serial.print(" ");
          //Serial.print(e2); Serial.println(" ");
          startTime = currentTime;
        }
      }
    } 

    /* OPTION '3'
     * Command usage:
     * "3;0;an;bn;bn1;max_angle;min_angle\n"
     */ 
    if(stringCommand.charAt(0) == '3' && stringCommand.charAt(1) == '0'){
      Serial.println("Initiating mode '3'"); Serial.println(" ");
      pidGo = false;
      activeGo = false;
      
      // Mode '3' initialization
      // a. parse command
      Serial.println("a. parsing command"); Serial.println(" ");
      delay(500);
      active_isotonic(stringCommand); // parsing single command

      // b. zero proportional control
      Serial.println("b. zeroing control param"); Serial.println(" ");
      delay(500);
      zero_everything();
      
      // c. zeroing stepper position (home)
      Serial.println("c. zeroing motor position"); Serial.println(" ");
      delay(500);
      read_angle();
      back_to_flexion();
      motor_actuator.setCurrentPosition(0); // set home

      // d. zeroing load cell (tare)
      Serial.println("d. zeroing force sensor"); Serial.println(" ");
      LoadCell.tareNoDelay(); // tare (zero load cell)
      while(!LoadCell.getTareStatus()){}
      Serial.println("Tare complete."); Serial.println(" ");
      delay(500);

      Serial.println("Entering mode '3': isotonic");
      delay(1000);
      activeGo = true;
      startTime = millis();      

      while(activeGo){

        // a. Read force sensor
        static boolean newDataReady = 0;
        const int serialPrintInterval = 2000; //increase value to slow down serial print activity
        LoadCell.update(); // check for new data/start next conversion
        measuredForce = LoadCell.getData(); // get smoothed value from the dataset
        
        // b. run motor
        motor_actuator.moveTo(step_target_n);
        motor_actuator.run();

        // c. Measured angle
        read_angle();

        // d. Stoping criteria (force stop "-s")
        stopping_criteria_hap_ren();
        
        // e. undersampling print
        print_data("mode30");

        // f. check if measured angle <= min_angle
        back_to_flexion();

      }
    }
  } 
}

// Active mode '2' 
void assistive_control (String command_data){
  String acoef1 = getValue(command_data, ';', 2);
  String bcoef0 = getValue(command_data, ';', 3);
  String bcoef1 = getValue(command_data, ';', 4);
  String maxAng = getValue(command_data, ';', 5);
  String minAng = getValue(command_data, ';', 6);

  a_coef_1 = acoef1.toFloat();
  b_coef_0 = bcoef0.toFloat();
  b_coef_1 = bcoef1.toFloat();
  max_angle = maxAng.toFloat();
  min_angle = minAng.toFloat();
}

// Active mode '3' & '0'
void active_isotonic (String command_data){
  String acoef1 = getValue(command_data, ';', 2);
  String bcoef0 = getValue(command_data, ';', 3);
  String bcoef1 = getValue(command_data, ';', 4);
  String maxAng = getValue(command_data, ';', 5);
  String minAng = getValue(command_data, ';', 6);

  a_coef_1 = acoef1.toFloat();
  b_coef_0 = bcoef0.toFloat();
  b_coef_1 = bcoef1.toFloat();
  max_angle = maxAng.toFloat();
  min_angle = minAng.toFloat();
}

// Active mode '3' & '1'
void active_isometric (String command_data){
  String acoef1 = getValue(command_data, ';', 2);
  String bcoef0 = getValue(command_data, ';', 3);
  String bcoef1 = getValue(command_data, ';', 4);
  String maxAng = getValue(command_data, ';', 5);
  String minAng = getValue(command_data, ';', 6);

  a_coef_1 = acoef1.toFloat();
  b_coef_0 = bcoef0.toFloat();
  b_coef_1 = bcoef1.toFloat();
  max_angle = maxAng.toFloat();
  min_angle = minAng.toFloat();
}

float haptic_rendering(float measured_force){
  y_n = a_coef_1*y_n_1 + b_coef_0*f_n + b_coef_1*f_n_1;
  long position_target = (long) y_n*50.0; // 1/8 rev/mm * 400 steps/rev 
  f_n_1 = f_n;
  y_n_1 = y_n;
  return position_target;
}

// return knee angle to flexion (max_angle) position
void back_to_flexion(){
  if (measuredAngle != max_angle){
    maxMotorSpeed = max_proportional_mode;
    pidGo = true;
    activeGo = false;
    while(pidGo){
      // i. measure angle
      read_angle();

      // ii. set target angle
      targetAngle = max_angle;

      // iii. run motor
      motor_actuator.setSpeed(assignedSpeed);
      motor_actuator.runSpeed();
      
      stopping_criteria_angleCont();
    }
  }
}

// get current angle sensor reading
void read_angle(){
  sensorValue = analogRead(angleSensorPin);
  measuredAngle = float(map(sensorValue, 0, 1023, 0, 333)-offsetAngle);
}

// print current data to screen
void print_data(String mode){
  currentTime = millis(); // grab time
  if (currentTime-startTime >= period){
    Serial.print(mode); Serial.print(" ");
    Serial.print(measuredAngle); Serial.print(" ");
    Serial.print(u0); Serial.print(" ");
    Serial.print(measuredForce); Serial.println(" ");
    startTime = currentTime;
  }  
}
// stopping criteria for angle tracking (proportional control)
void stopping_criteria_angleCont(){
  if (Serial.available() > 0){
    stringCommand = Serial.readStringUntil('\n');
    if (stringCommand == "-s"){
      pidGo = false;
      activeGo = true;
    }
  }
}

// stopping criteria for haptic rendering
void stopping_criteria_hap_ren(){
  if (Serial.available() > 0){
    stringCommand = Serial.readStringUntil('\n');
    if (stringCommand == "-s"){
      activeGo = false;
    }
  }
}

// ISR for executing angle proporsional control & haptic rendering
ISR(TIMER1_COMPA_vect){
  /* Interrupt service routine for
   *  PID execution
   */
  if (pidGo == true){
    count_pidGo++;
    if(count_pidGo>49){
      assignedSpeed = int(pid_execute(targetAngle, measuredAngle, maxMotorSpeed));
      count_pidGo = 0;
    }
    digitalWrite(LED_BUILTIN, LOW);
  } 
  else if (pidGo == false){
    assignedSpeed = 0;
  }
  
  if (activeGo == true){
    step_target_n = haptic_rendering(measuredForce);
    digitalWrite(LED_BUILTIN, HIGH);
  }
}

// Handles internal interrupt execution
void TimerInit(){
  /* Initialize timer1 
   * Sampling Frequency = 100 Hz 
   * Timer for PID loop in passive training (angle control)  
   * 
   * Study reference:
   * https://www.robotshop.com/community/forum/t/arduino-101-timers-and-interrupts/13072
   * 
   */ 
  noInterrupts(); //disable all interrupts
  TCCR1A = 0; // Timer or Counter Control Register
  TCCR1B = 0; // The prescaler can be configure in TCCRx
  TCNT1  = 0; // Timer or Counter Register. The actual timer value is stored here.

  OCR1A = 1249; // Output Compare Match Register (16Mhz/256/<sampling_freq>Hz)
  //62499 (1 Hz);//31249 (2Hz); //15625 (4Hz) //6249 (10Hz); //1249 (50Hz);
  //624 (100Hz); 

  TCCR1B |= (1 << WGM12);  // CTC (Clear Time on Compare Match)
  TCCR1B |= (1 << CS12);   // 256 prescaler
  TIMSK1 |= (1 << OCIE1A); // Interrupt Mask Register (Enable Output Compare Interrupt)
  interrupts();            // Enable all interrupts
}

void zero_everything(){
  e2 = 0, e1 = 0, e0 =0;
  u2 = 0, u1 = 0, u0 = 0; 
  f_n = 0, f_n_1 = 0;
  y_n = 0, y_n_1 = 0;
}

void init_pid(float Kp, float Ki, float Kd, float N, float Ts){
  /* Initializing PID equation based on user defined parameters
   * 
   *  Args:
   *    Kp [float]: proportional gain  
   *    Ki [float]: integral gain
   *    Kd [float]: derivative gain
   *    N [float]: filter coefficient
   *    Ts [float]: sample time in seconds
   */
   
  a0 = (1+N*Ts);
  a1 = -(2+N*Ts);
  a2 = 1;

  b0 = Kp*(1+N*Ts) + Ki*Ts*(1+N*Ts) + Kd*N;
  b1 = -(Kp*(2+N*Ts) + Ki*Ts + 2*Kd*N);
  b2 = Kp + Kd*N;
  
  ku1 = a1/a0; 
  ku2 = a2/a0; 

  ke0 = b0/a0; 
  ke1 = b1/a0; 
  ke2 = b2/a0;
}

float pid_execute(float target_angle, float plant_output, long speed_sat){
  /* Executing PID control algorithm
   * 
   *  Args:
   *    target_angle [float]: reference angle [deg]
   *    plant_output [float]: measured angle [deg]
   *    speed_sat [float]: max/min speed of motor [pulse/s] (aka speed saturation)
   */

  /*e2 = e1; 
  e1 = e0; 
  u2 = u1;
  u1 = u0;*/

  r = target_angle;
  y = plant_output;

  float speed_sat_fl = float(speed_sat);
  e0 = r-y; // compute error
  u0 = ke0*e0; //Kp*e0;//-ku1*u1 - ku2*u2 + ke0*e0 + ke1*e1 + ke2*e2; 

  if (u0 >= speed_sat_fl){
    u0 = speed_sat;
  }

  else if (u0 <= -speed_sat_fl){
    u0 = -speed_sat;
  }
  return u0;
}

String getValue(String command_data, char separator, int index){
 /* This code is thanks to the people of stackOverflow <3!!
  * convinient for extracting command usage.
  */
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = command_data.length()-1;

  for(int i=0; i<=maxIndex && found<=index; i++){
    if(command_data.charAt(i)==separator || i==maxIndex){
        found++;
        strIndex[0] = strIndex[1]+1;
        strIndex[1] = (i == maxIndex) ? i+1 : i;
        }
  }

  return found>index ? command_data.substring(strIndex[0], strIndex[1]) : "";
}
