/****************************************************
 * OpenLRSng transmitter code
 ****************************************************/
uint8_t RF_channel = 0;

uint8_t FSstate = 0; // 1 = waiting timer, 2 = send FS, 3 sent waiting btn release
uint32_t FStime = 0;  // time when button went down...

uint32_t lastSent = 0;

uint32_t lastTelemetry = 0;

uint8_t RSSI_rx = 0;
uint8_t RSSI_tx = 0;
uint8_t RX_ain0 = 0;
uint8_t RX_ain1 = 0;
uint32_t sampleRSSI = 0;

uint16_t linkQuality = 0;
uint16_t linkQualityRX = 0;

volatile uint8_t ppmAge = 0; // age of PPM data

volatile uint8_t ppmCounter = PPM_CHANNELS; // ignore data until first sync pulse

uint8_t serialMode = 0; // 0 normal, 1 spektrum 1024 , 2 spektrum 2048, 3 SBUS

#ifndef BZ_FREQ
#define BZ_FREQ 2000
#endif

/****************************************************
 * Interrupt Vector
 ****************************************************/

static inline void processPulse(uint16_t pulse)
{
  if (serialMode) {
    return;
  }

  if (!(bind_data.flags & MICROPPM)) {
    pulse >>= 1; // divide by 2 to get servo value on normal PPM
  }

  if (pulse > 2500) {      // Verify if this is the sync pulse (2.5ms)
    ppmCounter = 0;             // -> restart the channel counter
    ppmAge = 0;                 // brand new PPM data received
  } else if ((pulse > 700) && (ppmCounter < PPM_CHANNELS)) { // extra channels will get ignored here
    PPM[ppmCounter++] = servoUs2Bits(pulse);   // Store measured pulse length (converted)
  } else {
    ppmCounter = PPM_CHANNELS; // glitch ignore rest of data
  }
}

#ifdef USE_ICP1 // Use ICP1 in input capture mode
volatile uint16_t startPulse = 0;
ISR(TIMER1_CAPT_vect)
{
  uint16_t stopPulse = ICR1;
  processPulse(stopPulse - startPulse); // as top is 65535 uint16 math will take care of rollover
  startPulse = stopPulse;         // Save time at pulse start
}

void setupPPMinput()
{
  // Setup timer1 for input capture (PSC=8 -> 0.5ms precision)
  TCCR1A = ((1 << WGM10) | (1 << WGM11));
  TCCR1B = ((1 << WGM12) | (1 << WGM13) | (1 << CS11) | (1 <<ICNC1));
  // normally capture on rising edge, allow invertting via SW flag
  if (!(bind_data.flags & INVERTED_PPMIN)) {
    TCCR1B |= (1 << ICES1);
  }
  OCR1A = 65535;
  TIMSK1 |= (1 << ICIE1);   // Enable timer1 input capture interrupt
}

#else // sample PPM using pinchange interrupt
ISR(PPM_Signal_Interrupt)
{
  uint16_t pulseWidth;
  if ( (bind_data.flags & INVERTED_PPMIN) ^ PPM_Signal_Edge_Check) {
    pulseWidth = TCNT1; // read the timer1 value
    TCNT1 = 0; // reset the timer1 value for next
    processPulse(pulseWidth);
  }
}

void setupPPMinput(void)
{
  // Setup timer1 for input capture (PSC=8 -> 0.5ms precision)
  TCCR1A = ((1 << WGM10) | (1 << WGM11));
  TCCR1B = ((1 << WGM12) | (1 << WGM13) | (1 << CS11));
  OCR1A = 65535;
  TIMSK1 = 0;
  PPM_Pin_Interrupt_Setup
}
#endif

void bindMode(void)
{
  uint32_t prevsend = millis();
  uint8_t  tx_buf[sizeof(bind_data) + 1];
  boolean  sendBinds = 1;

  init_rfm(1);

  while (Serial.available()) {
    Serial.read();    // flush serial
  }

  while (1) {
    if (sendBinds & (millis() - prevsend > 200)) {
      prevsend = millis();
      Green_LED_ON;
      buzzerOn(BZ_FREQ);
      tx_buf[0] = 'b';
      memcpy(tx_buf + 1, &bind_data, sizeof(bind_data));
      tx_packet(tx_buf, sizeof(bind_data) + 1);
      Green_LED_OFF;
      buzzerOff();
      RF_Mode = Receive;
      rx_reset();
      delay(50);
      if (RF_Mode == Received) {
        RF_Mode = Receive;
        spiSendAddress(0x7f);   // Send the package read command
        if ('B' == spiReadData()) {
          sendBinds = 0;
        }
      }
    }

    if (!digitalRead(BTN)) {
      sendBinds = 1;
    }

    while (Serial.available()) {
      switch (Serial.read()) {
      case '\n':
      case '\r':
        Serial.println(F("Enter menu..."));
        handleCLI();
        break;
      case '#':
        scannerMode();
        break;
      case 'B':
        binaryMode();
        break;
      default:
        break;
      }
    }
  }
}

void checkButton(void)
{
  uint32_t time, loop_time;

  if (digitalRead(BTN) == 0) {     // Check the button
    delay(200);   // wait for 200mS with buzzer ON
    buzzerOff();

    time = millis();  //set the current time
    loop_time = time;

    while (millis() < time + 4800) {
      if (digitalRead(BTN)) {
        goto just_bind;
      }
    }

    // Check the button again, If it is still down reinitialize
    if (0 == digitalRead(BTN)) {
      int8_t bzstate = HIGH;
      uint8_t swapProfile = 0;

      buzzerOn(bzstate?BZ_FREQ:0);
      loop_time = millis();

      while (0 == digitalRead(BTN)) {     // wait for button to release
        if (loop_time > time + 9800) {
          buzzerOn(BZ_FREQ);
          swapProfile = 1;
        } else {
          if ((millis() - loop_time) > 200) {
            loop_time = millis();
            bzstate = !bzstate;
            buzzerOn(bzstate ? BZ_FREQ : 0);
          }
        }
      }

      buzzerOff();
      if (swapProfile) {
        profileSwap((activeProfile + 1) % TX_PROFILE_COUNT);
        Serial.print("New profile:");
        Serial.println(activeProfile);
        if (bindReadEeprom()) {
          Serial.println("Loaded settings from EEPROM\n");
        } else {
          Serial.print("EEPROM data not valid, reiniting\n");
          bindInitDefaults();
          bindWriteEeprom();
        }
        return;
      }
      bindRandomize();
      bindWriteEeprom();
      bindPrint();
    }
just_bind:
    // Enter binding mode, automatically after recoding or when pressed for shorter time.
    Serial.println("Entering binding mode\n");
    bindMode();
  }
}

void checkBND(void)
{
  if ((Serial.available() > 3) &&
      (Serial.read() == 'B') && (Serial.read() == 'N') &&
      (Serial.read() == 'D') && (Serial.read() == '!')) {
    buzzerOff();
    bindMode();
  }
}

void checkFS(void)
{

  switch (FSstate) {
  case 0:
    if (!digitalRead(BTN)) {
      FSstate = 1;
      FStime = millis();
    }

    break;

  case 1:
    if (!digitalRead(BTN)) {
      if ((millis() - FStime) > 1000) {
        FSstate = 2;
        buzzerOn(BZ_FREQ);
      }
    } else {
      FSstate = 0;
    }

    break;

  case 2:
    if (digitalRead(BTN)) {
      buzzerOff();
      FSstate = 0;
    }

    break;
  }
}

uint8_t tx_buf[21];
uint8_t rx_buf[9];

#define SERIAL_BUFSIZE 32
uint8_t serial_buffer[SERIAL_BUFSIZE];
uint8_t serial_resend[9];
uint8_t serial_head;
uint8_t serial_tail;
uint8_t serial_okToSend; // 2 if it is ok to send serial instead of servo

void setup(void)
{
  uint32_t start;
  setupSPI();
#ifdef SDN_pin
  pinMode(SDN_pin, OUTPUT); //SDN
  digitalWrite(SDN_pin, 0);
#endif
  //LED and other interfaces
  pinMode(Red_LED, OUTPUT); //RED LED
  pinMode(Green_LED, OUTPUT); //GREEN LED
#ifdef Red_LED2
  pinMode(Red_LED2, OUTPUT); //RED LED
  pinMode(Green_LED2, OUTPUT); //GREEN LED
#endif
  pinMode(BTN, INPUT); //Buton
  pinMode(PPM_IN, INPUT); //PPM from TX
  digitalWrite(PPM_IN, HIGH); // enable pullup for TX:s with open collector output
#if defined (RF_OUT_INDICATOR)
  pinMode(RF_OUT_INDICATOR, OUTPUT);
  digitalWrite(RF_OUT_INDICATOR, LOW);
#endif
  buzzerInit();

  Serial.begin(115200);
  profileInit();
  if (bindReadEeprom()) {
    Serial.println("Loaded settings from EEPROM\n");
  } else {
    Serial.print("EEPROM data not valid, reiniting\n");
    bindInitDefaults();
    bindWriteEeprom();
  }

  setupPPMinput();
  ppmAge = 255;

  setupRfmInterrupt();

  sei();

  start = millis();
  while ((ppmAge == 255) && ((millis() - start) < 2000));

  buzzerOn(BZ_FREQ);
  digitalWrite(BTN, HIGH);
  Red_LED_ON ;

  while (Serial.available()) {
    Serial.read();
  }

  Serial.print("OpenLRSng TX starting ");
  printVersion(version);
  Serial.print(" on HW ");
  Serial.println(BOARD_TYPE);

  delay(200);
  checkBND();

  if (bind_data.serial_baudrate && (bind_data.serial_baudrate < 4)) {
    serialMode = bind_data.serial_baudrate;
    TelemetrySerial.begin((serialMode == 3) ? 100000 : 115200);
  } else {
    // switch to userdefined baudrate here
    TelemetrySerial.begin(bind_data.serial_baudrate);
  }
  checkButton();

  Red_LED_OFF;
  buzzerOff();

  setupPPMinput(); // need to do this to make sure ppm polarity is correct if profile was changed

  init_rfm(0);
  rfmSetChannel(RF_channel);
  rx_reset();

  serial_head = 0;
  serial_tail = 0;
  serial_okToSend = 0;

  delay(300);
  buzzerOn(BZ_FREQ);
  delay(100);
  buzzerOff();
  for (uint8_t i = 0; i < activeProfile; i++) {
    delay(100);
    buzzerOn(BZ_FREQ);
    delay(100);
    buzzerOff();
  }

  if (bind_data.flags & TELEMETRY_FRSKY) {
    frskyInit((bind_data.flags & TELEMETRY_MASK) == TELEMETRY_SMARTPORT);
  } else if (bind_data.flags & TELEMETRY_MASK) {
    // ?
  }
}

uint8_t compositeRSSI(uint8_t rssi, uint8_t linkq)
{
  if (linkq >= 15) {
    // RSSI 0 - 255 mapped to 192 - ((255>>2)+192) == 192-255
    return (rssi >> 2) + 192;
  } else {
    // linkquality gives 0 to 14*13 == 182
    return linkq * 13;
  }
}

#define SBUS_SYNC 0x0f
#define SBUS_TAIL 0x00
struct sbus_dat {
  uint16_t ch0 : 11;
  uint16_t ch1 : 11;
  uint16_t ch2 : 11;
  uint16_t ch3 : 11;
  uint16_t ch4 : 11;
  uint16_t ch5 : 11;
  uint16_t ch6 : 11;
  uint16_t ch7 : 11;
  uint16_t ch8 : 11;
  uint16_t ch9 : 11;
  uint16_t ch10 : 11;
  uint16_t ch11 : 11;
  uint16_t ch12 : 11;
  uint16_t ch13 : 11;
  uint16_t ch14 : 11;
  uint16_t ch15 : 11;
  uint16_t res  : 15;
  uint8_t  status;
} __attribute__ ((__packed__));

union serial_msg {
  uint8_t  bytes[24];
  uint16_t words[24];
  struct sbus_dat sbus;
} frame;

#define SPKTRM_SYNC1 0x03
#define SPKTRM_SYNC2 0x01

uint8_t frameIndex=0;

void processSpektrum(uint8_t c)
{
  if (frameIndex == 0) {
    if (c == SPKTRM_SYNC1) {
      frameIndex++;
    }
  } else if (frameIndex == 1) {
    if (c == SPKTRM_SYNC2) {
      frameIndex++;
    } else {
      frameIndex = 0;
    }
  } else if (frameIndex < 16) {
    frame.bytes[frameIndex++] = c;
    if (frameIndex==16) { // frameComplete
      for (uint8_t i=1; i<8; i++) {
        uint8_t ch,v;
        if (serialMode == 1) {
          ch = frame.words[i] >> 10;
          v = frame.words[i] & 0x3ff;
        } else {
          ch = frame.words[i] >> 11;
          v = (frame.words[i] & 0x7ff)>>1;
        }
        if (ch<16) {
          PPM[ch] = v;
        }
        ppmAge=0;
      }
    }
  } else {
    frameIndex=0;
  }
}

void processSBUS(uint8_t c)
{
  if (frameIndex == 0) {
    if (c == SBUS_SYNC) {
      frameIndex++;
    }
  } else if (frameIndex < 24) {
    frame.bytes[(frameIndex++)-1] = c;
  } else {
    if ((frameIndex == 24) && (c==SBUS_TAIL)) {
      PPM[0] = frame.sbus.ch0;
      PPM[1] = frame.sbus.ch1;
      PPM[2] = frame.sbus.ch2;
      PPM[3] = frame.sbus.ch3;
      PPM[4] = frame.sbus.ch4;
      PPM[5] = frame.sbus.ch5;
      PPM[6] = frame.sbus.ch6;
      PPM[7] = frame.sbus.ch7;
      PPM[8] = frame.sbus.ch8;
      PPM[9] = frame.sbus.ch9;
      PPM[10] = frame.sbus.ch10;
      PPM[11] = frame.sbus.ch11;
      PPM[12] = frame.sbus.ch12;
      PPM[13] = frame.sbus.ch13;
      PPM[14] = frame.sbus.ch14;
      PPM[15] = frame.sbus.ch15;
      ppmAge=0;
    }
    frameIndex = 0;
  }
}

void processChannelsFromSerial(uint8_t c)
{
  if ((serialMode == 1) || (serialMode == 2)) { // SPEKTRUM
    processSpektrum(c);
  } else if (serialMode==2) { // SBUS
    processSBUS(c);
  }
}

void loop(void)
{
  if (spiReadRegister(0x0C) == 0) {     // detect the locked module and reboot
    Serial.println("module locked?");
    Red_LED_ON;
    init_rfm(0);
    rx_reset();
    Red_LED_OFF;
  }

  while (TelemetrySerial.available()) {
    if (serialMode) {
      processChannelsFromSerial(TelemetrySerial.read());
    } else if (((serial_tail + 1) % SERIAL_BUFSIZE) != serial_head) {
      serial_buffer[serial_tail] = TelemetrySerial.read();
      serial_tail = (serial_tail + 1) % SERIAL_BUFSIZE;
    }
  }

  if (RF_Mode == Received) {
    // got telemetry packet
    lastTelemetry = micros();
    if (!lastTelemetry) {
      lastTelemetry = 1; //fixup rare case of zero
    }
    linkQuality |= 1;
    RF_Mode = Receive;
    spiSendAddress(0x7f); // Send the package read command
    for (int16_t i = 0; i < 9; i++) {
      rx_buf[i] = spiReadData();
    }

    if ((tx_buf[0] ^ rx_buf[0]) & 0x40) {
      tx_buf[0] ^= 0x40; // swap sequence to ack
      if ((rx_buf[0] & 0x38) == 0x38) {
        uint8_t i;
        // transparent serial data...
        for (i = 0; i<= (rx_buf[0] & 7);) {
          i++;
          if (bind_data.flags & TELEMETRY_FRSKY) {
            frskyUserData(rx_buf[i]);
          } else {
            TelemetrySerial.write(rx_buf[i]);
          }
        }
      } else if ((rx_buf[0] & 0x3F) == 0) {
        RSSI_rx = rx_buf[1];
        RX_ain0 = rx_buf[2];
        RX_ain1 = rx_buf[3];
#ifdef TEST_DUMP_AFCC
#define SIGNIT(x) ((int16_t)(((x&0x200)?0xFC00U:0)|(x&0x3FF)))
        Serial.print(SIGNIT(rfmGetAFCC()));
        Serial.print(':');
        Serial.println(SIGNIT((rx_buf[4] << 8) + rx_buf[5]));
#endif
        linkQualityRX = rx_buf[6];
      }
    }
    if (serial_okToSend == 1) {
      serial_okToSend = 2;
    }
    if (serial_okToSend == 3) {
      serial_okToSend = 0;
    }
  }

  uint32_t time = micros();

  if ((sampleRSSI) && ((time - sampleRSSI) >= 3000)) {
    RSSI_tx = rfmGetRSSI();
    sampleRSSI = 0;
  }

  if ((time - lastSent) >= getInterval(&bind_data)) {
    lastSent = time;

    if (ppmAge < 8) {
      ppmAge++;

      if (lastTelemetry) {
        if ((time - lastTelemetry) > getInterval(&bind_data)) {
          // telemetry lost
          if (!(bind_data.flags & MUTE_TX)) {
            buzzerOn(BZ_FREQ);
          }
          lastTelemetry = 0;
        } else {
          // telemetry link re-established
          buzzerOff();
        }
      }

      // Construct packet to be sent
      tx_buf[0] &= 0xc0; //preserve seq. bits
      if ((serial_tail != serial_head) && (serial_okToSend == 2)) {
        tx_buf[0] ^= 0x80; // signal new data on line
        uint8_t bytes = 0;
        uint8_t maxbytes = 8;
        if (getPacketSize(&bind_data) < 9) {
          maxbytes = getPacketSize(&bind_data) - 1;
        }
        while ((bytes < maxbytes) && (serial_head != serial_tail)) {
          bytes++;
          tx_buf[bytes] = serial_buffer[serial_head];
          serial_resend[bytes] = serial_buffer[serial_head];
          serial_head = (serial_head + 1) % SERIAL_BUFSIZE;
        }
        tx_buf[0] |= (0x37 + bytes);
        serial_resend[0] = bytes;
        serial_okToSend = 3; // sent but not acked
      } else if (serial_okToSend == 4) {
        uint8_t i;
        for (i = 0; i < serial_resend[0]; i++) {
          tx_buf[i + 1] = serial_resend[i + 1];
        }
        tx_buf[0] |= (0x37 + serial_resend[0]);
        serial_okToSend = 3; // sent but not acked
      } else {
        if (FSstate == 2) {
          tx_buf[0] |= 0x01; // save failsafe
          Red_LED_ON
        } else {
          tx_buf[0] |= 0x00; // servo positions
          Red_LED_OFF
          if (serial_okToSend == 0) {
            serial_okToSend = 1;
          }
          if (serial_okToSend == 3) {
            serial_okToSend = 4;  // resend
          }
        }
        cli(); // disable interrupts when copying servo positions, to avoid race on 2 byte variable
        packChannels(bind_data.flags & 7, PPM, tx_buf + 1);
        sei();
      }
      //Green LED will be on during transmission
      Green_LED_ON;

      // Send the data over RF
      rfmSetChannel(RF_channel);

      tx_packet(tx_buf, getPacketSize(&bind_data));

      //Hop to the next frequency
      RF_channel++;

      if ((RF_channel == MAXHOPS) || (bind_data.hopchannel[RF_channel] == 0)) {
        RF_channel = 0;
      }

      // do not switch channel as we may receive telemetry on the old channel
      if (bind_data.flags & TELEMETRY_MASK) {
        linkQuality <<= 1;
        RF_Mode = Receive;
        rx_reset();
        // tell loop to sample downlink RSSI
        sampleRSSI = micros();
        if (sampleRSSI == 0) {
          sampleRSSI = 1;
        }
      }
    } else {
      if (ppmAge == 8) {
        Red_LED_ON
      }
      ppmAge = 9;
      // PPM data outdated - do not send packets
    }
  }

  if (bind_data.flags & TELEMETRY_FRSKY) {
    uint8_t linkQualityTX = countSetBits(linkQuality & 0xfffe);

    uint8_t compRX = compositeRSSI(RSSI_rx, linkQualityRX);
    uint8_t compTX = compositeRSSI(RSSI_tx, linkQualityTX);

    frskyUpdate(RX_ain0, RX_ain1, compRX, compTX, activeProfile);
    //frskyUpdate(RX_ain0,RX_ain1,lastTelemetry?RSSI_rx:0,lastTelemetry?RSSI_tx:0);
  }
  //Green LED will be OFF
  Green_LED_OFF;

  checkFS();
}
