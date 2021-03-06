#ifdef TEST
  #include "stub_io.h"
  #include "stub_interrupt.h"
#else
  #include <avr/io.h>
  #include <avr/interrupt.h>
#endif /* TEST */

#include <stdbool.h>
#include "i2c.h"

// 1, 2, 4, 8, 16, 32, 64, 128, or 256 bytes are allowed bank sizes
#define REGISTER_BANK_SIZE (16)
#define REGISTER_ADDRESS_MASK ( REGISTER_BANK_SIZE - 1 )

#if ( REGISTER_BANK_SIZE & REGISTER_BANK_MASK )
  #error Register bank size is not a power of 2
#endif

// ATtiny24/44/84-dependent definitions
#define DDR_I2C      DDRA
#define PORT_I2C     PORTA
#define PIN_I2C      PINA
#define PORT_I2C_SDA PA6
#define PORT_I2C_SCL PA4
#define PIN_I2C_SDA  PINA6

/*! Static Variables
 */
static uint8_t I2C_slaveAddress;
static volatile uint8_t registerAddress;
static volatile bool isRegisterAddress;
static volatile uint8_t registerBank[REGISTER_BANK_SIZE];
static uint8_t registerBankReadMask[REGISTER_BANK_SIZE];
static uint8_t registerBankWriteMask[REGISTER_BANK_SIZE];

/* Static functions */
static __inline__ void SET_USI_TO_SEND_ACK( void );
static __inline__ void SET_USI_TO_READ_ACK( void );
static __inline__ void SET_USI_TO_I2C_START_CONDITION_MODE( void );
static __inline__ void SET_USI_TO_SEND_DATA( void );
static __inline__ void SET_USI_TO_READ_DATA( void );

static volatile enum {
  i2c_StateNone,
  i2c_CHECK_ADDRESS,
  i2c_SEND_DATA,
  i2c_REQUEST_REPLY_FROM_SEND_DATA,
  i2c_CHECK_REPLY_FROM_SEND_DATA,
  i2c_REQUEST_DATA,
  i2c_GET_DATA_AND_SEND_ACK
} USI_I2C_Overflow_State;


void i2c_init( uint8_t I2C_ownAddress )
{
  I2C_slaveAddress = I2C_ownAddress;
  registerAddress = 0;
  
  PORT_I2C |=  (1<<PORT_I2C_SCL);                                 // Set SCL high
  PORT_I2C |=  (1<<PORT_I2C_SDA);                                 // Set SDA high
  DDR_I2C  |=  (1<<PORT_I2C_SCL);                                 // Set SCL as output
  DDR_I2C  &= ~(1<<PORT_I2C_SDA);                                 // Set SDA as input

  USICR    =  (1<<USISIE)|(0<<USIOIE)|                            // Enable Start Condition Interrupt. Disable Overflow Interrupt.
    (1<<USIWM1)|(1<<USIWM0)|                            // Set USI in Two-wire mode. No USI Counter overflow prior
    // to first Start Condition (potentail failure)
    (1<<USICS1)|(0<<USICS0)|(0<<USICLK)|                // Shift Register Clock Source = External, positive edge
    (0<<USITC);
  USISR    = 0xF0;                                                // Clear all flags and reset overflow counter
  USI_I2C_Overflow_State=i2c_StateNone; 
  
}

/* Usi start condition ISR
 * Detects the USI_I2C Start Condition and intialises the USI
 * for reception of the "I2C Address" packet.
 */

ISR(USI_START_vect)
{
  // Set default starting conditions for new I2C package
  USI_I2C_Overflow_State = i2c_CHECK_ADDRESS;
  isRegisterAddress = true;
  DDR_I2C  &= ~(1<<PORT_I2C_SDA);                                 // Set SDA as input
  
  while ( (PIN_I2C & (1<<PORT_I2C_SCL)) && (!(PIN_I2C & (1<<PORT_I2C_SDA))));
  if (!( PIN_I2C & ( 1 << PIN_I2C_SDA )))
    {
      // NO Stop
      USICR   = (1<<USISIE)|(1<<USIOIE)|                            // Enable Overflow and Start Condition Interrupt. (Keep StartCondInt to detect RESTART)
	(1<<USIWM0)|(1<<USIWM1)|                            // Set USI in Two-wire mode.
	(1<<USICS1)|(0<<USICS0)|(0<<USICLK)|                // Shift Register Clock Source = External, positive edge
	(0<<USITC);
    }
  else
    {     // STOP
      USICR   = (1<<USISIE)|(0<<USIOIE)|                            // Enable Overflow and Start Condition Interrupt. (Keep StartCondInt to detect RESTART)
	(1<<USIWM1)|(0<<USIWM1)|                            // Set USI in Two-wire mode.
	(1<<USICS1)|(0<<USICS0)|(0<<USICLK)|                // Shift Register Clock Source = External, positive edge
	(0<<USITC);
      
    }
  USISR  =    (1<<USISIF)|(1<<USIOIF)|(1<<USIPF)|(1<<USIDC)|      // Clear flags
    (0x0<<USICNT0);                                     // Set USI to sample 8 bits i.e. count 16 external pin toggles.    
  
}


/*! \brief USI counter overflow ISR
 * Handels all the comunication. Is disabled only when waiting
 * for new Start Condition.
 */
ISR(USI_OVF_vect)
{
  switch (USI_I2C_Overflow_State){
    
  case i2c_StateNone:
    // This should never happen, but this case prevents a warning
    // with -Wswitch
    break;
    
    // ---------- Address mode ----------
    // Check address and send ACK (and next i2c_SEND_DATA) if OK, else reset USI.
  case i2c_CHECK_ADDRESS:
    if ((USIDR == 0) || (( USIDR>>1 ) == I2C_slaveAddress)){
      if ( USIDR & 0x01 ){
	USI_I2C_Overflow_State = i2c_SEND_DATA;
      } else {
	USI_I2C_Overflow_State = i2c_REQUEST_DATA;
      }
      SET_USI_TO_SEND_ACK();
    } else {
      SET_USI_TO_I2C_START_CONDITION_MODE();          // not my Adress.....
    }
    break;
    
    // ----- Master write data mode ------
    // Check reply and goto i2c_SEND_DATA if OK, else reset USI.
  case i2c_CHECK_REPLY_FROM_SEND_DATA:
    if ( USIDR ){ // If NACK, the master does not want more data.
      SET_USI_TO_I2C_START_CONDITION_MODE();
      return;
    }
    // ** NOTICE NO BREAK HERE **
    // From here we just drop straight into i2c_SEND_DATA if the master sent an ACK
    
    // Copy data from buffer to USIDR and set USI to shift byte. Next i2c_REQUEST_REPLY_FROM_SEND_DATA
  case i2c_SEND_DATA:
    // Get data from Buffer
    USIDR = registerBank[registerAddress] & ~registerBankReadMask[registerAddress];
    registerAddress = (registerAddress + 1) & REGISTER_ADDRESS_MASK;
    
    USI_I2C_Overflow_State = i2c_REQUEST_REPLY_FROM_SEND_DATA;
    SET_USI_TO_SEND_DATA();
    break;

    // Set USI to sample reply from master. Next i2c_CHECK_REPLY_FROM_SEND_DATA
  case i2c_REQUEST_REPLY_FROM_SEND_DATA:
    USI_I2C_Overflow_State = i2c_CHECK_REPLY_FROM_SEND_DATA;
    SET_USI_TO_READ_ACK();
    break;
    
    // ----- Master read data mode ------
    // Set USI to sample data from master. Next i2c_GET_DATA_AND_SEND_ACK.
  case i2c_REQUEST_DATA:
    USI_I2C_Overflow_State = i2c_GET_DATA_AND_SEND_ACK;
    SET_USI_TO_READ_DATA();
    break;
    
    // Copy data from USIDR and send ACK. Next i2c_REQUEST_DATA
  case i2c_GET_DATA_AND_SEND_ACK:
    if(isRegisterAddress){
      registerAddress = (USIDR & REGISTER_ADDRESS_MASK);
      isRegisterAddress = false;
    } else {
      // Put data into Buffer
      registerBank[registerAddress] = USIDR & ~registerBankWriteMask[registerAddress];
      registerAddress = (registerAddress + 1) & REGISTER_ADDRESS_MASK;
    }
    USI_I2C_Overflow_State = i2c_REQUEST_DATA;
    SET_USI_TO_SEND_ACK();
    break;
  }
}

static __inline__ void
SET_USI_TO_SEND_ACK( void )
{
  USIDR    =  0;                                              /* Prepare ACK                        */
  DDR_I2C |=  (1<<PORT_I2C_SDA);                              /* Set SDA as output                  */
  USISR    =  (0<<USISIF)|(1<<USIOIF)|(1<<USIPF)|(1<<USIDC)|  /* Clear all flags, except Start Cond */
    (0x0E<<USICNT0);                                          /* set USI counter to shift 1 bit.    */
}

static __inline__ void
SET_USI_TO_READ_ACK( void )
{
  DDR_I2C &=  ~(1<<PORT_I2C_SDA);                             /* Set SDA as intput */
  USIDR    =  0;                                              /* Prepare ACK        */
  USISR    =  (0<<USISIF)|(1<<USIOIF)|(1<<USIPF)|(1<<USIDC)|  /* Clear all flags, except Start Cond  */
    (0x0E<<USICNT0);                                          /* set USI counter to shift 1 bit. */
}

static __inline__ void
SET_USI_TO_I2C_START_CONDITION_MODE( void )
{
  USICR    =  (1<<USISIE)|(0<<USIOIE)|                        /* Enable Start Condition Interrupt. Disable Overflow Interrupt.*/
    (1<<USIWM1)|(0<<USIWM0)|                        /* Set USI in Two-wire mode. No USI Counter overflow hold.      */
    (1<<USICS1)|(0<<USICS0)|(0<<USICLK)|            /* Shift Register Clock Source = External, positive edge        */
    (0<<USITC);
  USISR    =  (0<<USISIF)|(1<<USIOIF)|(1<<USIPF)|(1<<USIDC)|  /* Clear all flags, except Start Cond                            */
    (0x0<<USICNT0);
}

static __inline__ void
SET_USI_TO_SEND_DATA( void )
{
  DDR_I2C |=  (1<<PORT_I2C_SDA);                                  /* Set SDA as output                  */
  USISR    =  (0<<USISIF)|(1<<USIOIF)|(1<<USIPF)|(1<<USIDC)|      /* Clear all flags, except Start Cond */
    (0x0<<USICNT0);                                     /* set USI to shift out 8 bits        */
}

static __inline__ void
SET_USI_TO_READ_DATA( void )
{
  DDR_I2C &= ~(1<<PORT_I2C_SDA);                                  /* Set SDA as input                   */
  USISR    =  (0<<USISIF)|(1<<USIOIF)|(1<<USIPF)|(1<<USIDC)|      /* Clear all flags, except Start Cond */
    (0x0<<USICNT0);                                     /* set USI to shift out 8 bits        */
}

uint8_t
i2c_getRegister( const uint8_t reg )
{
  return registerBank[reg];
}

void
i2c_setRegister( const uint8_t reg, const uint8_t data )
{
  registerBank[reg] = data;
}

void
i2c_setRegisterReadWriteMasks( const uint8_t reg, const uint8_t rmask, const uint8_t wmask )
{
  registerBankReadMask[reg] = rmask;
  registerBankWriteMask[reg] = wmask;
}

