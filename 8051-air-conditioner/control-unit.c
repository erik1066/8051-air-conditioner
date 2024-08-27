// PROJECT TEAM : (in firstname ABC order)
// 
// Charlie So
// Erik Knudsen
// Warren McClure
//
// SwE 6843 Embedded Systems - Spring 2017 - Professor Lartigue - Final Project
//
// This project represents an air conditioner control unit. The "set" value is
// the temperature the user wishes the room to remain at. This value is 
// received from an XBee ZigBee S2C device via UART1. The actual room 
// temperature is also received from the XBee ZigBee S2C device, but in this
// case, the incoming values can be from a mesh of potentially many remote 
// XBees. The A/C unit computes an average of all the remote sensor readings
// and determines whether, based on the difference between the set and actual
// values, if the A/C fan needs to turn on or off.
//
// The idea behind having a "running average" from many remote XBee devices is
// that no one "spike" or sudden drop in input will itself cause a reaction
// from the A/C unit. These spikes and drops will instead be smoothed out.
//
// Additionally, the A/C unit has its own temperature sensor that exists inside
// the unit and is used to determine when the coolant (in our case, this is 
// either ice or dry ice) is empty. For example, if the *internal* temp of the
// cooler is above, say, 70F, then it's likely there is no more ice and hence
// no point in running the fan. This also triggers an alert for the user in 
// the form of an audible buzz from a buzzer and an LED is turned on for a 
// visual indication that replenishment is needed.
//
// Separate control code will be written and implemented for the thermostat
// which will also be controlled by an 8051.
//
// Code attributions:
//
// 1) Gupta, Sen Gourab and Chew, Moi Tin. "Embedded Programming with Field-Programmable Mixed-Signal uControllers", 3rd Edition. SiLabs, 2012.
//		ADC1 programming example pages 209-11
//		Many other small references
//
// 2) SiLabs example code found in F02x_UART1_Interrupt in C:\SiLabs\MCU\Examples\C8051F02x\UART folder
//		UART programming and UART interrupt examples, which we heavily modified but still relied on

//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------

#include <c8051f020.h>                 // SFR declarations
#include <stdio.h>

//-----------------------------------------------------------------------------
// 16-bit SFR Definitions for 'F02x
//-----------------------------------------------------------------------------

sfr16 RCAP2    = 0xca;                 // Timer2 capture/reload
sfr16 TMR2     = 0xcc;                 // Timer2

//-----------------------------------------------------------------------------
// Global Constants
//-----------------------------------------------------------------------------

#define BAUDRATE     9600            // Baud rate of UART in bps

// SYSTEMCLOCK = System clock frequency in Hz

#define SYSTEMCLOCK       (22118400L)

//-----------------------------------------------------------------------------
// Function Prototypes
//-----------------------------------------------------------------------------

void OSCILLATOR_Init (void);
void PORT_Init (void);
void UART1_Init (void);
void GetInternalReadings ();
void Wait_MS (unsigned int ms);
void Wait_uS (unsigned int us);
void Set_LEDs ();
void Display_Temp (float measurement, short output);
void Display_Digit (short digit, short latch);
void TransmitData (short avgTemp, char state);

//-----------------------------------------------------------------------------
// Global Variables
//-----------------------------------------------------------------------------

#define UART_BUFFERSIZE 24
unsigned char UART_Buffer[UART_BUFFERSIZE];
unsigned char UART_Buffer_Size = 0;
unsigned char UART_Input_First = 0;
unsigned char UART_Output_First = 0;
unsigned char TX_Ready = 1;
static char Byte;
unsigned int dht11_dat[5] = { 0, 0, 0, 0, 0 };
float internal_temp = 0.0;

//-----------------------------------------------------------------------------
// DHT11
//-----------------------------------------------------------------------------
sbit DHT11 = P1^4;
sbit RELAY = P1^2;

sbit LATCH0    = P2^0; // latches
sbit LATCH1    = P2^2;
sbit LATCH2    = P2^4;
sbit LATCH4    = P2^6;

sbit DIGIT1    = P2^1; // digits
sbit DIGIT2    = P2^3;
sbit DIGIT4    = P2^5;
sbit DIGIT8    = P2^7;

//-----------------------------------------------------------------------------
// main() Routine
//-----------------------------------------------------------------------------

void main (void)
{
	unsigned short AVG_Temps[7] = {0,0,0,0,0,0,0};
	unsigned short AVG_Temp = 0;
	unsigned short SET_Temp = 0;

	unsigned short PREV_SET_Temp = 0;
	unsigned short PREV_AVG_Temp = 0;

	bit first = 1;
	bit isOn = 0;

	int i = 0;
	int j = 0;

	unsigned short state = 0x00;

   WDTCN = 0xDE;                       // Disable watchdog timer
   WDTCN = 0xAD;

   OSCILLATOR_Init ();                 // Initialize oscillator
   PORT_Init ();                       // Initialize crossbar and GPIO

   UART1_Init ();                      // Initialize UART1

   EA = 1;

   P5 = 0;

   RELAY = 1; // 1 for the relay means OFF

   i = 0;

   while (1)
   {
   		// Determine the internal temp of the coolant resevior
	 	if (j == 0)
		{
			GetInternalReadings();
			//internal_temp = 32;
		}

		Set_LEDs();
		
		// Check for a ZigBee Rx Packet API frame that contains a combo set/actual temp pair, 
		// and if it is, read the first two bytes
		if ( UART_Buffer_Size == 18 && UART_Buffer[0] == 0x7E && UART_Buffer[3] == 0x90)
		{
			// Get the previous value
			PREV_SET_Temp = SET_Temp;

			// Get the previous avg
			PREV_AVG_Temp = AVG_Temp;

			// Assign the set value to a variable
			SET_Temp = UART_Buffer[15];

			// Display the new set value if different
			if (PREV_SET_Temp != SET_Temp)
			{
				Display_Temp(SET_Temp, 0);
			}
			
			// Get a running avg	
			if (first == 1) 
			{
				AVG_Temps[0] = AVG_Temps[1] = AVG_Temps[2] = AVG_Temps[3] = AVG_Temps[4] = AVG_Temps[5] = AVG_Temps[6] = UART_Buffer[16];
				first = 0;
			}
			else 
			{
				AVG_Temps[i] = UART_Buffer[16];
				i++;
			}

			AVG_Temp = (AVG_Temps[0] + AVG_Temps[1] + AVG_Temps[2] + AVG_Temps[3] + AVG_Temps[4] + AVG_Temps[5] + AVG_Temps[6]) / 7;

			// Display the new avg temp from last 7 readings
			if (PREV_AVG_Temp != AVG_Temp)
			{
				Display_Temp(AVG_Temp, 1);
			}

			if (TX_Ready == 1)
			{	
				TransmitData(AVG_Temp, state);
			}

			UART_Buffer_Size = 0; // reset buffer
		}
		// Otherwise, check for a ZigBee Rx Packet API frame that contains just an actual temp reading
		// without a set value. This will have come from a remote sensor and not the thermostat
		else if ( UART_Buffer_Size == 17 && UART_Buffer[0] == 0x7E && UART_Buffer[3] == 0x90)
		{
			// Get a running avg	
			if (first == 1) 
			{
				AVG_Temps[0] = AVG_Temps[1] = AVG_Temps[2] = AVG_Temps[3] = AVG_Temps[4] = AVG_Temps[5] = AVG_Temps[6] = UART_Buffer[15];
				first = 0;
			}
			else 
			{
				AVG_Temps[i] = UART_Buffer[15];
				i++;
			}

			AVG_Temp = (AVG_Temps[0] + AVG_Temps[1] + AVG_Temps[2] + AVG_Temps[3] + AVG_Temps[4] + AVG_Temps[5] + AVG_Temps[6]) / 7;

			// Display the new avg temp from last 7 readings
			Display_Temp(AVG_Temp, 1);

			if(TX_Ready == 1)
			{	
				TransmitData(AVG_Temp, state);
			}

			UART_Buffer_Size = 0; // reset buffer
		}
		else 
		{
			UART_Buffer_Size = 0;
		}

		if (i >= 7) i = 0;

		// Do we turn the unit on or off?
		if (j % 5 == 0)
		{		
			if (isOn == 1)
			{
				if ( SET_Temp > AVG_Temp || internal_temp >= 70.0 ) 
				{
					// turn unit off
					RELAY = 1;
					isOn = 0;
				}
			}
			else if (isOn == 0)
			{
				if ( SET_Temp < AVG_Temp && internal_temp < 70.0)
				{
					// turn unit on
					RELAY = 0;
					isOn = 1;
				}
			}

			if (isOn == 1) state |= 0x01; else state &= ~0x01;
			if (internal_temp >= 70.0) state |= 0x02; else state &= ~0x02;
		}

		j++;

		if (j >= 10) j = 0;

		Wait_MS(1000);
   }
}

//-----------------------------------------------------------------------------
// Initialization Subroutines
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// OSCILLATOR_Init
//-----------------------------------------------------------------------------
//
// Return Value : None
// Parameters   : None
//
// This function initializes the system clock to use the  external 22.1184MHz
// crystal.
//
//-----------------------------------------------------------------------------
void OSCILLATOR_Init (void)
{
   int i;                              // Software timer

   OSCICN |= 0x80;                     // Enable the missing clock detector

   // Initialize external crystal oscillator to use 22.1184 MHz crystal

   OSCXCN = 0x67;                      // Enable external crystal osc.
   for (i=0; i < 256; i++);            // Wait at least 1ms
   while (!(OSCXCN & 0x80));           // Wait for crystal osc to settle
   OSCICN |= 0x08;                     // Select external clock source
   OSCICN &= ~0x04;                    // Disable the internal osc.
}

//-----------------------------------------------------------------------------
// PORT_Init
//-----------------------------------------------------------------------------
//
// Return Value : None
// Parameters   : None
//
// This function configures the crossbar and GPIO ports.
//
// P0.2   digital   push-pull     UART TX
// P0.3   digital   open-drain    UART RX
//
// P2.x	  digital   push-pull     7-seg displays and CD4543b latches
//
// P1.2   digital   push-pull	  5v Relay
//
//-----------------------------------------------------------------------------
void PORT_Init (void)
{
	// The TOOLSTICK UNI DC has ports 0.0 and 0.1 dedicated to USB, so we 
	// cannot use those for communicating with an XBee module via pins. 
	// Those pins are even labeled as "NC" on the card. This is a problem
	// because the standard UART configuration for the crossbar assigns 
	// P0.0 and P0.1 to the TX/RX pins.
	//
	// The workaround is to initialize both UART0 and UART1 on the crossbar.
	// This "assigns" P0.0 and P0.1 to UART0 and then the subsequent pins
	// P0.2 and P0.3 to UART1. Since P0.2 and P0.3 are connected, we can 
	// then use UART1 and avoid the problem. Just know that UART0 is
	// initialized is to work around the pin issue on the TOOLSTICK UNI DC
	// card and this implementation would likely not be necessary on a
	// typical 8051.

   XBR0     = 0x04;		// Enable UART0			

   XBR1     = 0x00;
   XBR2     = 0x44;     // Enable crossbar and weak pull-up, enable UART1

   P0MDOUT |= 0x04;     // Set UART TX pins to push-pull on port 0
	
   P74OUT = 0x08;	// Sets port 5 pins 4-7 as push-pull
   P5 |= 0x0F;		

   P2MDOUT = 0xFF;     // Set all pins on port 2 to push-pull
   						// Even port 2 pins are latch enable/disable
						// Odd port 2 pins are for 7-seg digits

   P1MDOUT |= 0x04;		// Set port 1 pin 2 to output push-pull digital
}

//-----------------------------------------------------------------------------
// UART1_Init
//-----------------------------------------------------------------------------
//
// Return Value : None
// Parameters   : None
//
// Configure the UART1 using Timer1, for <baudrate> and 8-N-1.
// This routine configures the UART1 based on the following equation:
//
// Baud = (2^SMOD1/32)*(SYSTEMCLOCK*12^(T1M-1))/(256-TH1)
//
// This equation can be found in the datasheet, Mode1 baud rate using timer1.
// The function select the proper values of the SMOD1 and T1M bits to allow
// for the proper baud rate to be reached.
//-----------------------------------------------------------------------------
void UART1_Init (void)
{
   SCON1   = 0x50;                     // SCON1: mode 1, 8-bit UART, enable RX

   TMOD   &= ~0xF0;
   TMOD   |=  0x20;                    // TMOD: timer 1, mode 2, 8-bit reload


   PCON |= 0x10;                    // SMOD1 (PCON.4) = 1 --> UART1 baudrate
                                       // divide-by-two disabled
   CKCON |= 0x10;                   // Timer1 uses the SYSTEMCLOCK
   TH1 = - ((SYSTEMCLOCK/BAUDRATE)/16);

   TL1 = TH1;                          // init Timer1
   TR1 = 1;                            // START Timer1
   TX_Ready = 1;                       // Flag showing that UART can transmit
   EIE2     = 0x40;                    // Enable UART1 interrupts

   EIP2    = 0x40;                     // Make UART high priority
}

//-----------------------------------------------------------------------------
// Interrupt Service Routines
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// UART1_Interrupt
//-----------------------------------------------------------------------------
//
// This routine is invoked whenever a byte is received from UART or transmitted.
//
//-----------------------------------------------------------------------------

void UART1_Interrupt (void) interrupt 20
{
   if ((SCON1 & 0x01) == 0x01)
   {
      // Check if a new word is being entered
      if( UART_Buffer_Size == 0)  
	  {
         UART_Input_First = 0; 
	  } 

      SCON1 = (SCON1 & 0xFE);          //RI1 = 0;
      Byte = SBUF1;                    // Read a character from Hyperterminal
		
      if (UART_Buffer_Size < UART_BUFFERSIZE)
      {
         UART_Buffer[UART_Input_First] = Byte;  // Store character

         UART_Buffer_Size++;            // Update array's size

         UART_Input_First++;            // Update counter
      }
   }

   if ((SCON1 & 0x02) == 0x02)         // Check if transmit flag is set
   {
      SCON1 = (SCON1 & 0xFD);
      if (UART_Buffer_Size != 1)        // If buffer not empty
      {

         // Check if a new word is being output
         if ( UART_Buffer_Size == UART_Input_First )  
         {
            UART_Output_First = 0;
         }

         Byte = UART_Buffer[UART_Output_First];

         SBUF1 = Byte;

         UART_Output_First++;           // Update counter
         UART_Buffer_Size--;            // Decrease array size
      }
      else
      {
         UART_Buffer_Size = 0;           // Set the array size to 0
         TX_Ready = 1;                   // Indicate transmission complete
      }
   }
}

//-----------------------------------------------------------------------------
// Support Subroutines
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// TransmitData
//-----------------------------------------------------------------------------
//
// Transmits a ZigBee Transmit Request frame with the avg temp in byte 0 and
// the system state in byte 1 (bit 1 is on/off and bit 2 is coolant remaining
// or empty.
//
//-----------------------------------------------------------------------------

void TransmitData(short avgTemp, char state)
{
	short i = 0;
	int sum = 0;

	UART_Buffer[0] = 0x7E; // start byte
	UART_Buffer[1] = 0x00; // Length MSB
	UART_Buffer[2] = 0x10; // Length LSB
	UART_Buffer[3] = 0x10; // frame type (0x10 = transmit request)
	UART_Buffer[4] = 0x01; // frame ID

	UART_Buffer[5] = 0xFF;	// start 64-bit addr
	UART_Buffer[6] = 0xFF;
	UART_Buffer[7] = 0xFF;
	UART_Buffer[8] = 0xFF;
	UART_Buffer[9] = 0xFF;
	UART_Buffer[10] = 0xFF;
	UART_Buffer[11] = 0xFF;
	UART_Buffer[12] = 0xFF;

	UART_Buffer[13] = 0x89;	// start 16-bit addr, we know 0x8949 is addr of the thermostat
	UART_Buffer[14] = 0x49;

	UART_Buffer[15] = 0x00;
	UART_Buffer[16] = 0x01; //disable ack

	UART_Buffer[17] = avgTemp; // temp
	UART_Buffer[18] = state; // state

	// compute the checksum per the ZigBee API spec
	// Algorithm: Add all bytes except the first three, then remove
	// all but the first 8 bits and subtract that value from 0xFF
	for ( i = 3; i <= 18; i++ )
	{
		sum = sum + (int)UART_Buffer[i];
	}

	sum = sum & ~0xFF00;

	sum = 0xFF - sum;

	UART_Buffer[19] = sum;// checksum
	UART_Buffer_Size = 19;
	// example API frame: 7E 00 10 10 01 FF FF FF FF FF FF FF FF FF FE 00 00 58 03 9E

	TX_Ready = 0;
	SCON1 = (SCON1 | 0x02);
}

//-----------------------------------------------------------------------------
// GetInternalReadings
//-----------------------------------------------------------------------------
//
// Determines the temp inside the cooler. This is used to determine how much
// coolant is remaining. The value is not intended for direct display to the
// user in decimal format. The value is read off a DHT11 using a form of
// software serial.
//
//-----------------------------------------------------------------------------

void GetInternalReadings ()
{
	bit laststate	= 1;
	short xstate = 1;
    short counter		= 0;
	short j	= 0, i;
    float f;

	dht11_dat[0] = dht11_dat[1] = dht11_dat[2] = dht11_dat[3] = dht11_dat[4] = 0;

	// Note: DHT11 pin is P1.4

	// set DHT11 pin to output
	// set DHT11 pin to push-pull
	P1MDOUT |= 0x10; // set the 16's bit

	// write DHT11 pin low
	DHT11 = 0;

	// delay 18 ms
	Wait_MS(18);

	// write DHT11 pin high
	DHT11 = 1;

	// delay 40 us
	Wait_uS(40);

	// set DHT pin to input
	P1MDOUT &= ~0x10;  // unset the 16's bit
	DHT11 = 1;
	xstate = DHT11;

	//reads %RH and Temp. (F & C)
    for (i=0; i< 85; i++) 
	{
        counter = 0;
        while (DHT11 == laststate) 
		{
            counter++;
            Wait_uS(1);
            if (counter == 255) 
			{
                break;
            }
        }
        laststate = DHT11;

		if (laststate == 0) 
		{
			xstate = xstate + 1;
		}

        if (counter == 255) break;

        // ignore first 3 transitions
        if ((i >= 4) && (i%2 == 0)) {
            // shove each bit into the storage bytes
            dht11_dat[j/8] <<= 1;
            if (counter > 16)
                dht11_dat[j/8] |= 1;
            j++;
        }
    }

    if ((j >= 40) &&
                (dht11_dat[4] == ((dht11_dat[0] + dht11_dat[1] + dht11_dat[2] + dht11_dat[3]) & 0xFF)) ) 
	{
        f = dht11_dat[2] * 9.0 / 5.0 + 32.0;
		if (f > 0.0f)
        	internal_temp = f;
    }

}

//-----------------------------------------------------------------------------
// Wait_MS
//-----------------------------------------------------------------------------
//
// Return Value : None
// Parameters:
//   1) unsigned int ms - number of milliseconds of delay
//                        range is full range of integer: 0 to 65335
//
// This routine inserts a delay of <ms> milliseconds.
//
//-----------------------------------------------------------------------------
void Wait_MS(unsigned int ms)
{

   CKCON &= ~0x20;                     // use SYSCLK/12 as timebase

   RCAP2 = -(SYSTEMCLOCK/1000/12);          // Timer 2 overflows at 1 kHz
   TMR2 = RCAP2;

   ET2 = 0;                            // Disable Timer 2 interrupts

   TR2 = 1;                            // Start Timer 2

   while(ms)
   {
      TF2 = 0;                         // Clear flag to initialize
      while(!TF2);                     // Wait until timer overflows
      ms--;                            // Decrement ms
   }

   TR2 = 0;                            // Stop Timer 2
}

//-----------------------------------------------------------------------------
// Wait_uS
//-----------------------------------------------------------------------------
//
// Return Value : None
// Parameters:
//   1) unsigned int us - number of microseconds of delay
//                        range is full range of integer: 0 to 65335
//
// This routine inserts a delay of <us> microseconds.
//
//-----------------------------------------------------------------------------
void Wait_uS(unsigned int us)
{

   CKCON &= ~0x20;                     // use SYSCLK/12 as timebase

   RCAP2 = -(SYSTEMCLOCK/ 1000000 /12);          // Timer 2 overflows at 1 kHz
   TMR2 = RCAP2;

   ET2 = 0;                            // Disable Timer 2 interrupts

   TR2 = 1;                            // Start Timer 2

   while(us)
   {
      TF2 = 0;                         // Clear flag to initialize
      while(!TF2);                     // Wait until timer overflows
      us--;                            // Decrement ms
   }

   TR2 = 0;                            // Stop Timer 2
}

//-----------------------------------------------------------------------------
// Sets LEDs
//-----------------------------------------------------------------------------
//
// Turns on zero, one, two, three, or four of the built-in LEDs (P5.4-P5.7) on 
// the TOOLSTICK UNI DC card depending on remaining coolant.
//
//-----------------------------------------------------------------------------

void Set_LEDs()
{
	if (internal_temp == 0.0) return;

	if (internal_temp >= 70.0) 
	{
		P5 = 0; // gone
	}
	else if (internal_temp < 70.0 && internal_temp >= 60.0)
	{
		P5 |= 0x10; // almost out
	}
	else if (internal_temp < 60.0 && internal_temp >= 50.0)
	{
		P5 |= 0x30; // 50 to 60 it's getting low
	}
	else if (internal_temp < 50.0 && internal_temp >= 40)
	{
		P5 |= 0x70; // 40 to 50, we assume we're 3/4 full
	}
	else 
	{
		P5 |= 0xF0; // if its below 40F then we assume FULL collant
	}
}

//-----------------------------------------------------------------------------
// Sets 7-segment displays
//-----------------------------------------------------------------------------
//
// Given a measurement, display that measurement on the given output line.
// Measurement is assumed to be between 0 and 99 and one value of output will
// corresponding to two pins. Each CD4543B has a dedicated 'latch' pin so that
// all 4 7-segment displays only need 8 pins total, 4 for the digit value of
// 0-9 and 4 more for the latch open/close.
//
//-----------------------------------------------------------------------------

void Display_Temp(float measurement, short output)
{
	short firstDigit = 0;
	short tens = 0;
	short secondDigit = 0;

	if (measurement < 0.0) measurement = 0.0f;
	if (measurement >= 100.0) measurement = 99.0f;

	firstDigit = (short)(measurement / 10.0f);
	
	if (firstDigit > 0.0f) 
	{
		Display_Digit(firstDigit, 0 + (output * 2));
	}
	else 
	{
		Display_Digit(0, 0 + (output * 2));
	}

	tens = (short)firstDigit * 10;

	secondDigit = (short)measurement - tens;

	Display_Digit(secondDigit, 1 + (output * 2));
}

//-----------------------------------------------------------------------------
// Displays a digit on the given latch line
//-----------------------------------------------------------------------------
//
// Displays a single digit on the given latch line, 0-3. The latch that needs 
// to be open (so it can be changed) is sent a high signal. The rest are set
// to low so that the digit does not get written to them, and they hold onto
// the last value that was sent.
//
//-----------------------------------------------------------------------------

void Display_Digit(short digit, short latch) 
{
	// latch 0 == leftmost 7-seg display
	// latch 1 == second to leftmost 7-seg display
	// latch 2 == second to rightmost 7-seg display
	// latch 3 == rightmost 7-seg display

	if (latch == 0) 
	{
		LATCH0 = 1;
		LATCH1 = 0;
		LATCH2 = 0;
		LATCH4 = 0;		
	}	
	else if (latch == 1) 
	{
		LATCH0 = 0;
		LATCH1 = 1;
		LATCH2 = 0;
		LATCH4 = 0;
	}	
	else if (latch == 2) 
	{
		LATCH0 = 0;
		LATCH1 = 0;
		LATCH2 = 1;
		LATCH4 = 0;
	}	
	else if (latch == 3) 
	{
		LATCH0 = 0;
		LATCH1 = 0;
		LATCH2 = 0;
		LATCH4 = 1;
	}

	if (digit == 0) 
	{
			DIGIT1 = 0;
			DIGIT2 = 0;
			DIGIT4 = 0;
			DIGIT8 = 0;
	}
	else if (digit ==  1)
	{
			DIGIT1 = 1;
			DIGIT2 = 0;
			DIGIT4 = 0;
			DIGIT8 = 0;
	}
	else if (digit ==  2)
	{
			DIGIT1 = 0;
			DIGIT2 = 1;
			DIGIT4 = 0;
			DIGIT8 = 0;
	}
	else if (digit ==  3)
	{
			DIGIT1 = 1;
			DIGIT2 = 1;
			DIGIT4 = 0;
			DIGIT8 = 0;
	}
	else if (digit ==  4)
	{
			DIGIT1 = 0;
			DIGIT2 = 0;
			DIGIT4 = 1;
			DIGIT8 = 0;
	}
	else if (digit ==  5)
	{
			DIGIT1 = 1;
			DIGIT2 = 0;
			DIGIT4 = 1;
			DIGIT8 = 0;
	}
	else if (digit ==  6)
	{
			DIGIT1 = 0;
			DIGIT2 = 1;
			DIGIT4 = 1;
			DIGIT8 = 0;
	}
	else if (digit ==  7)
	{
			DIGIT1 = 1;
			DIGIT2 = 1;
			DIGIT4 = 1;
			DIGIT8 = 0;
	}
	else if (digit ==  8)
	{
			DIGIT1 = 0;
			DIGIT2 = 0;
			DIGIT4 = 0;
			DIGIT8 = 1;
	}
	else if (digit ==  9)
	{
			DIGIT1 = 1;
			DIGIT2 = 0;
			DIGIT4 = 0;
			DIGIT8 = 1;
	}
}

//-----------------------------------------------------------------------------
// End Of File
//-----------------------------------------------------------------------------