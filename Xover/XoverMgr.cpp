/*

This file is part of Arduino Turnout
Copyright (C) 2017-2018 Eric Thorstenson

Arduino Turnout is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Arduino Turnout is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.

*/

#include "XoverMgr.h"


// ========================================================================================================
// Public Methods


// TurnoutMgr constructor
XoverMgr::XoverMgr()
{
	// set pointer to this instance of the turnout manager, so that we can reference it in callbacks
	currentInstance = this;

	// configure sensor/servo event handlers
	button.SetButtonPressHandler(WrapperButtonPress);
	osAB.SetButtonPressHandler(WrapperOSAB);
	osCD.SetButtonPressHandler(WrapperOSCD);

	// configure dcc event handlers
	dcc.SetBasicAccessoryDecoderPacketHandler(WrapperDCCAccPacket);
	dcc.SetExtendedAccessoryDecoderPacketHandler(WrapperDCCExtPacket);
	dcc.SetBasicAccessoryPomPacketHandler(WrapperDCCAccPomPacket);
	dcc.SetBitstreamMaxErrorHandler(WrapperMaxBitErrors);
	dcc.SetPacketMaxErrorHandler(WrapperMaxPacketErrors);
	dcc.SetDecodingErrorHandler(WrapperDCCDecodingError);

	// configure timer event handlers
	errorTimer.SetTimerHandler(WrapperErrorTimer);
	resetTimer.SetTimerHandler(WrapperResetTimer);
	servoTimer.SetTimerHandler(WrapperServoTimer);

	// configure servo event handlers
	for (byte i = 0; i < numServos; i++)
		servo[i].SetServoMoveDoneHandler(WrapperServoMoveDone);
}


// Check for factory reset, then proceed with main initialization
void XoverMgr::Initialize()
{
	// check for button hold on startup (for reset to defaults)
	if (button.RawState() == LOW)
	{
		// disable button/occupancy sensor handlers
		button.SetButtonPressHandler(0);
		osAB.SetButtonPressHandler(0);
		osCD.SetButtonPressHandler(0);

		FactoryReset(true);    // perform a complete reset
	}
	else
	{
		InitMain();
	}
}


// update sensors and outputs
void XoverMgr::Update()
{
	// do all the updates that TurnoutBase handles
	TurnoutBase::Update();

	// then update our sensors and servo
	const unsigned long currentMillis = millis();
	osAB.Update(currentMillis);
	osCD.Update(currentMillis);

	if (servosActive)
		for (byte i = 0; i < numServos; i++)
			servo[i].Update(currentMillis);
}


// Initialize the turnout manager by setting up the dcc config, reading stored values from CVs, and setting up the servo
void XoverMgr::InitMain()
{
	// do the init stuff in TurnoutBase
	TurnoutBase::InitMain();

	// servo setup - get extents, rates, and last position from cv's
	const int lowSpeed = cv.getCV(CV_servoLowSpeed) * 100;
	const int highSpeed = cv.getCV(CV_servoHighSpeed) * 100;
	servo[0].Initialize(cv.getCV(CV_servo1MinTravel), cv.getCV(CV_servo1MaxTravel), lowSpeed, highSpeed, servoState[0][position]);
	servo[1].Initialize(cv.getCV(CV_servo2MinTravel), cv.getCV(CV_servo2MaxTravel), lowSpeed, highSpeed, servoState[1][position]);
	servo[2].Initialize(cv.getCV(CV_servo3MinTravel), cv.getCV(CV_servo3MaxTravel), lowSpeed, highSpeed, servoState[2][position]);
	servo[3].Initialize(cv.getCV(CV_servo4MinTravel), cv.getCV(CV_servo4MaxTravel), lowSpeed, highSpeed, servoState[3][position]);

	// set led and relays, and begin bitstream capture
	EndServoMove();

#ifdef _DEBUG
	Serial.println("TurnoutMgr init done.");
#endif
}


// set the turnout to a new position
void XoverMgr::BeginServoMove()
{
	// store new position to cv
	cv.setCV(CV_turnoutPosition, position);
	SaveConfig();

	// set the led to indicate servo is in motion
	led.SetLED((position == STRAIGHT) ? RgbLed::GREEN : RgbLed::RED, RgbLed::FLASH);

	// stop the bitstream capture
	dcc.SuspendBitstream();

	// turn off the relays
	for (byte i = 0; i < numServos; i++)
		relay[i].SetPin(LOW);

	// start pwm for current positions of all servos
	for (byte i = 0; i < numServos; i++)
		servo[i].StartPWM();

	// turn on servo power
	servoPower.SetPin(HIGH);

	// set the servo index to the first servo and start moving the servos in sequence
	servosActive = true;
	currentServo = 0;
	ServoMoveDoneHandler();
}


// resume normal operation after servo motion is complete
void XoverMgr::EndServoMove()
{
	// set the led solid for the current position
	led.SetLED((position == STRAIGHT) ? RgbLed::GREEN : RgbLed::RED, RgbLed::ON);

	// turn off servo power
	//servoPower.SetPin(LOW);

	// stop pwm all servos
	//for (byte i = 0; i < numServos; i++)
	//	servo[i].StopPWM();

	// enable the appropriate relay
	for (byte i = 0; i < numServos; i++)
		relay[i].SetPin(relayState[i][position]);

	// resume the bitstream capture
	servosActive = false;
	dcc.ResumeBitstream();
}





// ========================================================================================================
// Event Handlers


// handle the reset timer callback
void XoverMgr::ResetTimerHandler()
{
	// enable button/occupancy sensor handlers
	button.SetButtonPressHandler(WrapperButtonPress);
	osAB.SetButtonPressHandler(WrapperOSAB);
	osCD.SetButtonPressHandler(WrapperOSCD);

	// run the main init after the reset timer expires
	InitMain();
}


// do things after the servo finishes moving to its new position
void XoverMgr::ServoMoveDoneHandler()
{
	if (currentServo < numServos)
	{
#ifdef _DEBUG
		Serial.print("Setting servo ");
		Serial.print(currentServo, DEC);
		Serial.print(" to ");
		Serial.print(servoState[currentServo][position], DEC);
		Serial.print(" at rate ");
		Serial.println(servoRate, DEC);
#endif

		servo[currentServo].Set(servoState[currentServo][position], servoRate);
		currentServo++;
	}
	else
	{
		const int servoPowerOffDelay = 500;    // ms
		servoTimer.StartTimer(servoPowerOffDelay);
	}
}


// handle a button press
void XoverMgr::ButtonEventHandler(bool ButtonState)
{
	// check button state (HIGH so we respond after button release)
	if (ButtonState == HIGH)
	{
		// proceed only if both occupancy sensors are inactive (i.e., sensors override button press)
		if (osAB.SwitchState() == HIGH && osCD.SwitchState() == HIGH)
		{
			// toggle from current position and set new position
			position = (State)!position;
			servoRate = LOW;
			BeginServoMove();
		}
		else
		{
			// button error indication, normal led will resume after this timer expires
			errorTimer.StartTimer(1000);
			led.SetLED(RgbLed::YELLOW, RgbLed::ON);
		}
	}
}


void XoverMgr::OSABHandler(bool ButtonState)
{
}

void XoverMgr::OSCDHandler(bool ButtonState)
{
}


// handle a DCC basic accessory command, used for changing the state of the turnout
void XoverMgr::DCCAccCommandHandler(unsigned int Addr, unsigned int Direction)
{
	// assume we are filtering repeated packets in the packet builder, so we don't check for that here
	// assume DCCdecoder is set to return only packets for this decoder's address.

	State dccState = (Direction == 0) ? CURVED : STRAIGHT;
	if (dccCommandSwap) dccState = (State)!dccState; // swap the interpretation of dcc command if needed

	// if we are already in the desired position, just exit
	if (dccState == position) return;

#ifdef _DEBUG
	Serial.print("Received dcc command to position ");
	Serial.println(dccState, DEC);
#endif

	// proceed only if both occupancy sensors are inactive (i.e., sensors override dcc command)
	if (osAB.SwitchState() == HIGH && osCD.SwitchState() == HIGH)
	{
		// set switch state based on dcc command
		position = dccState;
		servoRate = LOW;
		BeginServoMove();
	}
	else
	{
		// command error indication, normal led will resume after this timer expires
		errorTimer.StartTimer(1000);
		led.SetLED(RgbLed::YELLOW, RgbLed::FLASH);
	}
}


// handle a DCC program on main command
void XoverMgr::DCCPomHandler(unsigned int Addr, byte instType, unsigned int CV, byte Value)
{
	// do most of the program on main stuff in TurnoutBase
	TurnoutBase::DCCPomHandler(Addr, instType, CV, Value);

	// update servo vars from eeprom
	if (CV == CV_servo1MinTravel) servo[0].SetExtent(LOW, cv.getCV(CV_servo1MinTravel));
	if (CV == CV_servo1MaxTravel) servo[0].SetExtent(HIGH, cv.getCV(CV_servo1MaxTravel));

	if (CV == CV_servo2MinTravel) servo[1].SetExtent(LOW, cv.getCV(CV_servo2MinTravel));
	if (CV == CV_servo2MaxTravel) servo[1].SetExtent(HIGH, cv.getCV(CV_servo2MaxTravel));

	if (CV == CV_servo3MinTravel) servo[2].SetExtent(LOW, cv.getCV(CV_servo3MinTravel));
	if (CV == CV_servo3MaxTravel) servo[2].SetExtent(HIGH, cv.getCV(CV_servo3MaxTravel));

	if (CV == CV_servo4MinTravel) servo[3].SetExtent(LOW, cv.getCV(CV_servo4MinTravel));
	if (CV == CV_servo4MaxTravel) servo[3].SetExtent(HIGH, cv.getCV(CV_servo4MaxTravel));
	
	if (CV == CV_servoLowSpeed)
		for (byte i = 0; i < numServos; i++)
			servo[i].SetDuration(LOW, cv.getCV(CV_servoLowSpeed) * 100);
	if (CV == CV_servoHighSpeed)
		for (byte i = 0; i < numServos; i++)
			servo[i].SetDuration(HIGH, cv.getCV(CV_servoHighSpeed) * 100);
}


// ========================================================================================================

XoverMgr *XoverMgr::currentInstance = 0;    // pointer to allow us to access member objects from callbacks

												// servo/sensor callback wrappers
void XoverMgr::WrapperButtonPress(bool ButtonState) { currentInstance->ButtonEventHandler(ButtonState); }
void XoverMgr::WrapperOSAB(bool ButtonState) { currentInstance->OSABHandler(ButtonState); }
void XoverMgr::WrapperOSCD(bool ButtonState) { currentInstance->OSCDHandler(ButtonState); }
void XoverMgr::WrapperServoMoveDone() { currentInstance->ServoMoveDoneHandler(); }


// ========================================================================================================
// dcc processor callback wrappers

void XoverMgr::WrapperDCCAccPacket(int boardAddress, int outputAddress, byte activate, byte data)
{
	currentInstance->DCCAccCommandHandler(outputAddress, data);
}

void XoverMgr::WrapperDCCExtPacket(int boardAddress, int outputAddress, byte data)
{
	currentInstance->DCCExtCommandHandler(outputAddress, data);
}

void XoverMgr::WrapperDCCAccPomPacket(int boardAddress, int outputAddress, byte instructionType, int cv, byte data)
{
	currentInstance->DCCPomHandler(outputAddress, instructionType, cv, data);
}

void XoverMgr::WrapperMaxBitErrors(byte errorCode) { currentInstance->TurnoutBase::MaxBitErrorHandler(); }
void XoverMgr::WrapperMaxPacketErrors(byte errorCode) { currentInstance->TurnoutBase::MaxPacketErrorHandler(); }
void XoverMgr::WrapperDCCDecodingError(byte errorCode) { currentInstance->TurnoutBase::DCCDecodingError(); }


// timer callback wrappers
void XoverMgr::WrapperResetTimer() { currentInstance->ResetTimerHandler(); }
void XoverMgr::WrapperErrorTimer() { currentInstance->ErrorTimerHandler(); }
void XoverMgr::WrapperServoTimer() { currentInstance->EndServoMove(); }
