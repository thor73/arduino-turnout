
#include "DCCpacket.h"
#include "Bitstream.h"
#include <EEPROM.h>
#include <Servo.h>



//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Defines and structures
//
#define kDCC_INTERRUPT            0

typedef struct
{
    int count;
    byte validBytes;
    byte data[6];
} DCCPacket;

// The dcc decoder object and global data
//
int gPacketCount = 0;
int gIdlePacketCount = 0;
int gLongestPreamble = 0;
int gErrorCount = 0;
int gErrorLog[25];
int gBitErrorCount = 0;
int gBitErrorLog[25];

DCCPacket gPackets[25];

static unsigned long lastMillis = millis();


// Bitstream setup ==========================================================================

// global stuff
const byte intPin = 2;
BitStream bitStream(intPin, false, 48, 68, 88, 10000, 10);

boolean haveNewBits = false;
boolean currentBit = 0;
volatile unsigned long bits = 0;
byte numBits = 0;
int y = 0;


void BitStreamHandler(unsigned long incomingBits)
{
    noInterrupts();   // disable interrupts here, but shouldn't affect next dcc pulse, since this will be right after one
    bits = incomingBits;
    interrupts();

    haveNewBits = true;
}


void BitErrorHandler(byte errorCode)
{
    if (gBitErrorCount < 25) 
        gBitErrorLog[gBitErrorCount] = errorCode;
    gBitErrorCount++;
}




// dcc packet builder setup ============================================================

DCCpacket dccpacket(true, true, 250);

void MainPacketErrorHandler(byte errorCode)
{
    if (gErrorCount < 25) 
        gErrorLog[gErrorCount] = errorCode;
    gErrorCount++;
}

void RawPacketHandler(byte *packetBytes, byte byteCount)
{
    // Bump global packet count
    ++gPacketCount;

    // Walk table and look for a matching packet
    for ( int i = 0; i < (int)(sizeof(gPackets) / sizeof(gPackets[0])); ++i )
    {
        if ( gPackets[i].validBytes )
        {
            // Not an empty slot. Does this slot match this packet? If so, bump count.
            if ( gPackets[i].validBytes == byteCount )
            {
                char isPacket = true;
                for ( int j = 0; j < byteCount; j++)
                {
                    if ( gPackets[i].data[j] != packetBytes[j] )
                    {
                        isPacket = false;
                        break;
                    }
                }
                if ( isPacket )
                {
                    gPackets[i].count++;
                    return;
                }
            }
        }
        else
        {
            // Empty slot, just copy over data
            gPackets[i].count++;
            gPackets[i].validBytes = byteCount;
            for ( int j = 0; j < byteCount; j++)
            {
                gPackets[i].data[j] = packetBytes[j];
            }
            return;
        }
    }
}



// Min and max valid packet lengths
#define kPACKET_LEN_MIN               3
#define kPACKET_LEN_MAX               6

// Helper to make packet strings
char* MakePacketString(char* buffer60Bytes, byte byteCount, byte* packet)
{
    buffer60Bytes[0] = 0;
    if( byteCount>=kPACKET_LEN_MIN && byteCount<=kPACKET_LEN_MAX )
    {
        int i = 0;
        for(byte byt=0; byt<byteCount; ++byt)
        {
            byte bit=0x80;
            while(bit)
            {
                buffer60Bytes[i++] = (packet[byt] & bit) ? '1' : '0';
                bit=bit>>1;
            }
            buffer60Bytes[i++] = ' ';
        }
        buffer60Bytes[--i] = 0;
    }
    return buffer60Bytes;
}



void DumpAndResetTable()
{
    char buffer60Bytes[60];

    Serial.print("Total Packet Count: ");
    Serial.println(gPacketCount, DEC);

    Serial.print("Idle Packet Count:  ");
    Serial.println(gIdlePacketCount, DEC);

    Serial.print("Bit Error Count:  ");
    Serial.println(gBitErrorCount, DEC);

    Serial.print("Bit Errors:  ");
    int numErrors = (gBitErrorCount < 25) ? gBitErrorCount : 25;
    for (int i = 0; i < numErrors; i++)
    {
        Serial.print(gBitErrorLog[i], DEC);
        Serial.print(" ");
    }
    Serial.println();

    Serial.print("Packet Error Count:  ");
    Serial.println(gErrorCount, DEC);

    Serial.print("Packet Errors:  ");
    numErrors = (gErrorCount < 25) ? gErrorCount : 25;
    for (int i = 0; i < numErrors; i++)
    {
        Serial.print(gErrorLog[i], DEC);
        Serial.print(" ");
    }
    Serial.println();

    Serial.println("Count    Packet_Data");
    for ( int i = 0; i < (int)(sizeof(gPackets) / sizeof(gPackets[0])); ++i )
    {
        if ( gPackets[i].validBytes > 0 )
        {
            Serial.print(gPackets[i].count, DEC);
            if ( gPackets[i].count < 10 )
            {
                Serial.print("        ");
            } else {
                if ( gPackets[i].count < 100 )
                {
                    Serial.print("       ");
                } else {
                    Serial.print("      ");
                }
            }
            Serial.println( MakePacketString(buffer60Bytes, gPackets[i].validBytes, &gPackets[i].data[0]) );
        }
        gPackets[i].validBytes = 0;
        gPackets[i].count = 0;
    }
    Serial.println("============================================");

    gPacketCount = 0;
    gIdlePacketCount = 0;
    gLongestPreamble = 0;
    gErrorCount = 0;
    gBitErrorCount = 0;
}



// Setup  =================================================================
//
void setup()
{
    Serial.begin(115200);

    bitStream.SetDataFullHandler(&BitStreamHandler);
    bitStream.SetErrorHandler(&BitErrorHandler);

    dccpacket.SetPacketCompleteHandler(&RawPacketHandler);
    dccpacket.SetPacketErrorHandler(&MainPacketErrorHandler);

    pinMode(12, OUTPUT);
}



// Main loop   =======================================================================

void loop()
{
    unsigned long currentMillis = millis();

    if (haveNewBits)
    {
        dccpacket.ProcessIncomingBits(bits);
        haveNewBits = false;
    }

    if (currentMillis - lastMillis > 2000 )
    {
        DumpAndResetTable();
        lastMillis = millis();
    }
}
