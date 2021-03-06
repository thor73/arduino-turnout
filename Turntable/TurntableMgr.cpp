
#include "TurntableMgr.h"

// static pointer for callbacks
TurntableMgr* TurntableMgr::currentInstance = 0;

#if defined(ADAFRUIT_METRO_M0_EXPRESS)
FlashStorage(flashConfig, TurntableMgr::ConfigVars);
FlashStorage(flashState, TurntableMgr::StateVars);
#endif

TurntableMgr::TurntableMgr()
{
	// pointer for callback functions
	currentInstance = this;
}

void TurntableMgr::Initialize()
{
	// configure factory default CVs
	byte index = 0;
	index = configCVs.initCV(index, CV_AddressLSB, 50);
	index = configCVs.initCV(index, CV_AddressMSB, 0);
	index = configCVs.initCV(index, CV_WarmupTimeout, 5, 0, 30);
	configCVs.initCV16(index, CV_IdleTimeout, 600, 0, 1200);

	// configure default siding positions (set up to match current layout)
	index = 0;
	index = sidingCVs.initCV16(index, 1, 21472, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 2, 6640, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 3, 5056, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 4, 3488, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 5, 1888, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 6, 256, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 7, 27472, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 8, 8288, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 9, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 10, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 11, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 12, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 13, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 14, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 15, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 16, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 17, 0, 0, 180U * stepsPerDegree);
	index = sidingCVs.initCV16(index, 18, 0, 0, 180U * stepsPerDegree);

	// load config and last saved state
	LoadState();
	LoadConfig();

	// setup the stepper
	ConfigureStepper();

	// configure callbacks
	idleTimer.SetTimerHandler(WrapperIdleTimerHandler);
	warmupTimer.SetTimerHandler(WrapperWarmupTimerHandler);
	errorTimer.SetTimerHandler(WrapperErrorTimerHandler);

	#if defined(WITH_DCC)
	// Configure and initialize the DCC packet processor
	byte addr = (configCVs.getCV(CV_AddressMSB) << 8) + configCVs.getCV(CV_AddressLSB);
	dcc.SetAddress(addr);

	// configure dcc event handlers
	dcc.SetBasicAccessoryDecoderPacketHandler(WrapperDCCAccPacket);
	dcc.SetExtendedAccessoryDecoderPacketHandler(WrapperDCCExtPacket);
	dcc.SetBasicAccessoryPomPacketHandler(WrapperDCCAccPomPacket);
	dcc.SetBitstreamMaxErrorHandler(WrapperMaxBitErrors);
	dcc.SetPacketMaxErrorHandler(WrapperMaxPacketErrors);
	dcc.SetDecodingErrorHandler(WrapperDCCDecodingError);
	#endif

	#if defined(WITH_TOUCHSCREEN)
	touchpad.Init();
	touchpad.SetGraphicButtonHandler(WrapperGraphicButtonHandler);
	touchpad.SetButtonPress(currentSiding, true);    // set display to show initial siding on startup
	#endif // defined(WITH_TOUCHSCREEN)

	// start the state machine
	currentState = IDLE;
	currentStateFunction = &TurntableMgr::StateIdle;
}


void TurntableMgr::Update()
{
	// perform the current state function
	if (ttStateFunctions[currentState])
		(*this.*ttStateFunctions[currentState])();
}


//  state transition functions  ===================================================================

void TurntableMgr::StateIdle()
{
	if (subState == 0)     // transition to state
	{
		// TODO: go to full step before powering off?

		afStepper->release();                // power off the stepper
		flasher.SetLED(RgbLed::OFF);         // turn off the warning light

		#if defined(WITH_DCC)
		dcc.ResumeBitstream();               // resume listening for dcc commands
		#endif // defined(WITH_DCC)

		subState = 1;
	}

	// do the update functions for this state

	#if defined(WITH_DCC)
	dcc.ProcessTimeStamps();
	#endif // defined(WITH_DCC)

	#if defined(WITH_TOUCHSCREEN)
	touchpad.Update();
	#endif // defined(WITH_TOUCHSCREEN)

	errorTimer.Update();
}


void TurntableMgr::StateMoving()
{
	if (subState == 0)     // transition to state
	{

		#if defined(WITH_DCC)
		dcc.SuspendBitstream();
		#endif   // WITH_DCC

		flasher.SetLED(RgbLed::RED, RgbLed::FLASH, 500, 500);

		// move to the specified siding at normal speed
		accelStepper.setMaxSpeed(stepperMaxSpeed);
		accelStepper.setAcceleration(stepperAcceleration);
		MoveToSiding();

		subState = 1;
	}

	// do the update functions for this state
	uint32_t currentMillis = millis();
	accelStepper.run();
	flasher.Update(currentMillis);

	#if defined(WITH_TOUCHSCREEN)
	touchpad.Update();
	#endif // defined(WITH_TOUCHSCREEN)

	if (accelStepper.distanceToGo() == 0)    // move is done
	{
		SaveState();    // TODO: add checks here to minimize flash/eeprom writes
						// TODO: need to save current siding, but this is the wrong place to save state
		RaiseEvent(MOVE_DONE);
	}
}

void TurntableMgr::StateSeek()
{
	switch (subState)
	{
	case 0:                 // transition to seek state

		#if defined(WITH_DCC)
		dcc.SuspendBitstream();
		#endif

		// start moving in a complete clockwise circle at normal speed
		accelStepper.setMaxSpeed(stepperMaxSpeed / 2);
		accelStepper.setAcceleration(stepperAcceleration);
		accelStepper.move(360U * stepsPerDegree);

		flasher.SetLED(RgbLed::RED, RgbLed::OFF);

		subState = 1;
		break;
	case 1:    // CW rotation, waiting for hall sensor to go low, indicating magnet has entered its detection area
			   // TODO: investigate why debouncing is needed, rather than using falling/rising irq combo
		if (!hallSensor.SwitchState())
		{
			attachInterrupt(digitalPinToInterrupt(hallSensorPin), HallIrq, RISING);
			flasher.SetLED(RgbLed::RED, RgbLed::ON);
			subState = 2;   // found it, go to next state
		}
		break;
	case 2:    // CW rotation, waiting for IRQ callback to set subState = 3
		break;
	case 3:    // now we've swung past in the CW direction and our home position is set (in the IRQ)
		flasher.SetLED(RgbLed::RED, RgbLed::OFF);
		detachInterrupt(digitalPinToInterrupt(hallSensorPin));
		accelStepper.stop();     // set the stepper to stop with its current speed/accel settings
		subState = 4;            // indicate successful sensor detection
		break;
	}

	// once we stop, check if we found home and set it
	if (accelStepper.distanceToGo() == 0)
	{
		if (subState == 4)   // we successfully found both edges of the hall sensor and have a valid home position
		{
			// now that we have stopped, set our home position
			int16_t delta = accelStepper.currentPosition() - homePosition;
			accelStepper.setCurrentPosition(delta);
		}
		else
		{
			// we went a full circle without detecting the correct edge of the hall sensor
			errorTimer.StartTimer(5000);
			flasher.SetLED(RgbLed::RED, RgbLed::FLASH, 250, 250);
		}

		RaiseEvent(MOVE_DONE);
	}

	// do the update functions for this state
	hallSensor.Update();
	accelStepper.run();
}


void TurntableMgr::StateCalibrate()
{
	if (subState == 0)     // transition to state
	{

		flasher.SetLED(RgbLed::RED, RgbLed::FLASH, 500, 500);

		#if defined(WITH_DCC)
		dcc.SuspendBitstream();
		#endif   // WITH_DCC

		accelStepper.setMaxSpeed(stepperMaxSpeed);
		accelStepper.setAcceleration(10 * stepperAcceleration);

		subState = 1;
	}

	// do the update functions for this state
	uint32_t currentMillis = millis();
	accelStepper.run();
	flasher.Update(currentMillis);

	#if defined(WITH_TOUCHSCREEN)
	touchpad.Update();
	#endif // defined(WITH_TOUCHSCREEN)

	// if the current move is done, then set up the next cal move, or exit
	if (accelStepper.distanceToGo() == 0)
	{
		switch (calCmd.type)
		{
		case CalCmd::none:
			RaiseEvent(MOVE_DONE);
			break;
		case CalCmd::continuous:
			accelStepper.move(calCmd.calSteps);
			break;
		case CalCmd::incremental:
			accelStepper.move(calCmd.calSteps);
			calCmd.type = CalCmd::none;
			break;
		default:
			break;
		}
	}
}


void TurntableMgr::StatePowered()
{
	if (subState == 0)     // transition to state
	{

		flasher.SetLED(RgbLed::ON);         // turn off the warning light

		#if defined(WITH_DCC)
		dcc.ResumeBitstream();               // resume listening for dcc commands
		#endif // deinfed(WITH_DCC)

		// start the timer for the transition to idle state
		idleTimer.StartTimer(1000UL * configCVs.getCV(CV_IdleTimeout));
		subState = 1;
	}

	// do the update functions for this state

	#if defined(WITH_DCC)
	dcc.ProcessTimeStamps();
	#endif // deinfed(WITH_DCC)

	#if defined(WITH_TOUCHSCREEN)
	touchpad.Update();
	#endif // defined(WITH_TOUCHSCREEN)

	uint32_t currentMillis = millis();
	flasher.Update(currentMillis);
	idleTimer.Update(currentMillis);
	errorTimer.Update(currentMillis);
}

void TurntableMgr::StateWarmup()
{
	if (subState == 0)     // transition to state
	{

		#if defined(WITH_DCC)
		dcc.SuspendBitstream();
		#endif   // WITH_DCC

		flasher.SetLED(RgbLed::RED, RgbLed::FLASH, 500, 500);

		// start the timer for the transition to moving state
		warmupTimer.StartTimer(1000UL * configCVs.getCV(CV_WarmupTimeout));
		flasher.SetLED(RgbLed::RED, RgbLed::FLASH);
		subState = 1;
	}

	// do the update functions for this state
	uint32_t currentMillis = millis();
	flasher.Update(currentMillis);
	warmupTimer.Update(currentMillis);
}


void TurntableMgr::RaiseEvent(const ttEvent event)
{
	byte i = 0;
	const byte n = sizeof(stateTransMatrix) / sizeof(stateTransMatrixRow);

	// loop through state transition table until we find matching state and event
	while ((i < n) && !(currentState == stateTransMatrix[i].currState && event == stateTransMatrix[i].event)) i++;

	// if there was no match just return
	if (i == n)	return;

	// otherwise transition to next state specified in the matching row
	currentState = stateTransMatrix[i].nextState;
	currentStateFunction = ttStateFunctions[currentState];
	subState = 0;
}



//  local functions   ===========================================================================

void TurntableMgr::ConfigureStepper()
{
	// motorshield and stepper objects
	motorShield = Adafruit_MotorShield();
	afStepper = motorShield.getStepper(stepperStepsPerRev, motorShieldPort);
	accelStepper = AccelStepper(StepperClockwiseStep, StepperCounterclockwiseStep);

	// adafruit motorshield setup
	motorShield.begin();                            // create with the default PWM frequency 1.6KHz
	//TWBR = ((F_CPU / 400000l) - 16) / 2;        // change I2C clock to 400kHz
	Wire.setClock(400000l);                       // TODO: check that this really works on AVR and ARM

	// accelstepper setup
	accelStepper.setMaxSpeed(stepperMaxSpeed);
	accelStepper.setAcceleration(stepperAcceleration);

	// set stepper position to correspond to current siding
	accelStepper.setCurrentPosition(sidingCVs.getCV(currentSiding));
}


void TurntableMgr::MoveToSiding()
{
	// get stepper position for desired siding
	const int32_t targetPos = moveCmd.targetPos;  // put into int32 for later calcs

	// get current stepper position in positive half circle equivalent
	const uint16_t currentPos = FindBasicPosition(accelStepper.currentPosition());

	// steps needed for move (go in opposite direction if ouside +/-90 deg)
	int32_t moveSteps = targetPos - currentPos;
	if (moveSteps > 90L * stepsPerDegree) moveSteps -= 180L * stepsPerDegree;
	if (moveSteps < -90L * stepsPerDegree) moveSteps += 180L * stepsPerDegree;

	// update steps for reverse move if needed
	if (moveCmd.type == MoveCmd::reverse)
	{
		if (moveSteps > 0) 
			moveSteps -= 180L * stepsPerDegree;
		else
			moveSteps += 180L * stepsPerDegree;
	}

	moveCmd.type = MoveCmd::normal;    // reset for normal move if it was reversed

	// do the move
	if (moveSteps != 0)
		accelStepper.move(moveSteps);
	else
	{   // workaround to apply holding torque without moving (much...)
		StepperClockwiseStep();
		StepperCounterclockwiseStep();
	}

	previousSiding = currentSiding;   // for debug purposes
}


// convert stepper motor position to a positive position in the first half circle of rotation
uint16_t TurntableMgr::FindBasicPosition(int32_t pos)
{
	int32_t p = pos;
	const uint16_t halfCircleSteps = 180 * stepsPerDegree;

	if (p < 0)     // convert negative motor position
	{
		byte n = -p / halfCircleSteps;
		p += (1 + n) * halfCircleSteps;
	}

	return p % halfCircleSteps;
}

// return the nearest full step to the current position, in microsteps
int32_t TurntableMgr::FindFullStep(int32_t microsteps)
{
	byte remainder = microsteps % stepperMicroSteps;

	if (remainder < 8)
	{
		return microsteps - remainder;
	}
	else
	{
		return microsteps - remainder + stepperMicroSteps;
	}
}



// event handlers and static wrappers

void TurntableMgr::WrapperIdleTimerHandler() { currentInstance->RaiseEvent(IDLETIMER); }
void TurntableMgr::WrapperWarmupTimerHandler() { currentInstance->RaiseEvent(WARMUPTIMER); }
void TurntableMgr::WrapperErrorTimerHandler() {	currentInstance->flasher.SetLED(RgbLed::OFF); }

void TurntableMgr::WrapperGraphicButtonHandler(byte buttonID, bool state)
{
	currentInstance->CommandHandler(buttonID, state);
}

void TurntableMgr::WrapperDCCAccPacket(int boardAddress, int outputAddress, byte activate, byte data)
{
	if (data == 1) currentInstance->CommandHandler(1, true);    // accessory command for siding 1
}

void TurntableMgr::WrapperDCCExtPacket(int boardAddress, int outputAddress, byte data)
{
	currentInstance->CommandHandler(data, true);
}

void TurntableMgr::WrapperDCCAccPomPacket(int boardAddress, int outputAddress, byte instructionType, int cv, byte data)
{
	currentInstance->DCCPomHandler(outputAddress, instructionType, cv, data);
}

void TurntableMgr::WrapperMaxBitErrors(byte errorCode)
{
}

void TurntableMgr::WrapperMaxPacketErrors(byte errorCode)
{
}

void TurntableMgr::WrapperDCCDecodingError(byte errorCode)
{
}


void TurntableMgr::SaveState()
{
	stateVars.currentState = currentState;
	stateVars.currentSiding = currentSiding;
	stateVars.isValid = true;

	#if defined(ADAFRUIT_METRO_M0_EXPRESS)
	flashState.write(stateVars);
	#else
	EEPROM.put(0, stateVars);
	#endif
}

void TurntableMgr::LoadState()
{
	// determine if first boot or not
	#if defined(ADAFRUIT_METRO_M0_EXPRESS)
	StateVars tempStateVars = flashState.read();
	const bool firstBoot = !tempStateVars.isValid;  // this is false on read of unitialized flash
	#else
	const bool firstBoot = (EEPROM.read(0) == 255);
	#endif
	
	// if not first boot, then load stored state, otherwise use default statevars initialization below
	if (!firstBoot)   
	{
		#if defined(ADAFRUIT_METRO_M0_EXPRESS)
		stateVars = tempStateVars;
		#else
		EEPROM.get(0, stateVars);    // get the last state from eeprom
		#endif // defined(ADAFRUIT_METRO_M0_EXPRESS)
	}

	// set state and siding locals
	currentState = stateVars.currentState;
	currentSiding = stateVars.currentSiding;
}

void TurntableMgr::SaveConfig()
{
	// copy working CVs to our storage object
	for (byte i = 0; i < numCVindexes; i++)
		configVars.CVs[i] = configCVs.cv[i].cvValue;
	for (byte i = 0; i < numSidingIndexes; i++)
		configVars.sidingSteps[i] = sidingCVs.cv[i].cvValue;

	// store the config
	#if defined(ADAFRUIT_METRO_M0_EXPRESS)
	flashConfig.write(configVars);
	#else
	EEPROM.put(sizeof(stateVars), configVars);   // store the config vars struct starting after the state vars in EEPROM
	#endif
}

void TurntableMgr::LoadConfig()
{
	LoadConfig(false);
}

void TurntableMgr::LoadConfig(bool reset)
{
	// if this is the first boot on fresh eeprom/flash, or reset requested
	// NOTE: must load state from flash before loading config so that stateVars is set properly
	if (!stateVars.isValid || reset)   
	{

		//load default values for config vars and sidings
		configCVs.resetCVs();
		sidingCVs.resetCVs();

		SaveState();
		SaveConfig();
	}
	else   // normal boot, load config from eeprom/flash
	{

		#if defined(ADAFRUIT_METRO_M0_EXPRESS)
		configVars = flashConfig.read();
		#else
		EEPROM.get(sizeof(stateVars), configVars);   // load the config vars struct starting after the state vars in EEPROM
		#endif

		// copy stored config to working CVs
		for (byte i = 0; i < numCVindexes; i++)
			configCVs.cv[i].cvValue = configVars.CVs[i];
		for (byte i = 0; i < numSidingIndexes; i++)
			sidingCVs.cv[i].cvValue = configVars.sidingSteps[i];
	}
}

void TurntableMgr::SetSidingCal()
{
	// update the siding position in our cv struct
	long pos = accelStepper.currentPosition();
	uint16_t basicPos = FindBasicPosition(pos);
	int32_t fullstepPos = FindFullStep(basicPos);
	sidingCVs.setCV(currentSiding, fullstepPos);

	// store the turntable state and cv struct to nvram
	SaveConfig();
}


void TurntableMgr::HallIrq()
{
	currentInstance->homePosition = FindFullStep(currentInstance->accelStepper.currentPosition());
	currentInstance->subState = 3;   //  for seek state 3 to begin slowing down
}


void TurntableMgr::CommandHandler(byte buttonID, bool state)
{

	if (state)        // button was pressed or dcc command received
	{
		switch (buttonID)
		{
		case Touchpad::numpad1:
		case Touchpad::numpad2:
		case Touchpad::numpad3:
		case Touchpad::numpad4:
		case Touchpad::numpad5:
		case Touchpad::numpad6:
		case Touchpad::numpad7:
		case Touchpad::numpad8:
		case Touchpad::numpad9:
		case Touchpad::numpad10:
		case Touchpad::numpad11:
		case Touchpad::numpad12:
		case Touchpad::numpad13:
		case Touchpad::numpad14:
		case Touchpad::numpad15:
		case Touchpad::numpad16:
		case Touchpad::numpad17:
		case Touchpad::numpad18:
			currentSiding = buttonID;
			moveCmd.targetPos = sidingCVs.getCV(currentSiding);

			#if defined(WITH_TOUCHSCREEN)
			touchpad.SetButtonPress(currentSiding, true);
			#endif

			RaiseEvent(BUTTON_SIDING);
			break;
		case Touchpad::modeRun1:
		case Touchpad::modeRun2:
			#if defined(WITH_TOUCHSCREEN)
			touchpad.SetButtonPress(currentSiding, true);
			#endif
			break;
		case Touchpad::modeSetup:
			break;
		case Touchpad::runReverse:
			moveCmd.type = MoveCmd::reverse;  // this gets reset after the reverse move is complete

			#if defined(WITH_TOUCHSCREEN)
			touchpad.SetButtonPress(Touchpad::runReverse, true);
			#endif
			break;

		// calibration commands
		case Touchpad::setupStepCW:
			calCmd.type = CalCmd::continuous;
			calCmd.calSteps = stepperMicroSteps;
			RaiseEvent(BUTTON_CAL);
			break;
		case Touchpad::setupStepCCW:
			calCmd.type = CalCmd::continuous;
			calCmd.calSteps = -1L * stepperMicroSteps;
			RaiseEvent(BUTTON_CAL);
			break;
		case Touchpad::setup10CW:
			calCmd.type = CalCmd::incremental;
			calCmd.calSteps = 10 * stepsPerDegree;
			RaiseEvent(BUTTON_CAL);
			break;
		case Touchpad::setup10CCW:
			calCmd.type = CalCmd::incremental;
			calCmd.calSteps = -10L * stepsPerDegree;
			RaiseEvent(BUTTON_CAL);
			break;
		case Touchpad::setup30CW:
			calCmd.type = CalCmd::incremental;
			calCmd.calSteps = 30 * stepsPerDegree;
			RaiseEvent(BUTTON_CAL);
			break;
		case Touchpad::setup30CCW:
			calCmd.type = CalCmd::incremental;
			calCmd.calSteps = -30L * stepsPerDegree;
			RaiseEvent(BUTTON_CAL);
			break;
		case Touchpad::setup90CW:
			calCmd.type = CalCmd::incremental;
			calCmd.calSteps = 90 * stepsPerDegree;
			RaiseEvent(BUTTON_CAL);
			break;
		case Touchpad::setup90CCW:
			calCmd.type = CalCmd::incremental;
			calCmd.calSteps = -90L * stepsPerDegree;
			RaiseEvent(BUTTON_CAL);
			break;


		case Touchpad::setupSet:
			SetSidingCal();
			break;
		case Touchpad::setupHome:
			RaiseEvent(BUTTON_SEEK);
			break;
		case Touchpad::estop:
			RaiseEvent(BUTTON_ESTOP);
			break;
		default:
			break;
		}
	}
	else             // button was released (n/a for dcc commands)
	{
		switch (buttonID)
		{
		case Touchpad::setupStepCW:
		case Touchpad::setupStepCCW:
			calCmd.type = CalCmd::none;
		default:
			break;
		}
	}
}


void TurntableMgr::StepperClockwiseStep()
{
	currentInstance->afStepper->onestep(FORWARD, MICROSTEP);
}

void TurntableMgr::StepperCounterclockwiseStep()
{
	currentInstance->afStepper->onestep(BACKWARD, MICROSTEP);
}

void TurntableMgr::DCCPomHandler(unsigned int Addr, byte instType, unsigned int CV, byte Value)
{
	// assume we are filtering repeated packets in the packet builder, so we don't check for that here
	// assume DCCdecoder is set to return only packets for this decoder's address.

	// TODO: check for and perform reset

	// set the cv
	if (configCVs.setCV(CV,Value))
	{
		errorTimer.StartTimer(250);
		flasher.SetLED(RgbLed::RED, RgbLed::FLASH, 50, 50);
	}
	else
	{
		errorTimer.StartTimer(1000);
		flasher.SetLED(RgbLed::RED, RgbLed::FLASH, 250, 250);
	}

	#if defined(WITH_DCC)
	// update dcc address
	byte addr = (configCVs.getCV(CV_AddressMSB) << 8) + configCVs.getCV(CV_AddressLSB);
	dcc.SetAddress(addr);
	#endif

	// save the new config
	SaveConfig();
}
