#include <pattern.h>
#include <CircularBuffer.h>

class Movement {
    public:
        Movement() {};
        Movement(unsigned int position, unsigned int time) {
            _position = position;
            _time = time;
        }
        ~Movement() {};
        
        unsigned int position() {
            return _position;
        }
        unsigned int time() {
            return _time;
        }
    private:
        unsigned int _position;
        unsigned int _time;
};

class LivePosition : public Pattern {
    public:
        LivePosition() : Pattern("") {}

        void addPosition(unsigned int position, unsigned int time) {
            pendingMovements.push(new Movement(position, time));
        }
        void clear() {
            pendingMovements.clear();
        }

        void setTimeOfStroke(float speed = 0) { 
             // N/A
        }

        motionParameter nextTarget(int index) {
            if (pendingMovements.isEmpty()) { // no more pending movements
                _nextMove.skip = true;
            } else if (_currentMovement == NULL) { // function tried to re-use last index but we have no last movement stored
                _nextMove.skip = true;
            } else {
                // pull the next position + time value from the circular buffer and set StrokeEngine to move to it
                _currentMovement = pendingMovements.shift();
                _timeOfStroke = constrain(_currentMovement->time() / 1000.0, 0.01, 120.0); // seconds to complete a half stroke
                int newPos = _currentMovement->position() * (_depth - (_depth - _stroke)) / 100 + (_depth - _stroke); // convert from 0-100 to StrokeEngine stroke value
                int distance = abs(_lastPos - newPos); // TODO: this distance is incorrect when applyNow is true, need to know actual current position
                _nextMove.stroke = newPos;

                // maximum speed of the trapezoidal motion 
                _nextMove.speed = int(1.5 * (distance/_timeOfStroke));
                
                // acceleration to meet the profile
                _nextMove.acceleration = int(3.0 * _nextMove.speed/_timeOfStroke);
                _nextMove.skip = false;
                _lastPos = newPos;
            }

            _index = index;
            return _nextMove;
        }
    private:
        CircularBuffer<Movement*, 10> pendingMovements;
        Movement* _currentMovement;
        int _lastPos = 0;
};

static LivePosition* livePosition = new LivePosition();