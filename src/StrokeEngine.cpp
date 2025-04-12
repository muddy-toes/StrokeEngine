#include <Arduino.h>
#include <StrokeEngine.h>
#include <FastAccelStepper.h>
#include <pattern.h>

FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *servo = NULL;

void StrokeEngine::begin(machineGeometry *physics, motorProperties *motor) {
    // store the machine geometry and motor properties pointer
    _physics = physics;
    _motor = motor;

    // Derived Machine Geometry & Motor Limits in steps:
    _travel = (_physics->physicalTravel - (2 * _physics->keepoutBoundary));
    _minStep = 0;
    _maxStep = int(0.5 + _travel * _motor->stepsPerMillimeter);
    _maxStepPerSecond = int(0.5 + _motor->maxSpeed * _motor->stepsPerMillimeter);
    _maxStepAcceleration = int(0.5 + _motor->maxAcceleration * _motor->stepsPerMillimeter);
          
    // Initialize with default values
    _state = UNDEFINED;
    _isHomed = false;
    _patternIndex = 0;
    _index = 0;
    _depth = _maxStep;
    _previousDepth = _maxStep; 
    _stroke = _maxStep / 3;
    _previousStroke = _maxStep / 3;
    _timeOfStroke = 1.0;
    _sensation = 0.0;

    _catchup_multiplier = 1.0;

    // Setup FastAccelStepper 
    engine.init();
    servo = engine.stepperConnectToPin(_motor->stepPin);
    if (servo) {
        servo->setDirectionPin(_motor->directionPin, _motor->invertDirection);
        servo->setEnablePin(_motor->enablePin, _motor->enableActiveLow);
        servo->setAutoEnable(false);
        servo->disableOutputs(); 
    }
    Serial.println("Servo initialized");

#ifdef DEBUG_TALKATIVE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif
}

void StrokeEngine::setSpeed(float speed, bool applyNow = false) {

    // Update pattern with new speed, will be used with the next stroke or on update request
    if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {

        // Convert FPM into seconds to complete a full stroke
        // Constrain stroke time between 10ms and 120 seconds
        _timeOfStroke = constrain(60.0 / speed, 0.01, 120.0);

        patternTable[_patternIndex]->setTimeOfStroke(_timeOfStroke);

#ifdef DEBUG_TALKATIVE
    Serial.println("setTimeOfStroke: " + String(_timeOfStroke, 2));
#endif

        // When running a pattern and immediate update requested: 
        if ((_state == PATTERN) && (applyNow == true)) {
            // set flag to apply update from stroking thread
            _applyUpdate = true;

#ifdef DEBUG_TALKATIVE
        Serial.println("Apply New Settings Now");
#endif
        }

        // give back mutex
        xSemaphoreGive(_patternMutex);
    }
}

float StrokeEngine::getSpeed() {
    // Convert speed into FPMs
    return 60.0 / _timeOfStroke;
}

void StrokeEngine::setDepth(float depth, bool applyNow = false) {

    if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {
        // Convert depth from mm into steps
        // Constrain depth between minStep and maxStep
        _depth = constrain(int(depth * _motor->stepsPerMillimeter), _minStep, _maxStep); 

        if (_state == PATTERN) {
            patternTable[_patternIndex]->setDepth(_depth);
        } else if (_state == STREAMING) {
            livePosition->setDepth(_depth);
        }

#ifdef DEBUG_TALKATIVE
        Serial.println("setDepth: " + String(_depth));
#endif
        // When running a pattern and immediate update requested: 
        if ((_state == PATTERN) && (applyNow == true)) {
            // set flag to apply update from stroking thread
            _applyUpdate = true;

#ifdef DEBUG_TALKATIVE
        Serial.println("Apply New Settings Now");
#endif
        }

        // give back mutex
        xSemaphoreGive(_patternMutex);
    }

    // if in state SETUPDEPTH then adjust
    if (_state == SETUPDEPTH) {
        _setupDepths();
    }
}

float StrokeEngine::getDepth() {
    // Convert depth from steps into mm
    return _depth / _motor->stepsPerMillimeter;
}

void StrokeEngine::setStroke(float stroke, bool applyNow = false) {
    // Update pattern with new stroke, will be used with the next stroke or on update request
    if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {

        // Convert stroke from mm into steps
        // Constrain stroke between minStep and maxStep
        _stroke = constrain(int(stroke * _motor->stepsPerMillimeter), _minStep, _maxStep); 

        if (_state == PATTERN) {
            patternTable[_patternIndex]->setStroke(_stroke);
        } else if (_state == STREAMING) {
            livePosition->setStroke(_stroke);
        }

#ifdef DEBUG_TALKATIVE
        Serial.println("setStroke: " + String(_stroke));
#endif
    
        // When running a pattern and immediate update requested: 
        if ((_state == PATTERN) && (applyNow == true)) {
            // set flag to apply update from stroking thread
            _applyUpdate = true;

#ifdef DEBUG_TALKATIVE
        Serial.println("Apply New Settings Now");
#endif
        }

        // give back mutex
        xSemaphoreGive(_patternMutex);
    }

    // if in state SETUPDEPTH then adjust
    if (_state == SETUPDEPTH) {
        _setupDepths();
    }
}

float StrokeEngine::getStroke() {
    // Convert stroke from steps into mm
    return _stroke / _motor->stepsPerMillimeter;
}

void StrokeEngine::appendToStreaming(unsigned int position, unsigned int time, boolean replace) {
    bool overflow;
    if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {
        if (replace) {
            livePosition->clear();
        }
        overflow = livePosition->addPosition(position, time * _catchup_multiplier);
        if( overflow ) {
            _catchup_multiplier = 1;
        } else {
            // If addPosition returns false, it means we overwrote data in the buffer, so speed up a little on the next motion
            _catchup_multiplier *= 0.95;
        }

#ifdef DEBUG_TALKATIVE
        Serial.println("appendToStreaming: " + String(position) + " " + String(time));
#endif

        // give back mutex
        xSemaphoreGive(_patternMutex);
    }
}

void StrokeEngine::setSensation(float sensation, bool applyNow = false) {

    // Update pattern with new sensation, will be used with the next stroke or on update request
    if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {

        // Constrain sensation between -100 and 100
        _sensation = constrain(sensation, -100, 100); 

        patternTable[_patternIndex]->setSensation(_sensation);

#ifdef DEBUG_TALKATIVE
        Serial.println("setSensation: " + String(_sensation));
#endif

        // When running a pattern and immediate update requested: 
        if ((_state == PATTERN) && (applyNow == true)) {
            // set flag to apply update from stroking thread
            _applyUpdate = true;

#ifdef DEBUG_TALKATIVE
        Serial.println("Apply New Settings Now");
#endif
        }

        // give back mutex
        xSemaphoreGive(_patternMutex);
    }
    
    // if in state SETUPDEPTH then adjust
    if (_state == SETUPDEPTH) {
        _setupDepths();
    }
}

float StrokeEngine::getSensation() {
    return _sensation;
}

bool StrokeEngine::setPattern(int patternIndex, bool applyNow = false) {
    // Check wether pattern Index is in range
    if ((patternIndex < patternTableSize) && (patternIndex >= 0) && (patternIndex != _patternIndex)) {
        _patternIndex = patternIndex;

        // Inject current motion parameters into new pattern
        if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {
            patternTable[_patternIndex]->setSpeedLimit(_maxStepPerSecond, _maxStepAcceleration, _motor->stepsPerMillimeter);
            patternTable[_patternIndex]->setTimeOfStroke(_timeOfStroke);
            patternTable[_patternIndex]->setStroke(_stroke);
            patternTable[_patternIndex]->setDepth(_depth);
            patternTable[_patternIndex]->setSensation(_sensation);

            // When running a pattern and immediate update requested: 
            if ((_state == PATTERN) && (applyNow == true)) {
                // set flag to apply update from stroking thread
                _applyUpdate = true;

#ifdef DEBUG_TALKATIVE
            Serial.println("Apply New Settings Now");
#endif
            }

            // Reset index counter
            _index = -1; 

            // give back mutex
            xSemaphoreGive(_patternMutex);
        }

#ifdef DEBUG_TALKATIVE
    Serial.println("setPattern: [" + String(_patternIndex) + "] " + patternTable[_patternIndex]->getName());
    Serial.println("setTimeOfStroke: " + String(_timeOfStroke, 2));
    Serial.println("setDepth: " + String(_depth));
    Serial.println("setStroke: " + String(_stroke));
    Serial.println("setSensation: " + String(_sensation));
#endif
        return true;
    }

    // Return false on no match
#ifdef DEBUG_TALKATIVE
    Serial.println("Failed to set pattern: " + String(_patternIndex));
#endif
    return false;   
}

int StrokeEngine::getPattern() {
    return _patternIndex;
}

bool StrokeEngine::startPattern() {
    // Only valid if state is ready
    if (_state == READY || _state == SETUPDEPTH || _state == STREAMING) {

        // Stop current move, should one be pending (moveToMax or moveToMin)
        if (servo->isRunning()) {
            // Stop servo motor as fast as legally allowed
            servo->setAcceleration(_maxStepAcceleration);
            servo->applySpeedAcceleration();
            servo->stopMove();
        }

        // Set state to PATTERN
        _state = PATTERN;

        // Reset Stroke and Motion parameters
        _index = -1;
        if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {
            patternTable[_patternIndex]->setSpeedLimit(_maxStepPerSecond, _maxStepAcceleration, _motor->stepsPerMillimeter);
            patternTable[_patternIndex]->setTimeOfStroke(_timeOfStroke);
            patternTable[_patternIndex]->setStroke(_stroke);
            patternTable[_patternIndex]->setDepth(_depth);
            patternTable[_patternIndex]->setSensation(_sensation);            
            xSemaphoreGive(_patternMutex);
        }

        
#ifdef DEBUG_TALKATIVE
        Serial.print(" _timeOfStroke: " + String(_timeOfStroke));
        Serial.print(" | _depth: " + String(_depth));
        Serial.print(" | _stroke: " + String(_stroke));
        Serial.println(" | _sensation: " + String(_sensation));
#endif

        if (_taskStrokingHandle == NULL) {
            // Create Stroke Task
            xTaskCreatePinnedToCore(
                this->_strokingImpl,    // Function that should be called
                "Stroking",             // Name of the task (for debugging)
                4096,                   // Stack size (bytes)
                this,                   // Pass reference to this class instance
                24,                     // Pretty high task priority
                &_taskStrokingHandle,   // Task handle
                1                       // Pin to application core
            ); 
        } else {
            // Resume task, if it already exists
            vTaskResume(_taskStrokingHandle);
        }

#ifdef DEBUG_TALKATIVE
        Serial.println("Started motion task");
        Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

        return true;

    } else {

#ifdef DEBUG_TALKATIVE
        Serial.println("Failed to start motion");
#endif
        return false;

    }
}

bool StrokeEngine::startStreaming() {
    // Only valid if state is ready
    if (_state == READY || _state == SETUPDEPTH || _state == PATTERN) {

        // Stop current move, should one be pending (moveToMax or moveToMin)
        if (servo->isRunning()) {
            // Stop servo motor as fast as legally allowed
            servo->setAcceleration(_maxStepAcceleration);
            servo->applySpeedAcceleration();
            servo->stopMove();
        }

        // Set state to PATTERN
        _state = STREAMING;

        livePosition->setSpeedLimit(_maxStepPerSecond, _maxStepAcceleration, _motor->stepsPerMillimeter);
        livePosition->setTimeOfStroke(_timeOfStroke);
        livePosition->setStroke(_stroke);
        livePosition->setDepth(_depth);
        livePosition->setSensation(_sensation);    

        if (_taskStreamingHandle == NULL) {
            // Create Stroke Task
            xTaskCreatePinnedToCore(
                this->_streamingImpl,    // Function that should be called
                "Streaming",             // Name of the task (for debugging)
                4096,                   // Stack size (bytes)
                this,                   // Pass reference to this class instance
                24,                     // Pretty high task priority
                &_taskStreamingHandle,   // Task handle
                1                       // Pin to application core
            ); 
        } else {
            // Resume task, if it already exists
            vTaskResume(_taskStreamingHandle);
        }

#ifdef DEBUG_TALKATIVE
        Serial.println("Started streaming task");
        Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

        return true;

    } else {

#ifdef DEBUG_TALKATIVE
        Serial.println("Failed to start streaming");
#endif
        return false;

    }
}

void StrokeEngine::stopMotion() {
    // only valid when 
    if (_state == PATTERN || _state == SETUPDEPTH || _state == STREAMING) {
        // Set state
        _state = READY;

        // Stop servo motor as fast as legally allowed
        livePosition->clear();
        servo->setAcceleration(_maxStepAcceleration);
        servo->applySpeedAcceleration();
        servo->stopMove();

#ifdef DEBUG_TALKATIVE
        Serial.println("Motion stopped");
#endif

        // Wait for servo stopped
        while (servo->isRunning());

        // Send telemetry data
        if (_callbackTelemetry != NULL) {
            _callbackTelemetry(float(servo->getCurrentPosition() / _motor->stepsPerMillimeter), 0.0, false);
        }
    }
    
#ifdef DEBUG_TALKATIVE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif
}

void StrokeEngine::enableAndHome(endstopProperties *endstop, void(*callBackHoming)(bool), float speed) {
    // Store callback
    _callBackHomeing = callBackHoming;

    // enable and home
    enableAndHome(endstop, speed);
}

void StrokeEngine::enableAndHome(endstopProperties *endstop, float speed) {
    // set homing pin as input
    _sensorlessHomeing = false;
    _homeingPin = endstop->endstopPin;
    pinMode(_homeingPin, endstop->pinMode);
    _homeingActiveLow = endstop->activeLow;
    _homeingSpeed = speed * _motor->stepsPerMillimeter;

    // set homing direction so sign can be multiplied
    if (endstop->homeToBack == true) {
        _homeingToBack = 1;
    } else {
        _homeingToBack = -1;
    }

    // first stop current motion and delete stroke task
    stopMotion();

    // Enable Servo
    servo->enableOutputs();

    // Create homing task
    xTaskCreatePinnedToCore(
        this->_homingProcedureImpl,     // Function that should be called
        "Homing",                       // Name of the task (for debugging)
        4096,                           // Stack size (bytes)
        this,                           // Pass reference to this class instance
        20,                             // Pretty high task priority
        &_taskHomingHandle,             // Task handle
        1                               // Have it on application core
    ); 
#ifdef DEBUG_TALKATIVE
    Serial.println("Homing task started");
#endif

}

void StrokeEngine::enableAndSensorlessHome(sensorlessHomeProperties *sensorless, void(*callBackHoming)(bool), float speed) {
    // Store callback
    _callBackHomeing = callBackHoming;

    // enable and sensorless home
    enableAndSensorlessHome(sensorless, speed);
}

void StrokeEngine::enableAndSensorlessHome(sensorlessHomeProperties *sensorless, float speed) {
    _sensorlessHomeing = true;
    _homeingSpeed = speed * _motor->stepsPerMillimeter;
    _sensorlessHomeingCurrentPin = sensorless->currentPin;
    pinMode(_sensorlessHomeingCurrentPin, INPUT);
    _sensorlessHomeingCurrentLimit = sensorless->currentLimit;

    // first stop current motion and delete stroke task
    stopMotion();

    // Create homing task
    xTaskCreatePinnedToCore(
        this->_homingProcedureImpl,     // Function that should be called
        "SensorlessHoming",             // Name of the task (for debugging)
        2048,                           // Stack size (bytes)
        this,                           // Pass reference to this class instance
        20,                             // Pretty high task priority
        &_taskHomingHandle,             // Task handle
        1                               // Have it on application core
    ); 
#ifdef DEBUG_TALKATIVE
    Serial.println("Sensorless homing task started");
#endif

}

void StrokeEngine::thisIsHome(float speed) {
    // set homeing speed
    _homeingSpeed = speed * _motor->stepsPerMillimeter;

    if (_state == UNDEFINED) {
        // Enable Servo
        servo->enableOutputs();

        // Stet current position as home
        servo->setCurrentPosition(-_motor->stepsPerMillimeter * _physics->keepoutBoundary);

        // Set feedrate for homing
        servo->setSpeedInHz(_homeingSpeed);       
        servo->setAcceleration(_maxStepAcceleration / 10);

        // drive free of switch and set axis to 0
        servo->moveTo(_minStep);
        
        // Change state
        _isHomed = true;
        _state = READY;

#ifdef DEBUG_TALKATIVE
        Serial.println("This is Home now");
#endif

        return;
    }

#ifdef DEBUG_TALKATIVE
    Serial.println("Manual homing failed. Not in state UNDEFINED");
#endif

}

// Speed here is in mm/s
bool StrokeEngine::moveTo(float position, float speed, bool blocking) {

#ifdef DEBUG_TALKATIVE
    Serial.println("Move to max");
#endif

    if (_isHomed) {
        // Stop motion immediately
        stopMotion();

        // Set feedrate for safe move 
        // Constrain speed between 1 step/sec and _maxStepPerSecond
        servo->setSpeedInHz(constrain(speed * _motor->stepsPerMillimeter, 1, _maxStepPerSecond));
        servo->setAcceleration(_maxStepAcceleration / 10);
        position = constrain(position, _minStep, _maxStep);
        servo->moveTo(position, blocking);

        // Send telemetry data
        if (_callbackTelemetry != NULL) {
            _callbackTelemetry(float(position / _motor->stepsPerMillimeter), speed, false);
        } 

#ifdef DEBUG_TALKATIVE
        Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

        // Return success
        return true;

    } else {
        // Return failure
        return false;
    }
}


bool StrokeEngine::moveToMax(float speed, bool blocking) {

#ifdef DEBUG_TALKATIVE
    Serial.println("Move to max");
#endif

    if (_isHomed) {
        // Stop motion immediately
        stopMotion();

        // Set feedrate for safe move 
        // Constrain speed between 1 step/sec and _maxStepPerSecond
        servo->setSpeedInHz(constrain(speed * _motor->stepsPerMillimeter, 1, _maxStepPerSecond));
        servo->setAcceleration(_maxStepAcceleration / 10);
        servo->moveTo(_maxStep, blocking);

        // Send telemetry data
        if (_callbackTelemetry != NULL) {
            _callbackTelemetry(float(_maxStep / _motor->stepsPerMillimeter), speed, false);
        } 

#ifdef DEBUG_TALKATIVE
        Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

        // Return success
        return true;

    } else {
        // Return failure
        return false;
    }
}

bool StrokeEngine::moveToMin(float speed, bool blocking) {

#ifdef DEBUG_TALKATIVE
    Serial.println("Move to min");
#endif

    if (_isHomed) {
        // Stop motion immediately
        stopMotion();

        // Set feedrate for safe move 
        // Constrain speed between 1 step/sec and _maxStepPerSecond
        servo->setSpeedInHz(constrain(speed * _motor->stepsPerMillimeter, 1, _maxStepPerSecond));
        servo->setAcceleration(_maxStepAcceleration / 10);
        servo->moveTo(_minStep, blocking);

        // Send telemetry data
        if (_callbackTelemetry != NULL) {
            _callbackTelemetry(float(_minStep / _motor->stepsPerMillimeter), speed, false);
        } 

#ifdef DEBUG_TALKATIVE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

        // Return success
        return true;

    } else {
        // Return failure
        return false;
    }
}

bool StrokeEngine::setupDepth(float speed, bool fancy) {
#ifdef DEBUG_TALKATIVE
    Serial.println("Move to Depth");
#endif
    // store fanciness
    _fancyAdjustment = fancy;

    // returns true on success, and false if in wrong state
    bool allowed = false;

    // isHomed is only true in states READY, PATTERN and SETUPDEPTH
    if (_isHomed) {
        // Stop motion immediately
        stopMotion();

        // Set feedrate for safe move 
        // Constrain speed between 1 step/sec and _maxStepPerSecond
        servo->setSpeedInHz(constrain(speed * _motor->stepsPerMillimeter, 1, _maxStepPerSecond));
        servo->setAcceleration(_maxStepAcceleration / 10);

        // Set new state
        _state = SETUPDEPTH;

        // move to current depth position
        _setupDepths();

        // set return value to true
        allowed = true;
    }
#ifdef DEBUG_TALKATIVE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif
    return allowed;
}

ServoState StrokeEngine::getState() {
    return _state;
}

void StrokeEngine::disable() {
    // Delete homing Task
    _abortHoming = true;
    while (_taskHomingHandle != NULL) {
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    _abortHoming = false;

    _state = UNDEFINED;
    _isHomed = false;

    // Disable servo motor
    servo->disableOutputs();

    // Delete homing Task
    if (_taskHomingHandle != NULL) {
        vTaskDelete(_taskHomingHandle);
        _taskHomingHandle = NULL;
    }

#ifdef DEBUG_TALKATIVE
    Serial.println("Servo disabled. Call home to continue.");
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

}

String StrokeEngine::getPatternName(int index) {
    if (index >= 0 && index <= patternTableSize) {
        return String(patternTable[index]->getName());
    } else {
        return String("Invalid");
    }
    
}

void StrokeEngine::setMaxSpeed(float maxSpeed){
    // Update pattern with new speed limits
    if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {
        // Convert speed into steps
        _maxStepPerSecond = int(0.5 + _motor->maxSpeed * _motor->stepsPerMillimeter);
        patternTable[_patternIndex]->setSpeedLimit(_maxStepPerSecond, _maxStepAcceleration, _motor->stepsPerMillimeter);
        xSemaphoreGive(_patternMutex);
    }
}

void StrokeEngine::setPhysicalTravel(float travel) {
    if (_state != UNDEFINED || _isHomed) {
        return; // don't allow adjusting physical travel after homing has been completed
    }
    _physics->physicalTravel = travel;
    _travel = (_physics->physicalTravel - (2 * _physics->keepoutBoundary));
    _maxStep = int(0.5 + _travel * _motor->stepsPerMillimeter);
}

float StrokeEngine::getMaxSpeed() {
    return float(_maxStepPerSecond / _motor->stepsPerMillimeter);
}

void StrokeEngine::setMaxAcceleration(float maxAcceleration) {

    // Update pattern with new speed limits
    if (xSemaphoreTake(_patternMutex, portMAX_DELAY) == pdTRUE) {
        // Convert acceleration into steps
        _maxStepAcceleration = int(0.5 + _motor->maxAcceleration * _motor->stepsPerMillimeter);
        patternTable[_patternIndex]->setSpeedLimit(_maxStepPerSecond, _maxStepAcceleration, _motor->stepsPerMillimeter);
        xSemaphoreGive(_patternMutex);
    }    
}

float StrokeEngine::getMaxAcceleration() {
    return float(_maxStepAcceleration / _motor->stepsPerMillimeter);
}

float StrokeEngine::getMaxDepth() {
    return _travel;
}

void StrokeEngine::registerTelemetryCallback(void(*callbackTelemetry)(float, float, bool)) {
    _callbackTelemetry = callbackTelemetry;
}

float StrokeEngine::_getAnalogAveragePercent(int pinNumber, int samples) {
    float sum = 0;
    float average = 0;
    float percentage = 0;
    for (int i = 0; i < samples; i++)
    {
        // TODO: Possibly use fancier filters?
        sum += analogRead(pinNumber);
    }
    average = sum / samples;
    // TODO: Might want to add a deadband
    percentage = 100.0 * average / 4096.0; // 12 bit resolution
    return percentage;
}

void StrokeEngine::_homingProcedure() {
    if(_sensorlessHomeing) {
        _sensorlessHomingProcedure();
    } else {
        _sensorHomingProcedure();
    }

    _taskHomingHandle = NULL;
    vTaskDelete(NULL);
}

void StrokeEngine::_sensorlessHomingProcedure() {
#ifdef DEBUG_TALKATIVE
    Serial.println("Finding Home Sensorless");
#endif
    float currentSensorOffset = (_getAnalogAveragePercent(_sensorlessHomeingCurrentPin, 1000));
    float current = _getAnalogAveragePercent(_sensorlessHomeingCurrentPin, 200) - currentSensorOffset;
    long lastMillisMessage = 0;

    // Set feedrate for homing
    servo->setSpeedInHz(_homeingSpeed);
    servo->setAcceleration(_maxStepAcceleration / 10);

    // disable motor briefly in case we are against a hard stop.
    servo->disableOutputs();
    vTaskDelay(600 / portTICK_PERIOD_MS);
    if(_abortHoming) return;
    servo->enableOutputs();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if(_abortHoming) return;

#ifdef DEBUG_TALKATIVE
    Serial.print(_getAnalogAveragePercent(_sensorlessHomeingCurrentPin, 500) - currentSensorOffset);
    Serial.print(",");
    Serial.println(servo->getCurrentPosition() / _motor->stepsPerMillimeter);

    Serial.println("Sensorless homing move");
#endif
    servo->runForward();

    current = _getAnalogAveragePercent(_sensorlessHomeingCurrentPin, 200) - currentSensorOffset;
    while (current < _sensorlessHomeingCurrentLimit)
    {
        if(_abortHoming) return;
        current = _getAnalogAveragePercent(_sensorlessHomeingCurrentPin, 25) - currentSensorOffset;
#ifdef DEBUG_TALKATIVE
        if(millis() - lastMillisMessage > 200) {
            Serial.print(current);
            Serial.print(",");
            Serial.println(servo->getCurrentPosition());
            lastMillisMessage = millis();
        }
#endif

        // Let other tasks run
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    servo->forceStopAndNewPosition(0);
#ifdef DEBUG_TALKATIVE
    Serial.println("Sensorless found max");
#endif

    servo->runBackward();

    vTaskDelay(300 / portTICK_PERIOD_MS);

    current = _getAnalogAveragePercent(_sensorlessHomeingCurrentPin, 200) - currentSensorOffset;
    while (current < _sensorlessHomeingCurrentLimit)
    {
        if(_abortHoming) return;
        current = _getAnalogAveragePercent(_sensorlessHomeingCurrentPin, 25) - currentSensorOffset;
#ifdef DEBUG_TALKATIVE
        if(millis() - lastMillisMessage > 200) {
            Serial.print(current);
            Serial.print(",");
            Serial.println(servo->getCurrentPosition());
            lastMillisMessage = millis();
        }
#endif
        // Let other tasks run
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    if(_abortHoming) return;

    _physics->physicalTravel = abs(servo->getCurrentPosition()) / _motor->stepsPerMillimeter;
    _travel = (_physics->physicalTravel - (2 * _physics->keepoutBoundary));
    _maxStep = int(0.5 + _travel * _motor->stepsPerMillimeter);
    servo->forceStopAndNewPosition(-_motor->stepsPerMillimeter * _physics->keepoutBoundary);
    
#ifdef DEBUG_TALKATIVE
    Serial.printf("Found rail length: %f\n", _physics->physicalTravel);
#endif

    servo->moveTo(0);

    _isHomed = true;
    _state = READY;

#ifdef DEBUG_TALKATIVE
        Serial.println("Homing succeeded");
#endif

    // Call notification callback, if it was defined.
    if (_callBackHomeing != NULL) {
        _callBackHomeing(_isHomed);
    }

    // Set first point for telemetry
    if (_callbackTelemetry != NULL) {
        _callbackTelemetry(0.0, 0.0, false);
    }

#ifdef DEBUG_TALKATIVE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif
}

void StrokeEngine::_sensorHomingProcedure() {
    // Set feedrate for homing
    servo->setSpeedInHz(_homeingSpeed);       
    servo->setAcceleration(_maxStepAcceleration / 10);    

    // Check if we are already at the homing switch
    if (digitalRead(_homeingPin) == !_homeingActiveLow) {
        //back off 5 mm from switch
        servo->move(_motor->stepsPerMillimeter * 2 * _physics->keepoutBoundary * _homeingToBack);

        // wait for move to complete
        while (servo->isRunning()) {
            if(_abortHoming) return;
            // Pause the task for 100ms while waiting for move to complete
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        // move back towards endstop
        servo->move(-_motor->stepsPerMillimeter * 4 * _physics->keepoutBoundary * _homeingToBack);

    } else {
        // Move MAX_TRAVEL towards the homing switch
        servo->move(-_motor->stepsPerMillimeter * _physics->physicalTravel * _homeingToBack);
    }

    // Poll homing switch
    while (servo->isRunning()) {
        if(_abortHoming) return;
        // Switch is active low
        if (digitalRead(_homeingPin) == !_homeingActiveLow) {

            // Set home position
            if (_homeingToBack == 1) {
                //Switch is at -KEEPOUT_BOUNDARY
                servo->forceStopAndNewPosition(-_motor->stepsPerMillimeter * _physics->keepoutBoundary);

                // drive free of switch and set axis to lower end
                servo->moveTo(_minStep);

            } else {
                servo->forceStopAndNewPosition(_motor->stepsPerMillimeter * (_physics->physicalTravel - _physics->keepoutBoundary));

                // drive free of switch and set axis to front end
                servo->moveTo(_maxStep);
            }
            _isHomed = true;

            // drive free of switch and set axis to 0
            servo->moveTo(0);
            
            // Break loop, home was found
            break;
        }

        // Pause the task for 20ms to allow other tasks
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    
    // disable Servo if homing has not found the homing switch
    if (!_isHomed) {
        servo->disableOutputs();
        _state = UNDEFINED;

#ifdef DEBUG_TALKATIVE
        Serial.println("Homing failed");
#endif

    } else {
        // Set state to ready
        _state = READY;

#ifdef DEBUG_TALKATIVE
        Serial.println("Homing succeeded");
#endif
    }

    // Call notification callback, if it was defined.
    if (_callBackHomeing != NULL) {
        _callBackHomeing(_isHomed);
    }

    // Set first point for telemetry
    if (_callbackTelemetry != NULL) {
        _callbackTelemetry(0.0, 0.0, false);
    }

#ifdef DEBUG_TALKATIVE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

    // delete one-time task
    _taskHomingHandle = NULL;
    vTaskDelete(NULL);
}

void StrokeEngine::_stroking() {
    motionParameter currentMotion;

    while(1) { // infinite loop

        // Suspend task, if not in PATTERN state
        if (_state != PATTERN) {
            vTaskSuspend(_taskStrokingHandle);
        }

        // Take mutex to ensure no interference / race condition with communication threat on other core
        if (xSemaphoreTake(_patternMutex, 0) == pdTRUE) {

            if (_applyUpdate == true) {
                // Ask pattern for update on motion parameters
                currentMotion = patternTable[_patternIndex]->nextTarget(_index);
            
                // Increase deceleration if required to avoid crash
                if (servo->getAcceleration() > currentMotion.acceleration) {
#ifdef DEBUG_CLIPPING
                    Serial.print("Crash avoidance! Set Acceleration from " + String(currentMotion.acceleration));
                    Serial.println(" to " + String(servo->getAcceleration()));
#endif
                    currentMotion.acceleration = servo->getAcceleration();
                }

                // Apply new trapezoidal motion profile to servo
                _applyMotionProfile(&currentMotion);

                // clear update flag
                _applyUpdate = false;
            }

            // If motor has stopped issue moveTo command to next position
            else if (servo->isRunning() == false) {

                // Increment index for pattern
                _index++;

                // Querey new set of pattern parameters
                currentMotion = patternTable[_patternIndex]->nextTarget(_index);

                // Pattern may introduce pauses between strokes
                if (currentMotion.skip == false) {

#ifdef DEBUG_STROKE
                    Serial.println("Stroking Index: " + String(_index));
#endif
                    // Apply new trapezoidal motion profile to servo
                    _applyMotionProfile(&currentMotion);

                } else {
                    // decrement _index so that it stays the same until the next valid stroke parameters are delivered
                    _index--;
                }
            }

            // give back mutex
            xSemaphoreGive(_patternMutex);
        }
        
        // Delay 10ms 
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void StrokeEngine::_streaming() {
    motionParameter currentMotion;

    while(1) { // infinite loop

        // Suspend task, if not in STREAMING state
        if (_state != STREAMING) {
            vTaskSuspend(_taskStreamingHandle);
        }

        // Take mutex to ensure no interference / race condition with communication threat on other core
        if (xSemaphoreTake(_patternMutex, 0) == pdTRUE) {

            if (_applyUpdate == true) {
                // Ask pattern for update on motion parameters
                currentMotion = livePosition->nextTarget(_index);
            
                // Increase deceleration if required to avoid crash
                if (servo->getAcceleration() > currentMotion.acceleration) {
#ifdef DEBUG_CLIPPING
                    Serial.print("Crash avoidance! Set Acceleration from " + String(currentMotion.acceleration));
                    Serial.println(" to " + String(servo->getAcceleration()));
#endif
                    currentMotion.acceleration = servo->getAcceleration();
                }

                // Apply new trapezoidal motion profile to servo
                _applyMotionProfile(&currentMotion);

                // clear update flag
                _applyUpdate = false;
            }

            // If motor has stopped issue moveTo command to next position
            else if (servo->isRunning() == false) {

                // Increment index for pattern
                _index++;

                // Querey new set of pattern parameters
                currentMotion = livePosition->nextTarget(_index);

                // Pattern may introduce pauses between strokes
                if (currentMotion.skip == false) {

#ifdef DEBUG_STROKE
                    Serial.println("Stroking Index: " + String(_index));
#endif
                    // Apply new trapezoidal motion profile to servo
                    _applyMotionProfile(&currentMotion);

                } else {
                    // decrement _index so that it stays the same until the next valid stroke parameters are delivered
                    _index--;
                }
            }

            // give back mutex
            xSemaphoreGive(_patternMutex);
        }
        
        // Delay 10ms 
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void StrokeEngine::_applyMotionProfile(motionParameter* motion) {

    bool clipping = false;
    float speed = 0.0;
    float position = 0.0;

    // Apply new trapezoidal motion profile to servo if pattern does not skip
    if (motion->skip == false) {

        // Constrain speed to below _maxStepPerSecond
        if (motion->speed > _maxStepPerSecond) {
#ifdef DEBUG_CLIPPING
        Serial.println("Max Speed Exceeded: " + String(float(motion->speed / _motor->stepsPerMillimeter), 2)
                + "mm/s --> Limit: " + String(float(_maxStepPerSecond / _motor->stepsPerMillimeter), 2) + "mm/s");
#endif
            motion->speed = _maxStepPerSecond;
            clipping = true;
        } 

        // Constrain acceleration between 1 step/sec^2 and _maxStepAcceleration
        if (motion->acceleration > _maxStepAcceleration) {
#ifdef DEBUG_CLIPPING
        Serial.println("Max Acceleration Exceeded: " + String(float(motion->acceleration / _motor->stepsPerMillimeter), 2)
                + "mm/s² --> Limit: " + String(float(_maxStepAcceleration / _motor->stepsPerMillimeter), 2) + "mm/s²");
#endif
            motion->acceleration = _maxStepAcceleration;
            clipping = true;
        } 

        // Constrain stroke to motion envelope
        int pos = constrain((motion->stroke), _minStep, _maxStep);

        // write values to servo
        servo->setSpeedInHz(motion->speed);
        servo->setAcceleration(motion->acceleration);
        servo->moveTo(pos);

        // Compile speed telemetry data
        speed = float(motion->speed / _motor->stepsPerMillimeter);
        position = float(pos / _motor->stepsPerMillimeter);

#ifdef DEBUG_STROKE
    Serial.println("motion.stroke: " + String(position, 2) + "mm");
    Serial.println("motion.speed: " + String(speed, 2) + "mm/s");
    Serial.println("motion.acceleration: " + String(float(motion->acceleration / _motor->stepsPerMillimeter), 2) + "mm/s²");
#endif

        // Send telemetry data
        if (_callbackTelemetry != NULL) {
            _callbackTelemetry(position, speed, clipping);
        }
    }
}

void StrokeEngine::_setupDepths() {
    // set depth to _depth
    int depth = _depth;

    // in fancy mode we need to calculate exact position based on sensation, stroke & depth
    if (_fancyAdjustment == true) {
        // map sensation into the interval [depth-stroke, depth]
        depth = map(_sensation, -100, 100, _depth - _stroke, _depth);

#ifdef DEBUG_TALKATIVE
        Serial.println("map sensation " + String(_sensation)
            + " to interval [" + String(_depth - _stroke)
            + ", " + String(_depth) 
            + "] = " + String(depth));
#endif
    } 

    // move servo to desired position
    servo->moveTo(depth);

    // Send telemetry data
    if (_callbackTelemetry != NULL) {
        _callbackTelemetry(float(depth / _motor->stepsPerMillimeter), 
            float(servo->getSpeedInMilliHz() * 1000 / _motor->stepsPerMillimeter), 
            false);
    } 

#ifdef DEBUG_TALKATIVE
    Serial.println("setup new depth: " + String(depth));
#endif
}

