// PROJECT TEAM : (in firstname ABC order)
// 
// Charlie So
// Erik Knudsen
// Warren McClure
//
// SwE 6843 Embedded Systems - Spring 2017 - Professor Lartigue - Final Project
//
// This project represents an a thermostat. It is designed to work with a
// second 8051, where the second 8051 is controlling an air conditioner. The
// thermostat samples temperature from an analog TMP36 and sends those bytes
// over a ZigBee mesh network to the A/C controller 8051. The A/C controller
// averges those temperatures and then transmits both its state and the 
// average temperature back to the thermostat. The thermostat displays the
// air conditioner's status (on or off), whether the A/C is out of coolant,
// and the average temperature calculated by the air conditioner.
//
// The thermostat has a potentiometer (dial) that allows the user to set
// the desired room temperature. Both the TMP36 and potentiometer are
// wired to use ADC1 on the 8051. This requires using the ADC1 multiplex
// selector to choose the appropriate AN1 input pin. We use a timer to
// interrupt at a set interval to check the ADC1 value, and toggle between
// the two ADC1 inputs using the ADC1 multiplex selector register.
//
// An LCD display unit is the primary output device for the user. It is a
// 16x2 display unit. It shows the average temperature as reported by the
// air conditioner, the system state, and the user's desired room temperature.
// The user's desired room temperature is updated immediately upon their 
// adjusting the potentiometer. The average value is sent over the ZigBee
// network every few seconds and so changes less frequently.
//
// An XBee S2C radio is connected to the thermostat via UART. See the code
// comments in the 8051-air-conditioner project and the associated .PDF
// on 8051-to-XBee interfacing for more details on that subject.
//
// The team wished to include a buzzer that "buzzed" when a "no coolant"
// message was received over ZigBee. Technically this was a simple addon
// but numerous voltage output problems with both 8051 boards prevented it
// and a time constraint on the due date made us hesitant to rush it in 
// at the last minute. Our code to output signals to the buzzer are commented.
//
// With more time and effort, we believe it would have been possible to use 
// remote ZigBee sensor nodes to sample CO, gas, smoke, and other environmental
// factors, transmit these to the thermostat, and then use a button on the
// thermostat to toggle the LCD between different 'views' (temp, humidity,
// CO, smoke, etc). Time ran out, however.
//
// Code attributions:
//
// 1) Gupta, Sen Gourab and Chew, Moi Tin. "Embedded Programming with Field-Programmable Mixed-Signal Microcontrollers", 3rd Edition. SiLabs, 2012.
//		ADC1 programming example pages 209-11
//		Many other small references
//
// 2) SiLabs example code found in F02x_UART1_Interrupt in C:\SiLabs\MCU\Examples\C8051F02x\UART folder
//		UART programming and UART interrupt examples, which we heavily modified but still relied on
//
// 3) LCD Functions Developed by electroSome

//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------

#include <c8051f020.h>                 // SFR declarations
#include <compiler_defs.h>
#include <stdio.h>
#include "lcd.h"					   // Adding this library for LCD control

//-----------------------------------------------------------------------------
// 16-bit SFR Definitions for 'F02x
//-----------------------------------------------------------------------------

sfr16 ADC0     = 0xbe;                 // ADC0 data
sfr16 RCAP2    = 0xca;                 // Timer2 capture/reload
sfr16 RCAP3    = 0x92;                 // Timer3 capture/reload
sfr16 TMR2     = 0xcc;                 // Timer2
sfr16 TMR3     = 0x94;                 // Timer3

//LCD Module Connections
sbit RS = P1^0;                                                                   
sbit EN = P1^2;                            
sbit D0 = P2^0;
sbit D1 = P2^1;
sbit D2 = P2^2;
sbit D3 = P2^3;
sbit D4 = P2^4;
sbit D5 = P2^5;
sbit D6 = P2^6;
sbit D7 = P2^7;

sbit AM2302 = P1^7;

//-----------------------------------------------------------------------------
// Global Constants
//-----------------------------------------------------------------------------

#define BAUDRATE     9600	           // Baud rate of UART in bps
#define SYSCLK       22118400          // External crystal oscillator frequency
#define SAMPLE_RATE  50000             // Sample frequency in Hz
#define INT_DEC      256               // Integrate and decimate ratio

#define SAMPLE_DELAY 150                // Delay in ms before taking sample

//-----------------------------------------------------------------------------
// Function Prototypes
//-----------------------------------------------------------------------------

void OSCILLATOR_Init (void);           
void PORT_Init (void);
void UART1_Init (void);
void ADC1_Init (void);
void TIMER3_Init (int counts);
void Timer3_ISR ();
void Wait (unsigned int ms, short us);
void TransmitData (void);
//void GetExternalReadings (void);
void GetDigits (float measurement, int * digit1, int * digit2);

//-----------------------------------------------------------------------------
// Global Variables
//-----------------------------------------------------------------------------

#define UART_RX_BUFFERSIZE 20
unsigned char UART_Rx_Buffer[UART_RX_BUFFERSIZE];
unsigned char UART_Rx_Buffer_Size = 0;
unsigned char UART_Rx_Input_First = 0;
unsigned char UART_Rx_Output_First = 0;

#define UART_TX_BUFFERSIZE 20
unsigned char UART_Tx_Buffer[UART_TX_BUFFERSIZE];
unsigned char UART_Tx_Buffer_Size = 0;
unsigned char UART_Tx_Output_First = 0;

unsigned char TX_Ready = 1;
static char Byte;
unsigned char Dial_Reading;
unsigned char Temp_Reading;

float internal_temp = 0.0;

//-----------------------------------------------------------------------------
// main() Routine
//-----------------------------------------------------------------------------

void main (void)
{
	int j = 0;

	int digit1 = 0;
	int digit2 = 0;

	unsigned short averageTemp = 0;
	unsigned short controlUnitState = 0x00;

	WDTCN = 0xDE;                       // Disable watchdog timer
	WDTCN = 0xAD;

	OSCILLATOR_Init ();                 // Initialize oscillator
	PORT_Init ();                       // Initialize crossbar and GPIO
	UART1_Init ();                      // Initialize UART1 for ZigBee
	Lcd8_Init();						// Initialize LCD in 8bit mode

	// Timer 3 is used for ADC1
	TIMER3_Init (SYSCLK/12/10);   // Initialize Timer3 to overflow at
	                              // sample rate

	ADC1_Init ();                       // Init ADC

	EA = 1;                             // Enable global interrupts

	P5 = P5 & 0xFF;

	// Flash the LEDs on bootup for visual conf that the thing is running
	for ( j = 0; j < 10; j++) 
	{
		Wait(50, 0);
		P5 |= 0xF0;
		Wait(50, 0);
	  	P5 = 0x00;
	}

	j = 0;

	while (1)
	{
		//EA = 0;

		// Write the initial text into the LCD display. maybe need to put in second c file.
		Lcd8_Set_Cursor(1,1);
		Lcd8_Write_String("Temp: ");
		Lcd8_Set_Cursor(1,7);

		GetDigits((float)averageTemp, &digit1, &digit2);

		Lcd8_Write_Char(digit1 + 48);
		Lcd8_Set_Cursor(1,8);
		Lcd8_Write_Char(digit2 + 48);

		if (controlUnitState & 0x01)
		{
			Lcd8_Set_Cursor(1,13);
			Lcd8_Write_String("ON ");			
		}
		else
		{
			Lcd8_Set_Cursor(1,13);
			Lcd8_Write_String("OFF");			
		}

		Lcd8_Set_Cursor(2,1);
		Lcd8_Write_String("Set: ");

		GetDigits((float)Dial_Reading, &digit1, &digit2);

		Lcd8_Set_Cursor(2,7);
		Lcd8_Write_Char(digit1 + 48);
		Lcd8_Set_Cursor(2,8);
		Lcd8_Write_Char(digit2 + 48);
		

		if (controlUnitState & 0x02)
		{
			Lcd8_Set_Cursor(2,13);
			Lcd8_Write_String("NC");			
		}
		else
		{
			Lcd8_Set_Cursor(2,13);
			Lcd8_Write_String("  ");			
		}

		// Check for a ZigBee Rx Packet API frame that contains a combo set/actual temp pair, 
		// and if it is, read the first two bytes
		if (UART_Rx_Buffer_Size == 18 && UART_Rx_Buffer[0] == 0x7E && UART_Rx_Buffer[3] == 0x90)
		{
			// Assign the control unit's computed temp average to a variable
			averageTemp = UART_Rx_Buffer[15];
			controlUnitState = UART_Rx_Buffer[16];
			UART_Rx_Buffer_Size = 0; // reset buffer
		}
		else 
		{
			UART_Rx_Buffer_Size = 0;
		}

		// Check the control unit state for whether the A/C unit is
		// on and cooling the room or off
		if (controlUnitState & 0x01) 
		{
			P5 |= 0x10;
		}
		else 
		{
			P5 &= ~0x10;
		}

		// Check the control unit state for coolant remaining or empty
		if (controlUnitState & 0x02)
		{
			P5 &= ~0x20;
			
			// this condition will get hit over and over so let's not
			// keep sounding the buzzer each time. Do it once.
			//if (shouldBuzzOnEmpty) 
			//{			
			//	shouldBuzzOnEmpty = 0;
			//}
		}
		else 
		{
			P5 |= 0x20;

			// coolant remains so make sure next time it goes 'dry' we
			// activate the buzzer
			//shouldBuzzOnEmpty = 1;
		}

		if(TX_Ready == 1 && j == 0)
		{	
			//GetExternalReadings();
			TransmitData();
			UART_Tx_Buffer_Size = 0;
		}

		j++;

		if (j == 12) j = 0;

		Wait(150, 0);           // Wait some time before taking
                                       // another sample
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
// This routine initializes the system clock to use an 22.1184MHz crystal
// as its clock source.
//
//
//-----------------------------------------------------------------------------
void OSCILLATOR_Init (void)
{
   int i;                              // delay counter

   OSCXCN = 0x67;                      // start external oscillator with
                                       // 22.1184MHz crystal

   for (i=0; i < 256; i++) ;           // wait for oscillator to start

   while (!(OSCXCN & 0x80)) ;          // Wait for crystal osc. to settle

   OSCICN = 0x88;                      // select external oscillator as SYSCLK
                                       // source and enable missing clock
                                       // detector

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
//-----------------------------------------------------------------------------
void PORT_Init (void)
{
   XBR0    = 0x04;                     // Route UART0 to crossbar
	XBR1 = 0x00;
   XBR2    = 0x44;                    // Enable crossbar,  weak pull-ups,
   									   // and enable UART1

   P0MDOUT |= 0x04;     		// Set UART TX pins to push-pull on port 0

	P1MDOUT |= 0x07;
	P1MDOUT |= 0x80; // AM2302 P1^7

   	P2MDOUT = 0xFF;
	P3MDOUT = 0x00;

	P74OUT = 0x08;

	P5 |= 0x0F;
	P4 = 0xFF;

	// Analog inputs - take care to change otherwise will destroy TM36 
	P1MDIN &= ~0x40;
	//P1MDIN &= ~0x80; // destroyed too many TM36's...
	P1MDIN &= ~0x02;
	P1 |= 0x40;
	P1 |= 0x02;
	//P1 |= 0x80;


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
   TH1 = - ((SYSCLK/BAUDRATE)/16);

   TL1 = TH1;                          // init Timer1
   TR1 = 1;                            // START Timer1
   TX_Ready = 1;                       // Flag showing that UART can transmit
   EIE2     = 0x40;                    // Enable UART1 interrupts

   EIP2    = 0x40;                     // Make UART high priority

}

//-----------------------------------------------------------------------------
// TIMER3_Init
//-----------------------------------------------------------------------------
//
// Return Value : None
// Parameters   :
//   1)  int counts - calculated Timer overflow rate
//                    range is postive range of integer: 0 to 32767
//
// Configure Timer3 to auto-reload at interval specified by <counts> (no
// interrupt generated) using SYSCLK as its time base.
//
//-----------------------------------------------------------------------------
void TIMER3_Init (unsigned int counts)
{

   TMR3CN = 0x00;                      // Stop Timer3; Clear TF3; set sysclk
                                       // as timebase
	TMR3RLL = -counts;
	TMR3 = 0xFFFF;
	EIE2 |= 0x01;
	TMR3CN |= 0x04;
}

//-----------------------------------------------------------------------------
// Interrupt Service Routines
//-----------------------------------------------------------------------------

void Timer3_ISR(void) interrupt 14
{
	float temp = 0.0f;
	float dial = 0.0f;

	TMR3CN &= ~(0x80);

	//while((ADC1CN & 0x20) == 0);	

	if (AMX1SL == 0x06)
	{
		//Temp_Reading = ADC1;
		temp = ((float)ADC1 - 91.0f) * 1.8f;
		Temp_Reading = (short)temp + 32;

		// use ADC1 multiplex selector to switch back to other
		// ADC input so next timer interrupt polls the other device
		AMX1SL = 0x01; 

	}
	else
	{
		dial = (((float)ADC1) * 0.15686275) + 50;
		//dial = ADC1;//(((float)ADC1 - 14.0f) * 0.1659751037) + 50;

		if (dial < 50.0f) dial = 50.0f;
		else if (dial > 89.5f) dial = 90.0f;

		Dial_Reading = (short)dial;

		// use ADC1 multiplexer to switch to other ADC input so next
		// interrupt polls the other analog input device
		AMX1SL = 0x06;
	}

	ADC1CN &= 0xDF;
}

//-----------------------------------------------------------------------------
// ADC1_Init
//-----------------------------------------------------------------------------
//
// Return Value : None
// Parameters   : None
//
//-----------------------------------------------------------------------------
void ADC1_Init (void)
{
	REF0CN = 0x03;

	ADC1CF = 0x81;//(SYSCLK/SAR_CLK) << 3;     // ADC conversion clock = 2.5MHz
   	//ADC1CF |= 0x00;

	AMX1SL = 0x06;
	ADC1CN = 0x82; // enable ADC interrupts
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
      if( UART_Rx_Buffer_Size == 0)  
	  {
         UART_Rx_Input_First = 0; 
	  } 

      SCON1 = (SCON1 & 0xFE); 
      Byte = SBUF1;            
		
      if (UART_Rx_Buffer_Size < UART_RX_BUFFERSIZE)
      {
         UART_Rx_Buffer[UART_Rx_Input_First] = Byte;  

         UART_Rx_Buffer_Size++;            

         UART_Rx_Input_First++;            
      }
   }

   if ((SCON1 & 0x02) == 0x02)         
   {
      SCON1 = (SCON1 & 0xFD);
      if (UART_Tx_Buffer_Size != 1)        
      {
         if ( UART_Tx_Buffer_Size == UART_Tx_Output_First )  
         {
            UART_Tx_Output_First = 0;
         }

         Byte = UART_Tx_Buffer[UART_Tx_Output_First];

         SBUF1 = Byte;

         UART_Tx_Output_First++;           // Update counter
         UART_Tx_Buffer_Size--;            // Decrease array size
      }
      else
      {
         UART_Tx_Buffer_Size = 0;           // Set the array size to 0
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
// Transmits a ZigBee Transmit Request frame with the "set" temp in payload 
// byte 0 the actual room temp from the theromstat's temp sensor in byte 1
//
//-----------------------------------------------------------------------------

void TransmitData()
{
	short i = 0;
	int sum = 0;

	UART_Tx_Buffer[0] = 0x7E; // start byte
	UART_Tx_Buffer[1] = 0x00; // Length MSB
	UART_Tx_Buffer[2] = 0x10; // Length LSB
	UART_Tx_Buffer[3] = 0x10; // frame type (0x10 = transmit request)
	UART_Tx_Buffer[4] = 0x00; // frame ID, 0 means no ACK

	UART_Tx_Buffer[5] = 0xFF;	// start 64-bit addr
	UART_Tx_Buffer[6] = 0xFF;
	UART_Tx_Buffer[7] = 0xFF;
	UART_Tx_Buffer[8] = 0xFF;
	UART_Tx_Buffer[9] = 0xFF;
	UART_Tx_Buffer[10] = 0xFF;
	UART_Tx_Buffer[11] = 0xFF;
	UART_Tx_Buffer[12] = 0xFF;

	UART_Tx_Buffer[13] = 0xFF;	// 16-bit net addr
	UART_Tx_Buffer[14] = 0xFE;

	UART_Tx_Buffer[15] = 0x00;
	UART_Tx_Buffer[16] = 0x01; // set options flag

	UART_Tx_Buffer[17] = Dial_Reading;
	UART_Tx_Buffer[18] = Temp_Reading;

	// compute the checksum per the ZigBee API spec
	// Algorithm: Add all bytes except the first three, then remove
	// all but the first 8 bits and subtract that value from 0xFF
	for ( i = 3; i <= 18; i++ )
	{
		sum = sum + (int)UART_Tx_Buffer[i];
	}

	sum = sum & ~0xFF00;

	sum = 0xFF - sum;

	UART_Tx_Buffer[19] = sum;// checksum
	UART_Tx_Buffer_Size = 19;

	TX_Ready = 0;
	SCON1 = (SCON1 | 0x02);
}

void GetDigits(float measurement, int * digit1, int * digit2)
{
	short firstDigit = 0;
	short tens = 0;
	short secondDigit = 0;

	if (measurement < 0.0) measurement = 0.0f;
	if (measurement >= 100.0) measurement = 99.0f;

	firstDigit = (short)(measurement / 10.0f);
	
	if (firstDigit > 0.0f) 
	{
			*digit1 = firstDigit;
	}
	else 
	{
		*digit1 = 0;
	}

	tens = (short)firstDigit * 10;

	secondDigit = (short)measurement - tens;
	*digit2 = secondDigit;
}





//-----------------------------------------------------------------------------
// Wait
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
void Wait(unsigned int ms, short us)
{

   CKCON &= ~0x20;                     // use SYSCLK/12 as timebase

	if (us == 1)
		RCAP2 = -(SYSCLK/1000000/12);          
	else
	   RCAP2 = -(SYSCLK/1000/12);          // Timer 2 overflows at 1 kHz

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
// End Of File
//-----------------------------------------------------------------------------